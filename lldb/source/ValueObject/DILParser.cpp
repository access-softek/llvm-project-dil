//===-- DILParser.cpp -----------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// This implements the recursive descent parser for the Data Inspection
// Language (DIL), and its helper functions, which will eventually underlie the
// 'frame variable' command. The language that this parser recognizes is
// described in lldb/docs/dil-expr-lang.ebnf
//
//===----------------------------------------------------------------------===//

#include "lldb/ValueObject/DILParser.h"
#include "lldb/Target/ExecutionContextScope.h"
#include "lldb/ValueObject/DILAST.h"
#include "lldb/ValueObject/DILEval.h"
#include "lldb/lldb-enumerations.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/TargetParser/Host.h"
#include <cstdint>
#include <cstdlib>
#include <limits.h>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace {

const char* kValueIsNotConvertibleToBool =
    "value of type {0} is not contextually convertible to 'bool'";

template <typename T>
constexpr unsigned type_width() {
  return static_cast<unsigned>(sizeof(T)) * CHAR_BIT;
}

}  // namespace

namespace lldb_private::dil {

/// Quick lookup to check if a type name already exists in a
/// name-to-CompilerType map the DIL parser keeps of previously found
/// name/type pairs, for LLDB convenience var. -- look for 'persistent'
/// vars in Expressions directory.
static bool IsContextVar(const std::string &name) {
  return false;
}

static CompilerType GetBasicType(std::shared_ptr<ExecutionContextScope> ctx,
                                 lldb::BasicType basic_type) {
  static std::unordered_map<lldb::BasicType, CompilerType> basic_types;
  auto type = basic_types.find(basic_type);
  if (type != basic_types.end()) {
    std::string type_name((type->second).GetTypeName().AsCString());
    // Only return the found type if it's valid.
    if (type_name != "<invalid>")
      return type->second;
  }

  lldb::TargetSP target_sp = ctx->CalculateTarget();
  if (target_sp) {
    for (auto type_system_sp : target_sp->GetScratchTypeSystems())
      if (auto compiler_type = type_system_sp->GetBasicTypeFromAST(basic_type)){
        basic_types.insert({basic_type, compiler_type});
        return compiler_type;
      }
  }
  CompilerType empty_type;
  return empty_type;
}

static std::unique_ptr<BuiltinFunctionDef> GetBuiltinFunctionDef(
    std::shared_ptr<ExecutionContextScope> ctx, const std::string& identifier) {
  //
  // __log2(unsigned int x) -> unsigned int
  //
  //   Calculates the log2(x).
  //
  if (identifier == "__log2") {
    CompilerType return_type = GetBasicType(ctx, lldb::eBasicTypeUnsignedInt);
    std::vector<CompilerType> arguments = {
      GetBasicType(ctx, lldb::eBasicTypeUnsignedInt),
    };
    return std::make_unique<BuiltinFunctionDef>(identifier, return_type,
                                                std::move(arguments));
  }
  //
  // __findnonnull(T* ptr, long long buffer_size) -> int
  //
  //   Finds the first non-null object pointed by `ptr`. `ptr` is treated as an
  //   array of pointers of size `buffer_size`.
  //
  if (identifier == "__findnonnull") {
    auto return_type = GetBasicType(ctx, lldb::eBasicTypeInt);
    std::vector<CompilerType> arguments = {
        // The first argument should actually be "T*", but we don't support
        // templates here.
        // HACK: Void means "any" and we'll check in runtime. The argument will
        // be passed as is without any conversions.
      GetBasicType(ctx, lldb::eBasicTypeVoid),
      GetBasicType(ctx,lldb::eBasicTypeLongLong),
    };
    return std::make_unique<BuiltinFunctionDef>(identifier, return_type,
                                                std::move(arguments));
  }
  // Not a builtin function.
  return nullptr;
}

static bool TokenEndsTemplateArgumentList(const Token& token) {
  // Note: in C++11 ">>" can be treated as "> >" and thus be a valid token
  // for the template argument list.
  return token.IsOneOf({Token::comma, Token::greater, Token::greatergreater});
}

static ASTNodeUP InsertArrayToPointerConversion(ASTNodeUP expr) {
  assert(expr->GetDereferencedResultType().IsArrayType() &&
         "an argument to array-to-pointer conversion must be an array");

  // TODO: Make this an explicit array-to-pointer conversion instead of
  // using a "generic" CStyleCastNode.
  return std::make_unique<CStyleCastNode>(
      expr->GetLocation(),
      expr->GetDereferencedResultType().GetArrayElementType(nullptr).GetPointerType(),
      std::move(expr), TypePromotionCastKind::ePointer);
}

static CompilerType DoIntegralPromotion(
    std::shared_ptr<ExecutionContextScope> ctx, CompilerType from) {
  assert((from.IsInteger() || from.IsUnscopedEnumerationType()) &&
         "Integral promotion works only for integers and unscoped enums.");

  // Don't do anything if the type doesn't need to be promoted.
  if (!from.IsPromotableIntegerType()) {
    return from;
  }

  if (from.IsUnscopedEnumerationType()) {
    // Get the enumeration underlying type and promote it.
    return DoIntegralPromotion(ctx, from.GetEnumerationIntegerType());
  }

  // At this point the type should an integer.
  assert(from.IsInteger() && "invalid type: must be an integer");

  // Get the underlying builtin representation.
  lldb::BasicType builtin_type =
      from.GetCanonicalType().GetBasicTypeEnumeration();

  uint64_t from_size = 0;
  if (builtin_type == lldb::eBasicTypeWChar ||
      builtin_type == lldb::eBasicTypeSignedWChar ||
      builtin_type == lldb::eBasicTypeUnsignedWChar ||
      builtin_type == lldb::eBasicTypeChar16 ||
      builtin_type == lldb::eBasicTypeChar32) {
    // Find the type that can hold the entire range of values for our type.
    bool is_signed = from.IsSigned();
    if (auto temp = from.GetByteSize(ctx.get()))
      from_size = temp.value();

    CompilerType promote_types[] = {
      GetBasicType(ctx, lldb::eBasicTypeInt),
      GetBasicType(ctx, lldb::eBasicTypeUnsignedInt),
      GetBasicType(ctx, lldb::eBasicTypeLong),
      GetBasicType(ctx, lldb::eBasicTypeUnsignedLong),
      GetBasicType(ctx, lldb::eBasicTypeLongLong),
      GetBasicType(ctx, lldb::eBasicTypeUnsignedLongLong),
    };
    for (auto& type : promote_types) {
      uint64_t byte_size = 0;
      if (auto temp = type.GetByteSize(ctx.get()))
        byte_size = temp.value();
      if (from_size < byte_size ||
          (from_size == byte_size &&
           is_signed ==(bool)(
               type.GetTypeInfo() & lldb::eTypeIsSigned)))
      {
        return type;
      }
    }

    llvm_unreachable("char type should fit into long long");
  }

  // Here we can promote only to "int" or "unsigned int".
  CompilerType int_type = GetBasicType(ctx, lldb::eBasicTypeInt);
  uint64_t int_byte_size = 0;
  if (auto temp = int_type.GetByteSize(ctx.get()))
    int_byte_size = temp.value();

  // Signed integer types can be safely promoted to "int".
  if (from.IsSigned()) {
    return int_type;
  }
  // Unsigned integer types are promoted to "unsigned int" if "int" cannot hold
  // their entire value range.
  return (from_size == int_byte_size)
      ? GetBasicType(ctx, lldb::eBasicTypeUnsignedInt)
      : int_type;
}

static ASTNodeUP
UsualUnaryConversions(std::shared_ptr<ExecutionContextScope> ctx,
                      ASTNodeUP expr) {
  // Perform usual conversions for unary operators. At the moment this includes
  // array-to-pointer and the integral promotion for eligible types.
  auto result_type = expr->GetDereferencedResultType();

  if (expr->is_bitfield()) {
    // Promote bitfields. If `int` can represent the bitfield value, it is
    // converted to `int`. Otherwise, if `unsigned int` can represent it, it
    // is converted to `unsigned int`. Otherwise, it is treated as its
    // underlying type.

    uint32_t bitfield_size = expr->bitfield_size();
    // Some bitfields have undefined size (e.g. result of ternary operation).
    // The AST's `bitfield_size` of those is 0, and no promotion takes place.
    if (bitfield_size > 0 && result_type.IsInteger()) {
      auto int_type = GetBasicType(ctx, lldb::eBasicTypeInt);
      auto uint_type = GetBasicType(ctx, lldb::eBasicTypeUnsignedInt);
      uint64_t int_byte_size = 0;
      uint64_t uint_byte_size = 0;
      if (auto temp = int_type.GetByteSize(ctx.get()))
        int_byte_size = temp.value();
      if (auto temp = uint_type.GetByteSize(ctx.get()))
        uint_byte_size = temp.value();
      uint32_t int_bit_size = int_byte_size * CHAR_BIT;
      if (bitfield_size < int_bit_size ||
          (result_type.IsSigned() && bitfield_size == int_bit_size)) {
        expr = std::make_unique<CStyleCastNode>(expr->GetLocation(), int_type,
                                                std::move(expr),
                                                TypePromotionCastKind::eArithmetic);
      } else if (bitfield_size <= uint_byte_size * CHAR_BIT) {
        expr = std::make_unique<CStyleCastNode>(expr->GetLocation(), uint_type,
                                                std::move(expr),
                                                TypePromotionCastKind::eArithmetic);
      }
    }
  }

  if (result_type.IsArrayType()) {
    expr = InsertArrayToPointerConversion(std::move(expr));
  }

  if (result_type.IsInteger() || result_type.IsUnscopedEnumerationType()) {
    auto promoted_type = DoIntegralPromotion(ctx, result_type);

    // Insert a cast if the type promotion is happening.
    // TODO: Make this an implicit static_cast.
    if (!promoted_type.CompareTypes(result_type)) {
      expr = std::make_unique<CStyleCastNode>(expr->GetLocation(), promoted_type,
                                              std::move(expr),
                                              TypePromotionCastKind::eArithmetic);
    }
  }

  return expr;
}

static size_t ConversionRank(CompilerType type) {
  // Get integer conversion rank
  // https://eel.is/c++draft/conv.rank
  switch (type.GetCanonicalType().GetBasicTypeEnumeration()) {
    case lldb::eBasicTypeBool:
      return 1;
    case lldb::eBasicTypeChar:
    case lldb::eBasicTypeSignedChar:
    case lldb::eBasicTypeUnsignedChar:
      return 2;
    case lldb::eBasicTypeShort:
    case lldb::eBasicTypeUnsignedShort:
      return 3;
    case lldb::eBasicTypeInt:
    case lldb::eBasicTypeUnsignedInt:
      return 4;
    case lldb::eBasicTypeLong:
    case lldb::eBasicTypeUnsignedLong:
      return 5;
    case lldb::eBasicTypeLongLong:
    case lldb::eBasicTypeUnsignedLongLong:
      return 6;

      // TODO: The ranks of char16_t, char32_t, and wchar_t are equal to the
      // ranks of their underlying types.
    case lldb::eBasicTypeWChar:
    case lldb::eBasicTypeSignedWChar:
    case lldb::eBasicTypeUnsignedWChar:
      return 3;
    case lldb::eBasicTypeChar16:
      return 3;
    case lldb::eBasicTypeChar32:
      return 4;

    default:
      break;
  }
  return 0;
}

static lldb::BasicType BasicTypeToUnsigned(lldb::BasicType basic_type) {
  switch (basic_type) {
    case lldb::eBasicTypeInt:
      return lldb::eBasicTypeUnsignedInt;
    case lldb::eBasicTypeLong:
      return lldb::eBasicTypeUnsignedLong;
    case lldb::eBasicTypeLongLong:
      return lldb::eBasicTypeUnsignedLongLong;
    default:
      return basic_type;
  }
}

static void
PerformIntegerConversions(std::shared_ptr<ExecutionContextScope> ctx,
                          ASTNodeUP &l, ASTNodeUP &r, bool convert_lhs,
                          bool convert_rhs) {
  // Assert that rank(l) < rank(r).
  auto l_type = l->GetDereferencedResultType();
  auto r_type = r->GetDereferencedResultType();

  // if `r` is signed and `l` is unsigned, check whether it can represent all
  // of the values of the type of the `l`. If not, then promote `r` to the
  // unsigned version of its type.
  if (r_type.IsSigned() && !l_type.IsSigned()) {
    uint64_t l_size = 0;
    uint64_t r_size = 0;
    if (auto temp = l_type.GetByteSize(ctx.get()))
      l_size = temp.value();;
    if (auto temp = r_type.GetByteSize(ctx.get()))
      r_size = temp.value();

    assert(l_size <= r_size && "left value must not be larger then the right!");

    if (r_size == l_size) {
      auto r_type_unsigned = GetBasicType(
          ctx,
          BasicTypeToUnsigned(r_type.GetCanonicalType()
                                  .GetBasicTypeEnumeration()));
      if (convert_rhs) {
        r = std::make_unique<CStyleCastNode>(r->GetLocation(), r_type_unsigned,
                                             std::move(r),
                                             TypePromotionCastKind::eArithmetic);
      }
    }
  }

  if (convert_lhs) {
    l = std::make_unique<CStyleCastNode>(l->GetLocation(), r->result_type(),
                                         std::move(l),
                                         TypePromotionCastKind::eArithmetic);
  }
}

static CompilerType
UsualArithmeticConversions(std::shared_ptr<ExecutionContextScope> ctx,
                           ASTNodeUP &lhs, ASTNodeUP &rhs,
                           bool is_comp_assign = false) {
  // Apply unary conversions (e.g. intergal promotion) for both operands.
  // In case of a composite assignment operator LHS shouldn't get promoted.
  if (!is_comp_assign) {
    lhs = UsualUnaryConversions(ctx, std::move(lhs));
  }
  rhs = UsualUnaryConversions(ctx, std::move(rhs));

  auto lhs_type = lhs->GetDereferencedResultType();
  auto rhs_type = rhs->GetDereferencedResultType();

  if (lhs_type.CompareTypes(rhs_type)) {
    return lhs_type;
  }

  // If either of the operands is not arithmetic (e.g. pointer), we're done.
  if (!lhs_type.IsScalarType() || !rhs_type.IsScalarType()) {
    CompilerType bad_type;
    return bad_type;
  }

  // Handle conversions for floating types (float, double).
  if (lhs_type.IsFloat() || rhs_type.IsFloat()) {
    // If both are floats, convert the smaller operand to the bigger.
    if (lhs_type.IsFloat() && rhs_type.IsFloat()) {
      int order = lhs_type.GetBasicTypeEnumeration() -
                  rhs_type.GetBasicTypeEnumeration();
      if (order > 0) {
        rhs = std::make_unique<CStyleCastNode>(rhs->GetLocation(), lhs_type,
                                               std::move(rhs),
                                               TypePromotionCastKind::eArithmetic);
        return lhs_type;
      }
      assert(order < 0 && "illegal operands: must not be of the same type");
      if (!is_comp_assign) {
        lhs = std::make_unique<CStyleCastNode>(lhs->GetLocation(), rhs_type,
                                               std::move(lhs),
                                               TypePromotionCastKind::eArithmetic);
      }
      return rhs_type;
    }

    if (lhs_type.IsFloat()) {
      assert(rhs_type.IsInteger() && "illegal operand: must be an integer");
      rhs = std::make_unique<CStyleCastNode>(rhs->GetLocation(), lhs_type,
                                             std::move(rhs),
                                             TypePromotionCastKind::eArithmetic);
      return lhs_type;
    }
    assert(rhs_type.IsFloat() && "illegal operand: must be a float");
    if (!is_comp_assign) {
      lhs = std::make_unique<CStyleCastNode>(lhs->GetLocation(), rhs_type,
                                             std::move(lhs),
                                             TypePromotionCastKind::eArithmetic);
    }
    return rhs_type;
  }

  // Handle conversion for integer types.
  assert((lhs_type.IsInteger() && rhs_type.IsInteger()) &&
         "illegal operands: must be both integers");

  using Rank = std::tuple<size_t, bool>;
  Rank l_rank = {ConversionRank(lhs_type), !lhs_type.IsSigned()};
  Rank r_rank = {ConversionRank(rhs_type), !rhs_type.IsSigned()};

  if (l_rank < r_rank) {
    PerformIntegerConversions(ctx, lhs, rhs, !is_comp_assign, true);
  } else if (l_rank > r_rank) {
    PerformIntegerConversions(ctx, rhs, lhs, true, !is_comp_assign);
  }

  if (!is_comp_assign) {
    assert(lhs->GetDereferencedResultType().GetCanonicalType().CompareTypes(
               rhs->GetDereferencedResultType().GetCanonicalType()) &&
           "integral promotion error: operands result types must be the same");
  }

  return lhs->GetDereferencedResultType().GetCanonicalType();
}

static TypeDeclaration::TypeSpecifier ToTypeSpecifier(Token::Kind kind) {
  using TypeSpecifier = TypeDeclaration::TypeSpecifier;
  switch (kind) {
    case Token::kw_void:     return TypeSpecifier::kVoid;
    case Token::kw_bool:     return TypeSpecifier::kBool;
    case Token::kw_char:     return TypeSpecifier::kChar;
    case Token::kw_short:    return TypeSpecifier::kShort;
    case Token::kw_int:      return TypeSpecifier::kInt;
    case Token::kw_long:     return TypeSpecifier::kLong;
    case Token::kw_float:    return TypeSpecifier::kFloat;
    case Token::kw_double:   return TypeSpecifier::kDouble;
    case Token::kw_wchar_t:  return TypeSpecifier::kWChar;
    case Token::kw_char16_t: return TypeSpecifier::kChar16;
    case Token::kw_char32_t: return TypeSpecifier::kChar32;
    default:
      assert(false && "invalid type specifier token");
      return TypeSpecifier::kUnknown;
  }
}

std::tuple<lldb::BasicType, bool> PickIntegerType(
    std::shared_ptr<ExecutionContextScope> ctx,
    const dil::NumericLiteralParser& literal,
    const llvm::APInt& value) {
  uint64_t int_byte_size = 0;
  uint64_t long_byte_size = 0;
  uint64_t long_long_byte_size = 0;
  if (auto temp = GetBasicType(ctx, lldb::eBasicTypeInt).GetByteSize(nullptr))
    int_byte_size = temp.value();

  if (auto temp = GetBasicType(ctx, lldb::eBasicTypeLong).GetByteSize(nullptr))
    long_byte_size = temp.value();

  if (auto temp = GetBasicType(ctx,
                               lldb::eBasicTypeLongLong).GetByteSize(nullptr))
    long_long_byte_size = temp.value();

  unsigned int_size = int_byte_size * CHAR_BIT;
  unsigned long_size = long_byte_size * CHAR_BIT;
  unsigned long_long_size = long_long_byte_size * CHAR_BIT;


  // Binary, Octal, Hexadecimal and literals with a U suffix are allowed to be
  // an unsigned integer.
  bool unsigned_is_allowed = literal.isUnsigned || literal.getRadix() != 10;

  // Try int/unsigned int.
  if (!literal.isLong && !literal.isLongLong && value.isIntN(int_size)) {
    if (!literal.isUnsigned && value.isIntN(int_size - 1)) {
      return {lldb::eBasicTypeInt, false};
    }
    if (unsigned_is_allowed) {
      return {lldb::eBasicTypeUnsignedInt, true};
    }
  }
  // Try long/unsigned long.
  if (!literal.isLongLong && value.isIntN(long_size)) {
    if (!literal.isUnsigned && value.isIntN(long_size - 1)) {
      return {lldb::eBasicTypeLong, false};
    }
    if (unsigned_is_allowed) {
      return {lldb::eBasicTypeUnsignedLong, true};
    }
  }
  // Try long long/unsigned long long.
  if (value.isIntN(long_long_size)) {
    if (!literal.isUnsigned && value.isIntN(long_long_size - 1)) {
      return {lldb::eBasicTypeLongLong, false};
    }
    if (unsigned_is_allowed) {
      return {lldb::eBasicTypeUnsignedLongLong, true};
    }
  }

  // If we still couldn't decide a type, we probably have something that does
  // not fit in a signed long long, but has no U suffix. Also known as:
  //
  //  warning: integer literal is too large to be represented in a signed
  //  integer type, interpreting as unsigned [-Wimplicitly-unsigned-literal]
  //
  return {lldb::eBasicTypeUnsignedLongLong, true};
}

lldb::BasicType PickCharType(const dil::CharLiteralParser& literal) {
  if (literal.isMultiChar()) {
    return lldb::eBasicTypeInt;
#if LLVM_VERSION_MAJOR < 15
  } else if (literal.isAscii()) {
#else
  } else if (literal.isOrdinary()) {
#endif
    return lldb::eBasicTypeChar;
  } else if (literal.isWide()) {
    return lldb::eBasicTypeWChar;
  } else if (literal.isUTF8()) {
    // TODO: Change to eBasicTypeChar8 when support for u8 is added
    return lldb::eBasicTypeChar;
  }
  return lldb::eBasicTypeChar;
}

lldb::BasicType PickCharType(const dil::StringLiteralParser& literal) {
#if LLVM_VERSION_MAJOR < 15
  if (literal.isAscii()) {
#else
  if (literal.isOrdinary()) {
#endif
    return lldb::eBasicTypeChar;
  } else if (literal.isWide()) {
    return lldb::eBasicTypeWChar;
  } else if (literal.isUTF8()) {
    // TODO: Change to eBasicTypeChar8 when support for u8 is added.
    return lldb::eBasicTypeChar;
  }
  return lldb::eBasicTypeChar;
}

std::string FormatDiagnostics(llvm::StringRef text, const std::string &message,
                              uint32_t loc) {
  // Get the position, in the current line of text, of the diagnostics pointer.
  // ('loc' is the location of the start of the current token/error within the
  // overall text line).
  int32_t arrow = loc + 1; // Column offset starts at 1, not 0.

  return llvm::formatv("<expr:1:{0}>: {1}\n{2}\n{3}", loc + 1, message,
                       llvm::fmt_pad(text, 0, 0),
                       llvm::fmt_pad("^", arrow - 1, 0));
}

llvm::Expected<ASTNodeUP>
DILParser::Parse(llvm::StringRef dil_input_expr, DILLexer lexer,
                 std::shared_ptr<StackFrame> frame_sp,
                 lldb::DynamicValueType use_dynamic, bool use_synthetic,
                 bool fragile_ivar, bool check_ptr_vs_member) {
  Status error;
  DILParser parser(dil_input_expr, lexer, frame_sp, use_dynamic, use_synthetic,
                   fragile_ivar, check_ptr_vs_member, error);
  return parser.Run();
}

DILParser::DILParser(llvm::StringRef dil_input_expr, DILLexer lexer,
                     std::shared_ptr<StackFrame> frame_sp,
                     lldb::DynamicValueType use_dynamic, bool use_synthetic,
                     bool fragile_ivar, bool check_ptr_vs_member, Status &error)
    : m_ctx_scope(frame_sp), m_input_expr(dil_input_expr), m_dil_lexer(lexer),
      m_error(error), m_use_dynamic(use_dynamic),
      m_use_synthetic(use_synthetic), m_fragile_ivar(fragile_ivar),
      m_check_ptr_vs_member(check_ptr_vs_member) {}

llvm::Expected<ASTNodeUP> DILParser::Run() {
  ASTNodeUP expr;

  if (m_dil_lexer.IsStringLiteral(CurToken().GetKind()) &&
      m_dil_lexer.LookAhead(1).Is(Token::eof)) {
    // A special case to handle a single string-literal token.
    expr = ParseStringLiteral();
  } else {
    expr = ParseExpression();
  }

  Expect(Token::eof);

  // Check for any parsing errors.
  if (m_error.Fail())
    return m_error.ToError();

  return expr;
}

CompilerType DILParser::ResolveTypeDeclarators(
    CompilerType type, const std::vector<PtrOperator>& ptr_operators)
{
  CompilerType bad_type;
  // Resolve pointers/references.
  for (auto& [tk, loc] : ptr_operators) {
    if (tk == Token::star) {
      // Pointers to reference types are forbidden.
      if (type.IsReferenceType()) {
        BailOut(ErrorCode::kInvalidOperandType,
                llvm::formatv("'type name' declared as a pointer to a "
                              "reference of type {0}",
                              type.TypeDescription()),
                loc);
        return bad_type;
      }
      // Get pointer type for the base type: e.g. int* -> int**.
      type = type.GetPointerType();

    } else if (tk == Token::amp) {
      // References to references are forbidden.
      if (type.IsReferenceType()) {
        BailOut(ErrorCode::kInvalidOperandType,
                "type name declared as a reference to a reference", loc);
        return bad_type;
      }
      // Get reference type for the base type: e.g. int -> int&.
      type = type.GetLValueReferenceType();
    }
  }

  return type;
}

bool DILParser::IsSimpleTypeSpecifierKeyword(Token token) const {
  return token.IsOneOf({Token::kw_char, Token::kw_char16_t, Token::kw_char32_t,
                        Token::kw_wchar_t, Token::kw_bool, Token::kw_short,
                        Token::kw_int, Token::kw_long, Token::kw_signed,
                        Token::kw_unsigned, Token::kw_float, Token::kw_double,
                        Token::kw_void});
}

bool DILParser::IsCvQualifier(Token token) const {
  return token.IsOneOf({Token::kw_const, Token::kw_volatile});
}

bool DILParser::IsPtrOperator(Token token) const {
  return token.IsOneOf({Token::star, Token::amp});
}

bool DILParser::HandleSimpleTypeSpecifier(TypeDeclaration* type_decl) {
  using TypeSpecifier = TypeDeclaration::TypeSpecifier;
  using SignSpecifier = TypeDeclaration::SignSpecifier;

  TypeSpecifier type_spec = type_decl->m_type_specifier;
  uint32_t loc = CurToken().GetLocation();
  Token::Kind kind = CurToken().GetKind();

  switch (kind) {
    case Token::kw_int: {
      // "int" can have signedness and be combined with "short", "long" and
      // "long long" (but not with another "int").
      if (type_decl->m_has_int_specifier) {
        BailOut(ErrorCode::kInvalidOperandType,
                "cannot combine with previous 'int' declaration specifier",
                loc);
        return false;
      }
      if (type_spec == TypeSpecifier::kShort ||
          type_spec == TypeSpecifier::kLong ||
          type_spec == TypeSpecifier::kLongLong) {
        type_decl->m_has_int_specifier = true;
        return true;
      } else if (type_spec == TypeSpecifier::kUnknown) {
        type_decl->m_type_specifier = TypeSpecifier::kInt;
        type_decl->m_has_int_specifier = true;
        return true;
      }
      BailOut(ErrorCode::kInvalidOperandType,
              llvm::formatv(
                  "cannot combine with previous '{0}' declaration specifier",
                  type_spec),
              loc);
      return false;
    }

    case Token::kw_long: {
      // "long" can have signedness and be combined with "int" or "long" to
      // form "long long".
      if (type_spec == TypeSpecifier::kUnknown ||
          type_spec == TypeSpecifier::kInt) {
        type_decl->m_type_specifier = TypeSpecifier::kLong;
        return true;
      } else if (type_spec == TypeSpecifier::kLong) {
        type_decl->m_type_specifier = TypeSpecifier::kLongLong;
        return true;
      } else if (type_spec == TypeSpecifier::kDouble) {
        type_decl->m_type_specifier = TypeSpecifier::kLongDouble;
        return true;
      }
      BailOut(ErrorCode::kInvalidOperandType,
              llvm::formatv(
                  "cannot combine with previous '{0}' declaration specifier",
                  type_spec),
              loc);
      return false;
    }

    case Token::kw_short: {
      // "short" can have signedness and be combined with "int".
      if (type_spec == TypeSpecifier::kUnknown ||
          type_spec == TypeSpecifier::kInt) {
        type_decl->m_type_specifier = TypeSpecifier::kShort;
        return true;
      }
      BailOut(ErrorCode::kInvalidOperandType,
              llvm::formatv(
                  "cannot combine with previous '{0}' declaration specifier",
                  type_spec),
              loc);
      return false;
    }

    case Token::kw_char: {
      // "char" can have signedness, but it cannot be combined with any other
      // type specifier.
      if (type_spec == TypeSpecifier::kUnknown) {
        type_decl->m_type_specifier = TypeSpecifier::kChar;
        return true;
      }
      BailOut(ErrorCode::kInvalidOperandType,
              llvm::formatv(
                  "cannot combine with previous '{0}' declaration specifier",
                  type_spec),
              loc);
      return false;
    }

    case Token::kw_double: {
      // "double" can be combined with "long" to form "long double", but it
      // cannot be combined with signedness specifier.
      if (type_decl->m_sign_specifier != SignSpecifier::kUnknown) {
        BailOut(ErrorCode::kInvalidOperandType,
                "'double' cannot be signed or unsigned", loc);
        return false;
      }
      if (type_spec == TypeSpecifier::kUnknown) {
        type_decl->m_type_specifier = TypeSpecifier::kDouble;
        return true;
      } else if (type_spec == TypeSpecifier::kLong) {
        type_decl->m_type_specifier = TypeSpecifier::kLongDouble;
        return true;
      }
      BailOut(ErrorCode::kInvalidOperandType,
              llvm::formatv(
                  "cannot combine with previous '{0}' declaration specifier",
                  type_spec),
              loc);
      return false;
    }

    case Token::kw_bool:
    case Token::kw_void:
    case Token::kw_float:
    case Token::kw_wchar_t:
    case Token::kw_char16_t:
    case Token::kw_char32_t: {
      // These types cannot have signedness or be combined with any other type
      // specifiers.
      if (type_decl->m_sign_specifier != SignSpecifier::kUnknown) {
        BailOut(ErrorCode::kInvalidOperandType,
                llvm::formatv("'{0}' cannot be signed or unsigned",
                              ToTypeSpecifier(kind)),
                loc);
        return false;
      }
      if (type_spec != TypeSpecifier::kUnknown) {
        BailOut(ErrorCode::kInvalidOperandType,
                llvm::formatv(
                    "cannot combine with previous '{0}' declaration specifier",
                    type_spec),
                loc);
      }
      type_decl->m_type_specifier = ToTypeSpecifier(kind);
      return true;
    }

    case Token::kw_signed:
    case Token::kw_unsigned: {
      // "signed" and "unsigned" cannot be combined with another signedness
      // specifier.
      if (type_decl->m_sign_specifier != SignSpecifier::kUnknown) {
        BailOut(ErrorCode::kInvalidOperandType,
                llvm::formatv(
                    "cannot combine with previous '{0}' declaration specifier",
                    type_decl->m_sign_specifier),
                loc);
        return false;
      }
      if (type_spec == TypeSpecifier::kVoid ||
          type_spec == TypeSpecifier::kBool ||
          type_spec == TypeSpecifier::kFloat ||
          type_spec == TypeSpecifier::kDouble ||
          type_spec == TypeSpecifier::kLongDouble ||
          type_spec == TypeSpecifier::kWChar ||
          type_spec == TypeSpecifier::kChar16 ||
          type_spec == TypeSpecifier::kChar32) {
        BailOut(ErrorCode::kInvalidOperandType,
                llvm::formatv("'{0}' cannot be signed or unsigned", type_spec),
                loc);
        return false;
      }

      type_decl->m_sign_specifier = (kind == Token::kw_signed)
                                       ? SignSpecifier::kSigned
                                       : SignSpecifier::kUnsigned;
      return true;
    }

    default:
      assert(false && "invalid simple type specifier kind");
      return false;
  }
}

ASTNodeUP DILParser::ParseStringLiteral() {
  ExpectOneOf(std::vector<Token::Kind>{Token::string_literal,
                                       Token::wide_string_literal,
                                       Token::utf8_string_literal});
  uint32_t loc = CurToken().GetLocation();

  // TODO: Support parsing of joined string-literals (e.g. "abc" "def").
  // Currently, only a single token can be parsed into a string.
  dil::StringLiteralParser string_literal(llvm::ArrayRef<Token>(CurToken()),
                                          m_dil_lexer);

  if (string_literal.hadError) {
    // TODO: Use ErrorCode::kInvalidStringLiteral in the future.
    BailOut(ErrorCode::kInvalidNumericLiteral,
            llvm::formatv("Failed to parse token as string-literal: {0}",
                          CurToken()),
            loc);
    return std::make_unique<ErrorNode>();
  }

  auto char_type = GetBasicType(m_ctx_scope, PickCharType(string_literal));
  // Strings are terminated by a null value (add +1).
  CompilerType compiler_type = char_type;
  uint64_t byte_size = 0;
  if (auto temp = compiler_type.GetByteSize(nullptr))
    byte_size = temp.value();
  uint64_t array_size = string_literal.GetStringLength() / byte_size + 1;
  auto array_type = compiler_type.GetArrayType(array_size);

  llvm::StringRef value = string_literal.GetString();
  std::string data(value.data());

  assert(data.size() == array_type.GetByteSize(nullptr) &&
         "invalid string literal: unexpected data size");
  m_dil_lexer.Advance();
  return std::make_unique<StringLiteralNode>(loc, array_type, std::move(data));
}

// Parse an expression.
//
//  expression:
//    assignment_expression
//
ASTNodeUP DILParser::ParseExpression() { return ParseAssignmentExpression(); }

// Parse an assignment_expression.
//
//  assignment_expression:
//    conditional_expression
//    logical_or_expression assignment_operator assignment_expression
//
//  assignment_operator:
//    "="
//    "*="
//    "/="
//    "%="
//    "+="
//    "-="
//    ">>="
//    "<<="
//    "&="
//    "^="
//    "|="
//
//  conditional_expression:
//    logical_or_expression
//    logical_or_expression "?" expression ":" assignment_expression
//
ASTNodeUP DILParser::ParseAssignmentExpression() {
  auto lhs = ParseLogicalOrExpression();

  // Check if it's an assignment expression.
  if (CurToken().IsOneOf({Token::equal, Token::starequal, Token::slashequal,
                          Token::percentequal, Token::plusequal,
                          Token::minusequal, Token::greatergreaterequal,
                          Token::lesslessequal, Token::ampequal,
                          Token::caretequal, Token::pipeequal})) {
    // That's an assignment!
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseAssignmentExpression();
    lhs = BuildBinaryOp(dil_token_kind_to_binary_op_kind(token.GetKind()),
                        std::move(lhs), std::move(rhs), token.GetLocation());
  }

  // Check if it's a conditional expression.
  if (CurToken().Is(Token::question)) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto true_val = ParseExpression();
    Expect(Token::colon);
    m_dil_lexer.Advance();
    auto false_val = ParseAssignmentExpression();
    lhs = BuildTernaryOp(std::move(lhs), std::move(true_val),
                         std::move(false_val), token.GetLocation());
  }

  return lhs;
}

// Parse a logical_or_expression.
//
//  logical_or_expression:
//    logical_and_expression {"||" logical_and_expression}
//
ASTNodeUP DILParser::ParseLogicalOrExpression() {
  auto lhs = ParseLogicalAndExpression();

  while (CurToken().Is(Token::pipepipe)) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseLogicalAndExpression();
    lhs = BuildBinaryOp(BinaryOpKind::LOr, std::move(lhs), std::move(rhs),
                        token.GetLocation());
  }

  return lhs;
}

// Parse a logical_and_expression.
//
//  logical_and_expression:
//    inclusive_or_expression {"&&" inclusive_or_expression}
//
ASTNodeUP DILParser::ParseLogicalAndExpression() {
  auto lhs = ParseInclusiveOrExpression();

  while (CurToken().Is(Token::ampamp)) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseInclusiveOrExpression();
    lhs = BuildBinaryOp(BinaryOpKind::LAnd, std::move(lhs), std::move(rhs),
                        token.GetLocation());
  }

  return lhs;
}

// Parse an inclusive_or_expression.
//
//  inclusive_or_expression:
//    exclusive_or_expression {"|" exclusive_or_expression}
//
ASTNodeUP DILParser::ParseInclusiveOrExpression() {
  auto lhs = ParseExclusiveOrExpression();

  while (CurToken().Is(Token::pipe)) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseExclusiveOrExpression();
    lhs = BuildBinaryOp(BinaryOpKind::Or, std::move(lhs), std::move(rhs),
                        token.GetLocation());
  }

  return lhs;
}

// Parse an exclusive_or_expression.
//
//  exclusive_or_expression:
//    and_expression {"^" and_expression}
//
ASTNodeUP DILParser::ParseExclusiveOrExpression() {
  auto lhs = ParseAndExpression();

  while (CurToken().Is(Token::caret)) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseAndExpression();
    lhs = BuildBinaryOp(BinaryOpKind::Xor, std::move(lhs), std::move(rhs),
                        token.GetLocation());
  }

  return lhs;
}

// Parse an and_expression.
//
//  and_expression:
//    equality_expression {"&" equality_expression}
//
ASTNodeUP DILParser::ParseAndExpression() {
  auto lhs = ParseEqualityExpression();

  while (CurToken().Is(Token::amp)) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseEqualityExpression();
    lhs = BuildBinaryOp(BinaryOpKind::And, std::move(lhs), std::move(rhs),
                        token.GetLocation());
  }

  return lhs;
}

// Parse an equality_expression.
//
//  equality_expression:
//    relational_expression {"==" relational_expression}
//    relational_expression {"!=" relational_expression}
//
ASTNodeUP DILParser::ParseEqualityExpression() {
  auto lhs = ParseRelationalExpression();

  while (CurToken().IsOneOf({Token::equalequal, Token::exclaimequal})) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseRelationalExpression();
    lhs = BuildBinaryOp(dil_token_kind_to_binary_op_kind(token.GetKind()),
                        std::move(lhs), std::move(rhs), token.GetLocation());
  }

  return lhs;
}

// Parse a relational_expression.
//
//  relational_expression:
//    shift_expression {"<" shift_expression}
//    shift_expression {">" shift_expression}
//    shift_expression {"<=" shift_expression}
//    shift_expression {">=" shift_expression}
//
ASTNodeUP DILParser::ParseRelationalExpression() {
  auto lhs = ParseShiftExpression();

  while (CurToken().IsOneOf(
      {Token::less, Token::greater, Token::lessequal, Token::greaterequal})) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseShiftExpression();
    lhs = BuildBinaryOp(dil_token_kind_to_binary_op_kind(token.GetKind()),
                        std::move(lhs), std::move(rhs), token.GetLocation());
  }

  return lhs;
}

// Parse a shift_expression.
//
//  shift_expression:
//    additive_expression {"<<" additive_expression}
//    additive_expression {">>" additive_expression}
//
ASTNodeUP DILParser::ParseShiftExpression() {
  auto lhs = ParseAdditiveExpression();

  while (CurToken().IsOneOf({Token::lessless, Token::greatergreater})) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseAdditiveExpression();
    lhs = BuildBinaryOp(dil_token_kind_to_binary_op_kind(token.GetKind()),
                        std::move(lhs), std::move(rhs), token.GetLocation());
  }

  return lhs;
}

// Parse an additive_expression.
//
//  additive_expression:
//    multiplicative_expression {"+" multiplicative_expression}
//    multiplicative_expression {"-" multiplicative_expression}
//
ASTNodeUP DILParser::ParseAdditiveExpression() {
  auto lhs = ParseMultiplicativeExpression();

  while (CurToken().IsOneOf({Token::plus, Token::minus})) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseMultiplicativeExpression();
    lhs = BuildBinaryOp(dil_token_kind_to_binary_op_kind(token.GetKind()),
                        std::move(lhs), std::move(rhs), token.GetLocation());
  }

  return lhs;
}

// Parse a multiplicative_expression.
//
//  multiplicative_expression:
//    cast_expression {"*" cast_expression}
//    cast_expression {"/" cast_expression}
//    cast_expression {"%" cast_expression}
//
ASTNodeUP DILParser::ParseMultiplicativeExpression() {
  auto lhs = ParseCastExpression();

  while (CurToken().IsOneOf({Token::star, Token::slash, Token::percent})) {
    Token token = CurToken();
    m_dil_lexer.Advance();
    auto rhs = ParseCastExpression();
    lhs = BuildBinaryOp(dil_token_kind_to_binary_op_kind(token.GetKind()),
                        std::move(lhs), std::move(rhs), token.GetLocation());
  }

  return lhs;
}

// Parse a cast_expression.
//
//  cast_expression:
//    unary_expression
//    "(" type_id ")" cast_expression
//
ASTNodeUP DILParser::ParseCastExpression() {
  // This can be a C-style cast, try parsing the contents as a type declaration.
  if (CurToken().Is(Token::l_paren)) {
    Token token = CurToken();

    // Enable lexer backtracking, so that we can rollback in case it's not
    // actually a type declaration.

    // Start tentative parsing (save token location/idx, for possible rollback).
    uint32_t save_token_idx = m_dil_lexer.GetCurrentTokenIdx();

    // Consume the token only after enabling the backtracking.
    m_dil_lexer.Advance();

    // Try parsing the type declaration. If the returned value is not valid,
    // then we should rollback and try parsing the expression.
    auto type_id = ParseTypeId();
    if (type_id) {
      // Successfully parsed the type declaration. Commit the backtracked
      // tokens and parse the cast_expression.

      if (!type_id.value().IsValid())
        return std::make_unique<ErrorNode>();

      Expect(Token::r_paren);
      m_dil_lexer.Advance();
      auto rhs = ParseCastExpression();

      return BuildCStyleCast(type_id.value(), std::move(rhs),
                             token.GetLocation());
    }

    // Failed to parse the contents of the parentheses as a type declaration.
    // Rollback the lexer and try parsing it as unary_expression.
    TentativeParsingRollback(save_token_idx);
  }

  return ParseUnaryExpression();
}

// Parse an unary_expression.
//
//  unary_expression:
//    postfix_expression
//    "++" cast_expression
//    "--" cast_expression
//    unary_operator cast_expression
//    sizeof unary_expression
//    sizeof "(" type_id ")"
//
//  unary_operator:
//    "&"
//    "*"
//    "+"
//    "-"
//    "~"
//    "!"
//
ASTNodeUP DILParser::ParseUnaryExpression() {
  if (CurToken().IsOneOf({Token::plusplus, Token::minusminus, Token::star,
                          Token::amp, Token::plus, Token::minus, Token::exclaim,
                          Token::tilde})) {
    Token token = CurToken();
    uint32_t loc = token.GetLocation();
    m_dil_lexer.Advance();
    auto rhs = ParseCastExpression();

    switch (token.GetKind()) {
      case Token::plusplus:
        return BuildUnaryOp(UnaryOpKind::PreInc, std::move(rhs), loc);
      case Token::minusminus:
        return BuildUnaryOp(UnaryOpKind::PreDec, std::move(rhs), loc);
      case Token::star:
        return BuildUnaryOp(UnaryOpKind::Deref, std::move(rhs), loc);
      case Token::amp:
        return BuildUnaryOp(UnaryOpKind::AddrOf, std::move(rhs), loc);
      case Token::plus:
        return BuildUnaryOp(UnaryOpKind::Plus, std::move(rhs), loc);
      case Token::minus:
        return BuildUnaryOp(UnaryOpKind::Minus, std::move(rhs), loc);
      case Token::tilde:
        return BuildUnaryOp(UnaryOpKind::Not, std::move(rhs), loc);
      case Token::exclaim:
        return BuildUnaryOp(UnaryOpKind::LNot, std::move(rhs), loc);

      default:
        llvm_unreachable("invalid token kind");
    }
  }

  if (CurToken().Is(Token::kw_sizeof)) {
    uint32_t sizeof_loc = CurToken().GetLocation();
    m_dil_lexer.Advance();

    // [expr.sizeof](http://eel.is/c++draft/expr.sizeof#1)
    //
    // The operand is either an expression, which is an unevaluated operand,
    // or a parenthesized type-id.

    // Either operand itself (if it's a type_id), or an operand return type
    // (if it's an expression).
    CompilerType operand;

    // `(` can mean either a type_id or a parenthesized expression.
    if (CurToken().Is(Token::l_paren)) {
      // Start tentative parsing (save token location/idx, for possible
      // rollback).
      uint32_t save_token_idx = m_dil_lexer.GetCurrentTokenIdx();

      Expect(Token::l_paren);
      m_dil_lexer.Advance();

      // Parse the type definition and resolve the type.
      auto type_id = ParseTypeId();
      if (type_id) {
        // type_id requires parentheses, so there must be a closing one.
        Expect(Token::r_paren);
        m_dil_lexer.Advance();

        operand = type_id.value();

      } else {
        TentativeParsingRollback(save_token_idx);

        // Failed to parse type_id, fallback to parsing an unary_expression.
        operand = ParseUnaryExpression()->GetDereferencedResultType();
      }

    } else {
      // No opening parenthesis means this must be an unary_expression.
      operand = ParseUnaryExpression()->GetDereferencedResultType();
    }
    // TODO: move
    // if (!operand.IsValid())
    //   return std::make_unique<ErrorNode>();

    lldb::BasicType size_type;
    llvm::Triple triple(llvm::Twine(
        m_ctx_scope->CalculateTarget()->GetArchitecture().GetTriple().str()));
    if (triple.isOSWindows()) {
      size_type = triple.isArch64Bit() ? lldb::eBasicTypeUnsignedLongLong
          : lldb::eBasicTypeUnsignedInt;
    } else {
      size_type = triple.isArch64Bit() ? lldb::eBasicTypeUnsignedLong
          : lldb::eBasicTypeUnsignedInt;
  }

    auto result_type = GetBasicType(m_ctx_scope, size_type);
    return std::make_unique<SizeOfNode>(sizeof_loc, result_type, operand);
  }

  return ParsePostfixExpression();
}

// Parse a postfix_expression.
//
//  postfix_expression:
//    primary_expression
//    postfix_expression "[" expression "]"
//    postfix_expression "." id_expression
//    postfix_expression "->" id_expression
//    postfix_expression "++"
//    postfix_expression "--"
//    static_cast "<" type_id ">" "(" expression ")" ;
//    dynamic_cast "<" type_id ">" "(" expression ")" ;
//    reinterpret_cast "<" type_id ">" "(" expression ")" ;
//
ASTNodeUP DILParser::ParsePostfixExpression() {
  // Parse the first part of the postfix_expression. This could be either a
  // primary_expression, or a postfix_expression itself.
  ASTNodeUP lhs;
  CompilerType bad_type;

  // C++-style cast.
  if (CurToken().IsOneOf({Token::kw_static_cast, Token::kw_dynamic_cast,
                          Token::kw_reinterpret_cast})) {
    Token::Kind cast_kind = CurToken().GetKind();
    uint32_t cast_loc = CurToken().GetLocation();
    m_dil_lexer.Advance();

    Expect(Token::less);
    m_dil_lexer.Advance();

    uint32_t loc = CurToken().GetLocation();

    // Parse the type definition and resolve the type.
    auto type_id = ParseTypeId(/*must_be_type_id*/ true);
    if (!type_id) {
      BailOut(ErrorCode::kInvalidOperandType,
              "type name requires a specifier or qualifier", loc);
      return std::make_unique<ErrorNode>();
    }
    if (!type_id.value().IsValid()) {
      return std::make_unique<ErrorNode>();
    }

    Expect(Token::greater);
    m_dil_lexer.Advance();

    Expect(Token::l_paren);
    m_dil_lexer.Advance();
    auto rhs = ParseExpression();
    Expect(Token::r_paren);
    m_dil_lexer.Advance();

    lhs = BuildCxxCast(cast_kind, type_id.value(), std::move(rhs), cast_loc);

  } else {
    // Otherwise it's a primary_expression.
    lhs = ParsePrimaryExpression();
  }
  assert(lhs && "LHS of the postfix_expression can't be NULL.");

  while (CurToken().IsOneOf({Token::l_square, Token::period, Token::arrow,
                             Token::plusplus, Token::minusminus})) {
    Token token = CurToken();
    switch (token.GetKind()) {
      case Token::period:
      case Token::arrow: {
        m_dil_lexer.Advance();
        Token member_token = CurToken();
        auto member_id = ParseIdExpression();
        // Check if this is a function call.
        if (CurToken().Is(Token::l_paren)) {
          // TODO: Check if `member_id` is actually a member function of `lhs`.
          // If not, produce a more accurate diagnostic.
          BailOut(ErrorCode::kNotImplemented,
                  "member function calls are not supported",
                  CurToken().GetLocation());
        }
        lhs = BuildMemberOf(std::move(lhs), std::move(member_id),
                            token.GetKind() == Token::arrow,
                            member_token.GetLocation());
        break;
      }
      case Token::plusplus: {
        m_dil_lexer.Advance();
        return BuildUnaryOp(UnaryOpKind::PostInc, std::move(lhs),
                            token.GetLocation());
      }
      case Token::minusminus: {
        m_dil_lexer.Advance();
        return BuildUnaryOp(UnaryOpKind::PostDec, std::move(lhs),
                            token.GetLocation());
      }
      case Token::l_square: {
        m_dil_lexer.Advance();
        auto rhs = ParseExpression();
        Expect(Token::r_square);
        m_dil_lexer.Advance();
        lhs = BuildBinarySubscript(std::move(lhs), std::move(rhs),
                                   token.GetLocation());
        break;
      }

      default:
        llvm_unreachable("invalid token");
    }
  }

  return lhs;
}

// Parse a primary_expression.
//
//  primary_expression:
//    numeric_literal
//    boolean_literal
//    pointer_literal
//    id_expression
//    "(" expression ")"
//    builtin_func
//
ASTNodeUP DILParser::ParsePrimaryExpression() {
  CompilerType bad_type;
  if (CurToken().Is(Token::numeric_constant)) {
    return ParseNumericLiteral();
  } else if (CurToken().IsOneOf({Token::kw_true, Token::kw_false})) {
    return ParseBooleanLiteral();
  } else if (CurToken().IsOneOf({Token::char_constant,
                                 Token::wide_char_constant,
                                 Token::utf8_char_constant})) {
    return ParseCharLiteral();
  } else if (m_dil_lexer.IsStringLiteral(CurToken().GetKind())) {
    // Note: Only expressions that consist of a single string literal can be
    // handled by DIL.
    BailOut(ErrorCode::kNotImplemented, "string literals are not supported",
            CurToken().GetLocation());
    return std::make_unique<ErrorNode>();
  } else if (CurToken().Is(Token::kw_nullptr)) {
    return ParsePointerLiteral();
  } else if (CurToken().IsOneOf({Token::coloncolon, Token::identifier})) {
    // Save the source location for the diagnostics message.
    uint32_t loc = CurToken().GetLocation();
    auto identifier = ParseIdExpression();
    // Check if this is a function call.
    if (CurToken().Is(Token::l_paren)) {
      auto func_def = GetBuiltinFunctionDef(m_ctx_scope, identifier);
      if (!func_def) {
        BailOut(
            ErrorCode::kNotImplemented,
            llvm::formatv("function '{0}' is not a supported builtin intrinsic",
                          identifier),
            loc);
        return std::make_unique<ErrorNode>();
      }
      return ParseBuiltinFunction(loc, std::move(func_def));
    }
    // Otherwise look for an identifier.
    // TODO: Handle bitfield identifiers when evaluating in the value context.
    auto value =
        LookupIdentifier(identifier, m_ctx_scope, m_use_dynamic, nullptr);
    if (!value)
      value = LookupGlobalIdentifier(identifier, m_ctx_scope,
                                     m_ctx_scope->CalculateTarget(),
                                     m_use_dynamic, nullptr);

    if (!value) {
      BailOut(ErrorCode::kUndeclaredIdentifier,
              llvm::formatv("use of undeclared identifier '{0}'", identifier),
              loc);
      return std::make_unique<ErrorNode>();
    }
    return std::make_unique<IdentifierNode>(
        loc, identifier, m_use_dynamic, std::move(value),
        /*is_rvalue*/ false, IsContextVar(identifier));
  } else if (CurToken().Is(Token::l_paren)) {
    // Check in case this is an anonynmous namespace
    if (m_dil_lexer.LookAhead(1).Is(Token::identifier)
        && (((Token)m_dil_lexer.LookAhead(1)).GetSpelling() == "anonymous")
        && m_dil_lexer.LookAhead(2).Is(Token::kw_namespace)
        && m_dil_lexer.LookAhead(3).Is(Token::r_paren)
        && m_dil_lexer.LookAhead(4).Is(Token::coloncolon)) {
      m_dil_lexer.Advance(4);
      std::string identifier = "(anonymous namespace)";
      Expect(Token::coloncolon);
      // Save the source location for the diagnostics message.
      uint32_t loc = CurToken().GetLocation();
      m_dil_lexer.Advance();
      assert(
          (CurToken().Is(Token::identifier) || CurToken().Is(Token::l_paren)) &&
          "Expected an identifier or anonymous namespeace, but not found.");
      std::string identifier2 = ParseNestedNameSpecifier();
      if (identifier2.empty()) {
        // There was only an identifer, no more levels of nesting. Or there
        // was an invalid expression starting with a left parenthesis.
        Expect(Token::identifier);
        identifier2 = CurToken().GetSpelling();
        m_dil_lexer.Advance();
      }
      identifier = identifier + "::" + identifier2;
      auto value = LookupIdentifier(identifier, m_ctx_scope, m_use_dynamic);
      if (!value)
        value = LookupGlobalIdentifier(identifier, m_ctx_scope,
                                       m_ctx_scope->CalculateTarget(),
                                       m_use_dynamic);
      if (!value) {
        BailOut(ErrorCode::kUndeclaredIdentifier,
                llvm::formatv("use of undeclared identifier '{0}'", identifier),
                loc);
        return std::make_unique<ErrorNode>();
      }
      return std::make_unique<IdentifierNode>(
          loc, identifier, m_use_dynamic, std::move(value),
          /* is_rvalue */ false, IsContextVar(identifier));
    } else {
      m_dil_lexer.Advance();
      auto expr = ParseExpression();
      Expect(Token::r_paren);
      m_dil_lexer.Advance();
      return expr;
    }
  }

  BailOut(ErrorCode::kInvalidExpressionSyntax,
          llvm::formatv("Unexpected token: {0}", CurToken()),
          CurToken().GetLocation());
  return std::make_unique<ErrorNode>();
}

// Parse a type_id.
//
//  type_id:
//    type_specifier_seq [abstract_declarator]
//
std::optional<CompilerType> DILParser::ParseTypeId(bool must_be_type_id) {
  uint32_t type_loc = CurToken().GetLocation();
  TypeDeclaration type_decl;
  CompilerType bad_type;

  // type_specifier_seq is required here, start with trying to parse it.
  ParseTypeSpecifierSeq(&type_decl);

  if (type_decl.IsEmpty()) {
    // TODO: Should we bail out if `must_be_type_id` is set?
    return {};
  }

  if (type_decl.m_has_error) {
    if (type_decl.m_is_builtin) {
      return bad_type;
    }

    assert(type_decl.m_is_user_type && "type_decl must be a user type");
    // Found something looking like a user type, but failed to parse it.
    // Return invalid type if we expect to have a type here, otherwise nullopt.
    if (must_be_type_id) {
      return bad_type;
    }
    return {};
  }

  // Try to resolve the base type.
  CompilerType type;
  if (type_decl.m_is_builtin) {
    type = GetBasicType(m_ctx_scope, type_decl.GetBasicType());
    assert(type.IsValid() && "cannot resolve basic type");

  } else {
    assert(type_decl.m_is_user_type && "type_decl must be a user type");
    type = ResolveTypeByName(type_decl.m_user_typename, m_ctx_scope);
    if (!type.IsValid()) {
      if (must_be_type_id) {
        BailOut(
            ErrorCode::kUndeclaredIdentifier,
            llvm::formatv("unknown type name '{0}'",
                          type_decl.m_user_typename),
            type_loc);
        return bad_type;
      }
      return {};
    }

    if (LookupIdentifier(type_decl.m_user_typename, m_ctx_scope,
                         m_use_dynamic)) {
      // Same-name identifiers should be preferred over typenames.
      // TODO: Make type accessible with 'class', 'struct' and 'union' keywords.
      if (must_be_type_id) {
        BailOut(ErrorCode::kUndeclaredIdentifier,
                llvm::formatv(
                    "must use '{0}' tag to refer to type '{1}' in this scope",
                    type.GetTypeTag(), type_decl.m_user_typename),
                type_loc);
        return bad_type;
      }
      return {};
    }

    if (LookupGlobalIdentifier(type_decl.m_user_typename, m_ctx_scope,
                               m_ctx_scope->CalculateTarget(), m_use_dynamic)) {
      // Same-name identifiers should be preferred over typenames.
      // TODO: Make type accessible with 'class', 'struct' and 'union' keywords.
      if (must_be_type_id) {
        BailOut(ErrorCode::kUndeclaredIdentifier,
                llvm::formatv(
                    "must use '{0}' tag to refer to type '{1}' in this scope",
                    type.GetTypeTag(), type_decl.m_user_typename),
                type_loc);
        return bad_type;
      }
      return {};
    }
  }

  //
  //  abstract_declarator:
  //    ptr_operator [abstract_declarator]
  //
  std::vector<DILParser::PtrOperator> ptr_operators;
  while (IsPtrOperator(CurToken())) {
    ptr_operators.push_back(ParsePtrOperator());
  }
  type = ResolveTypeDeclarators(type, ptr_operators);

  return type;
}

// Parse a type_specifier_seq.
//
//  type_specifier_seq:
//    type_specifier [type_specifier_seq]
//
void DILParser::ParseTypeSpecifierSeq(TypeDeclaration* type_decl) {
  while (true) {
    bool type_specifier = ParseTypeSpecifier(type_decl);
    if (!type_specifier) {
      break;
    }
  }
}

// Parse a type_specifier.
//
//  type_specifier:
//    simple_type_specifier
//    cv_qualifier
//
//  simple_type_specifier:
//    ["::"] [nested_name_specifier] type_name
//    "char"
//    "char16_t"
//    "char32_t"
//    "wchar_t"
//    "bool"
//    "short"
//    "int"
//    "long"
//    "signed"
//    "unsigned"
//    "float"
//    "double"
//    "void"
//
// Returns TRUE if a type_specifier was successfully parsed at this location.
//
bool DILParser::ParseTypeSpecifier(TypeDeclaration* type_decl) {
  if (IsCvQualifier(CurToken())) {
    // Just ignore CV quialifiers, we don't use them in type casting.
    m_dil_lexer.Advance();
    return true;
  }

  if (IsSimpleTypeSpecifierKeyword(CurToken())) {
    // User-defined typenames can't be combined with builtin keywords.
    if (type_decl->m_is_user_type) {
      BailOut(ErrorCode::kInvalidOperandType,
              "cannot combine with previous declaration specifier",
              CurToken().GetLocation());
      type_decl->m_has_error = true;
      return false;
    }

    // From now on this type declaration must describe a builtin type.
    // TODO: Should this be allowed -- `unsigned myint`?
    type_decl->m_is_builtin = true;

    if (!HandleSimpleTypeSpecifier(type_decl)) {
      type_decl->m_has_error = true;
      return false;
    }
    m_dil_lexer.Advance();
    return true;
  }

  // The type_specifier must be a user-defined type. Try parsing a
  // simple_type_specifier.
  {
    // Try parsing optional global scope operator.
    bool global_scope = false;
    if (CurToken().Is(Token::coloncolon)) {
      global_scope = true;
      m_dil_lexer.Advance();
    }

    uint32_t loc = CurToken().GetLocation();

    // Try parsing optional nested_name_specifier.
    auto nested_name_specifier = ParseNestedNameSpecifier();

    // Try parsing required type_name.
    auto type_name = ParseTypeName();

    // If there is a type_name, then this is indeed a simple_type_specifier.
    // Global and qualified (namespace/class) scopes can be empty, since they're
    // optional. In this case type_name is type we're looking for.
    if (!type_name.empty()) {
      // User-defined typenames can't be combined with builtin keywords.
      if (type_decl->m_is_builtin) {
        BailOut(ErrorCode::kInvalidOperandType,
                "cannot combine with previous declaration specifier", loc);
        type_decl->m_has_error = true;
        return false;
      }
      // There should be only one user-defined typename.
      if (type_decl->m_is_user_type) {
        BailOut(ErrorCode::kInvalidOperandType,
                "two or more data types in declaration of 'type name'", loc);
        type_decl->m_has_error = true;
        return false;
      }

      // Construct the fully qualified typename.
      type_decl->m_is_user_type = true;
      type_decl->m_user_typename =
          llvm::formatv("{0}{1}{2}", global_scope ? "::" : "",
                        nested_name_specifier, type_name);
      return true;
    }
  }

  // No type_specifier was found here.
  return false;
}

// Parse nested_name_specifier.
//
//  nested_name_specifier:
//    type_name "::"
//    namespace_name '::'
//    nested_name_specifier identifier "::"
//    nested_name_specifier simple_template_id "::"
//
std::string DILParser::ParseNestedNameSpecifier() {
  // The first token in nested_name_specifier is always an identifier, or
  // '(anonymous namespace)'.
  if (CurToken().IsNot(Token::identifier) && CurToken().IsNot(Token::l_paren)) {
    return "";
  }

  // Anonymous namespaces need to be treated specially: They are represented
  // the the string '(anonymous namespace)', which has a space in it (throwing
  // off normal parsing) and is not actually proper C++> Check to see if we're
  // looking at '(anonymous namespace)::...'
  if (CurToken().Is(Token::l_paren)) {
    // Look for all the pieces, in order:
    // l_paren 'anonymous' 'namespace' r_paren coloncolon
    if (m_dil_lexer.LookAhead(1).Is(Token::identifier)
        && (((Token)m_dil_lexer.LookAhead(1)).GetSpelling() == "anonymous")
        && m_dil_lexer.LookAhead(2).Is(Token::kw_namespace)
        && m_dil_lexer.LookAhead(3).Is(Token::r_paren)
        && m_dil_lexer.LookAhead(4).Is(Token::coloncolon)) {
      m_dil_lexer.Advance(4);

      assert(
          (CurToken().Is(Token::identifier) || CurToken().Is(Token::l_paren)) &&
          "Expected an identifier or anonymous namespace, but not found.");
      // Continue parsing the nested_namespace_specifier.
      std::string identifier2 = ParseNestedNameSpecifier();
      if (identifier2.empty()) {
        Expect(Token::identifier);
        identifier2 = CurToken().GetSpelling();
        m_dil_lexer.Advance();
      }
      return "(anonymous namespace)::" + identifier2;
    } else {
      return "";
    }
  } // end of special handling for '(anonymous namespace)'

  // If the next token is scope ("::"), then this is indeed a
  // nested_name_specifier
  if (m_dil_lexer.LookAhead(1).Is(Token::coloncolon)) {
    // This nested_name_specifier is a single identifier.
    std::string identifier = CurToken().GetSpelling();
    m_dil_lexer.Advance(1);
    Expect(Token::coloncolon);
    m_dil_lexer.Advance();
    // Continue parsing the nested_name_specifier.
    return identifier + "::" + ParseNestedNameSpecifier();
  }

  // If the next token starts a template argument list, then we have a
  // simple_template_id here.
  if (m_dil_lexer.LookAhead(1).Is(Token::less)) {
    // We don't know whether this will be a nested_name_identifier or just a
    // type_name. Prepare to rollback if this is not a nested_name_identifier.

    // Start tentative parsing (save token location/idx, for possible rollback).
    uint32_t save_token_idx = m_dil_lexer.GetCurrentTokenIdx();

    // TODO: Parse just the simple_template_id?
    auto type_name = ParseTypeName();

    // If we did parse the type_name successfully and it's followed by the scope
    // operator ("::"), then this is indeed a nested_name_specifier. Continue
    // parsing nested_name_specifier.
    if (!type_name.empty() && CurToken().Is(Token::coloncolon)) {
      m_dil_lexer.Advance();
      // Continue parsing the nested_name_specifier.
      return type_name + "::" + ParseNestedNameSpecifier();
    }

    // Not a nested_name_specifier, but could be just a type_name or something
    // else entirely. Rollback the parser and try a different path.

    TentativeParsingRollback(save_token_idx);
  }

  return "";
}

// Parse a type_name.
//
//  type_name:
//    class_name
//    enum_name
//    typedef_name
//    simple_template_id
//
//  class_name
//    identifier
//
//  enum_name
//    identifier
//
//  typedef_name
//    identifier
//
//  simple_template_id:
//    template_name "<" [template_argument_list] ">"
//
std::string DILParser::ParseTypeName() {
  // Typename always starts with an identifier.
  if (CurToken().IsNot(Token::identifier)) {
    return "";
  }

  // If the next token starts a template argument list, parse this type_name as
  // a simple_template_id.
  if (m_dil_lexer.LookAhead(1).Is(Token::less)) {
    // Parse the template_name. In this case it's just an identifier.
    std::string template_name = CurToken().GetSpelling();
    m_dil_lexer.Advance(1);
    // Consume the "<" token.
    m_dil_lexer.Advance();

    // Short-circuit for missing template_argument_list.
    if (CurToken().Is(Token::greater)) {
      m_dil_lexer.Advance();
      return llvm::formatv("{0}<>", template_name);
    }

    // Try parsing template_argument_list.
    auto template_argument_list = ParseTemplateArgumentList();

    if (CurToken().Is(Token::greater)) {
      // Single closing angle bracket is a valid end of the template argument
      // list, just consume it.
      m_dil_lexer.Advance();

    } else if (CurToken().Is(Token::greatergreater)) {
      // C++11 allows using ">>" in nested template argument lists and C++-style
      // casts. In this case we alter change the token type to ">", but don't
      // consume it -- it will be done on the outer level when completing the
      // outer template argument list or C++-style cast.
      uint32_t loc = CurToken().GetLocation();
      m_dil_lexer.InsertToken(Token(Token::greater, ">", loc+1));

    } else {
      // Not a valid end of the template argument list, failed to parse a
      // simple_template_id
      return "";
    }

    return llvm::formatv("{0}<{1}>", template_name, template_argument_list);
  }

  // Otherwise look for a class_name, enum_name or a typedef_name.
  std::string identifier = CurToken().GetSpelling();
  m_dil_lexer.Advance();

  return identifier;
}

// Parse a template_argument_list.
//
//  template_argument_list:
//    template_argument
//    template_argument_list "," template_argument
//
std::string DILParser::ParseTemplateArgumentList() {
  // Parse template arguments one by one.
  std::vector<std::string> arguments;

  do {
    // Eat the comma if this is not the first iteration.
    if (arguments.size() > 0) {
      m_dil_lexer.Advance();
    }

    // Try parsing a template_argument. If this fails, then this is actually not
    // a template_argument_list.
    auto argument = ParseTemplateArgument();
    if (argument.empty()) {
      return "";
    }

    arguments.push_back(argument);

  } while (CurToken().Is(Token::comma));

  // Internally in LLDB/Clang nested template type names have extra spaces to
  // avoid having ">>". Add the extra space before the closing ">" if the
  // template argument is also a template.
  if (arguments.back().back() == '>') {
    arguments.back().push_back(' ');
  }

  return llvm::formatv("{0:$[, ]}",
                       llvm::make_range(arguments.begin(), arguments.end()));
}

// Parse a template_argument.
//
//  template_argument:
//    type_id
//    numeric_literal
//    id_expression
//
std::string DILParser::ParseTemplateArgument() {
  // There is no way to know at this point whether there is going to be a
  // type_id or something else. Try different options one by one.

  {
    // [temp.arg](http://eel.is/c++draft/temp.arg#2)
    //
    // In a template-argument, an ambiguity between a type-id and an expression
    // is resolved to a type-id, regardless of the form of the corresponding
    // template-parameter.

    // Therefore, first try parsing type_id.
    // Start tentative parsing (save token location/idx, for possible rollback).
    uint32_t save_token_idx = m_dil_lexer.GetCurrentTokenIdx();

    auto type_id = ParseTypeId();
    if (type_id) {

      CompilerType type = type_id.value();
      return type.IsValid()
          ? std::string(type.GetTypeName().AsCString())
          : "";

    } else {
      // Failed to parse a type_id. Rollback the parser and try something else.
      TentativeParsingRollback(save_token_idx);
    }
  }

  {
    // The next candidate is a numeric_literal.
    // Start tentative parsing (save token location/idx, for possible rollback).
    uint32_t save_token_idx = m_dil_lexer.GetCurrentTokenIdx();

    // Parse a numeric_literal.
    if (CurToken().Is(Token::numeric_constant)) {
      // TODO: Actually parse the literal, check if it's valid and
      // canonize it (e.g. 8LL -> 8).
      std::string numeric_literal = CurToken().GetSpelling();
      m_dil_lexer.Advance();

      if (TokenEndsTemplateArgumentList(CurToken())) {
        return numeric_literal;
      }
    }

    // Failed to parse a numeric_literal.
    TentativeParsingRollback(save_token_idx);
  }

  {
    // The next candidate is an id_expression.
    // Start tentative parsing (save token location/idx, for possible rollback).
    uint32_t save_token_idx = m_dil_lexer.GetCurrentTokenIdx();

    // Parse an id_expression.
    auto id_expression = ParseIdExpression();

    // If we've parsed the id_expression successfully and the next token can
    // finish the template_argument, then we're done here.
    if (!id_expression.empty() && TokenEndsTemplateArgumentList(CurToken())) {
      return id_expression;
    }
    // Failed to parse a id_expression.
    TentativeParsingRollback(save_token_idx);
  }

  // TODO: Another valid option here is a constant_expression, but
  // we definitely don't want to support constant arithmetic like "Foo<1+2>".
  // We can probably use ParsePrimaryExpression here, but need to figure out the
  // "stringification", since ParsePrimaryExpression returns ASTNodeUP (and
  // potentially a whole expression, not just a single constant.)

  // This is not a template_argument.
  return "";
}

// Parse a ptr_operator.
//
//  ptr_operator:
//    "*" [cv_qualifier_seq]
//    "&"
//
DILParser::PtrOperator DILParser::ParsePtrOperator() {
  ExpectOneOf(std::vector<Token::Kind>{Token::star, Token::amp});

  PtrOperator ptr_operator;
  if (CurToken().Is(Token::star)) {
    ptr_operator = std::make_tuple(Token::star, CurToken().GetLocation());
    m_dil_lexer.Advance();

    //
    //  cv_qualifier_seq:
    //    cv_qualifier [cv_qualifier_seq]
    //
    //  cv_qualifier:
    //    "const"
    //    "volatile"
    //
    while (IsCvQualifier(CurToken())) {
      // Just ignore CV quialifiers, we don't use them in type casting.
      m_dil_lexer.Advance();
    }

  } else if (CurToken().Is(Token::amp)) {
    ptr_operator = std::make_tuple(Token::amp, CurToken().GetLocation());
    m_dil_lexer.Advance();
  }

  return ptr_operator;
}

// Parse an id_expression.
//
//  id_expression:
//    unqualified_id
//    qualified_id
//
//  qualified_id:
//    ["::"] [nested_name_specifier] unqualified_id
//    ["::"] identifier
//
//  identifier:
//    ? Token::identifier ?
//
std::string DILParser::ParseIdExpression() {
  // Try parsing optional global scope operator.
  bool global_scope = false;
  if (CurToken().Is(Token::coloncolon)) {
    global_scope = true;
    m_dil_lexer.Advance();
  }

  // Try parsing optional nested_name_specifier.
  auto nested_name_specifier = ParseNestedNameSpecifier();

  // If nested_name_specifier is present, then it's qualified_id production.
  // Follow the first production rule.
  if (!nested_name_specifier.empty()) {
    // Parse unqualified_id and construct a fully qualified id expression.
    auto unqualified_id = ParseUnqualifiedId();

    return llvm::formatv("{0}{1}{2}", global_scope ? "::" : "",
                         nested_name_specifier, unqualified_id);
  }

  // No nested_name_specifier, but with global scope -- this is also a
  // qualified_id production. Follow the second production rule.
  else if (global_scope) {
    Expect(Token::identifier);
    std::string identifier = CurToken().GetSpelling();
    m_dil_lexer.Advance();
    return llvm::formatv("{0}{1}", global_scope ? "::" : "", identifier);
  }

  // This is unqualified_id production.
  return ParseUnqualifiedId();
}

// Parse an unqualified_id.
//
//  unqualified_id:
//    identifier
//
//  identifier:
//    ? Token::identifier ?
//
std::string DILParser::ParseUnqualifiedId() {
  Expect(Token::identifier);
  std::string identifier = CurToken().GetSpelling();
  m_dil_lexer.Advance();
  return identifier;
}

// Parse a numeric_literal.
//
//  numeric_literal:
//    ? Token::numeric_constant ?
//
ASTNodeUP DILParser::ParseNumericLiteral() {
  Expect(Token::numeric_constant);
  ASTNodeUP numeric_constant = ParseNumericConstant();
  m_dil_lexer.Advance();
  return numeric_constant;
}

// Parse an boolean_literal.
//
//  boolean_literal:
//    "true"
//    "false"
//
ASTNodeUP DILParser::ParseBooleanLiteral() {
  ExpectOneOf(std::vector<Token::Kind>{Token::kw_true, Token::kw_false});
  uint32_t loc = CurToken().GetLocation();
  bool literal_value = CurToken().Is(Token::kw_true);
  m_dil_lexer.Advance();
  Scalar scalar_value(static_cast<int>(literal_value));
  return std::make_unique<ScalarLiteralNode>(
      loc, GetBasicType(m_ctx_scope, lldb::eBasicTypeBool), scalar_value);
}

ASTNodeUP DILParser::ParseCharLiteral() {
  ExpectOneOf(std::vector<Token::Kind>{Token::char_constant,
                                       Token::wide_char_constant,
                                       Token::utf8_char_constant});
  uint32_t loc = CurToken().GetLocation();

  std::string token_spelling = CurToken().GetSpelling();

  const char* token_begin = token_spelling.c_str();
  dil::CharLiteralParser char_literal(token_begin,
                                      token_begin + token_spelling.size(), loc,
                                      m_dil_lexer, CurToken().GetKind());

  if (char_literal.hadError()) {
    // TODO: Add new ErrorCode kInvalidCharLiteral and use it
    BailOut(ErrorCode::kInvalidNumericLiteral,
            llvm::formatv("Failed to parse token as char-constant: {0}",
                          CurToken()),
            CurToken().GetLocation());
    CompilerType bad_type;
    return std::make_unique<ErrorNode>();
  }

  auto ctx_basic_type = GetBasicType(m_ctx_scope, PickCharType(char_literal));
  uint64_t byte_size = 0;
  if (auto temp = ctx_basic_type.GetByteSize(nullptr))
    byte_size = temp.value();
  llvm::APInt literal_value(byte_size * CHAR_BIT,
                            char_literal.getValue());

  m_dil_lexer.Advance();
  Scalar scalar_value(literal_value);
  return std::make_unique<ScalarLiteralNode>(loc, ctx_basic_type, scalar_value);
}

// Parse an pointer_literal.
//
//  pointer_literal:
//    "nullptr"
//
ASTNodeUP DILParser::ParsePointerLiteral() {
  Expect(Token::kw_nullptr);
  uint32_t loc = CurToken().GetLocation();
  m_dil_lexer.Advance();
  llvm::APInt raw_value(type_width<uintmax_t>(), 0);
  Scalar scalar_value(raw_value);
  return std::make_unique<ScalarLiteralNode>(
      loc, GetBasicType(m_ctx_scope, lldb::eBasicTypeNullPtr), scalar_value);
}

ASTNodeUP DILParser::ParseNumericConstant() {
  CompilerType bad_type;
  // Parse numeric constant, it can be either integer or float.
  std::string tok_spelling = CurToken().GetSpelling();
  llvm::StringRef tok_spelling_ref(tok_spelling);

  lldb::TargetSP target_sp = m_ctx_scope->CalculateTarget();
  dil::NumericLiteralParser literal(tok_spelling_ref, CurToken().GetLocation(),
                                    /*AllowHalfType=*/true, m_dil_lexer,
                                    /*AllowMicrosoftExt=*/true);

  if (literal.hadError) {
    BailOut(ErrorCode::kInvalidNumericLiteral,
            llvm::formatv("Failed to parse token as numeric-constant: {0}",
                          CurToken()),
            CurToken().GetLocation());
    return std::make_unique<ErrorNode>();
  }

  // Check for floating-literal and integer-literal. Fail on anything else (i.e.
  // fixed-point literal, who needs them anyway??).
  Token token = CurToken();
  if (literal.isFloatingLiteral()) {
    return ParseFloatingLiteral(literal, token);
  }
  if (literal.isIntegerLiteral()) {
    return ParseIntegerLiteral(literal, token);
  }

  // Don't care about anything else.
  BailOut(ErrorCode::kInvalidNumericLiteral,
          llvm::formatv(
              "numeric-constant should be either float or integer literal: {0}",
              CurToken()),
          CurToken().GetLocation());
  return std::make_unique<ErrorNode>();
}

ASTNodeUP DILParser::ParseFloatingLiteral(dil::NumericLiteralParser &literal,
                                          Token &token) {
  const llvm::fltSemantics& format = literal.isFloat
                                         ? llvm::APFloat::IEEEsingle()
                                         : llvm::APFloat::IEEEdouble();
  llvm::APFloat raw_value(format);
  llvm::RoundingMode rm = llvm::RoundingMode::NearestTiesToEven;
  llvm::APFloat::opStatus result = literal.GetFloatValue(raw_value, rm);

  // Overflow is always an error, but underflow is only an error if we
  // underflowed to zero (APFloat reports denormals as underflow).
  if ((result & llvm::APFloat::opOverflow) ||
      ((result & llvm::APFloat::opUnderflow) && raw_value.isZero())) {
    BailOut(ErrorCode::kInvalidNumericLiteral,
            llvm::formatv("float underflow/overflow happened: {0}", token),
            token.GetLocation());
    CompilerType bad_type;
    return std::make_unique<ErrorNode>();
  }

  auto basic_type =
      literal.isFloat ? lldb::eBasicTypeFloat : lldb::eBasicTypeDouble;
  Scalar scalar_value(raw_value);
  return std::make_unique<ScalarLiteralNode>(
      token.GetLocation(), GetBasicType(m_ctx_scope, basic_type), scalar_value);
}

ASTNodeUP DILParser::ParseIntegerLiteral(dil::NumericLiteralParser &literal,
                                         Token &token) {
  // Create a value big enough to fit all valid numbers.
  llvm::APInt raw_value(type_width<uintmax_t>(), 0);

  if (literal.GetIntegerValue(raw_value)) {
    BailOut(ErrorCode::kInvalidNumericLiteral,
            llvm::formatv("integer literal is too large to be represented in "
                          "any integer type: {0}",
                          token),
            token.GetLocation());
    CompilerType bad_type;
    return std::make_unique<ErrorNode>();
  }

  auto [type, is_unsigned] = PickIntegerType(m_ctx_scope, literal, raw_value);

  Scalar scalar_value(raw_value);
  return std::make_unique<ScalarLiteralNode>(token.GetLocation(),
                                             GetBasicType(m_ctx_scope, type),
                                             scalar_value);
}

// Parse a builtin_func.
//
//  builtin_func:
//    builtin_func_name "(" [builtin_func_argument_list] ")"
//
//  builtin_func_name:
//    "__log2"
//
//  builtin_func_argument_list:
//    builtin_func_argument
//    builtin_func_argument_list "," builtin_func_argument
//
//  builtin_func_argument:
//    expression
//
ASTNodeUP
DILParser::ParseBuiltinFunction(uint32_t loc,
                                std::unique_ptr<BuiltinFunctionDef> func_def) {
  Expect(Token::l_paren);
  m_dil_lexer.Advance();

  std::vector<ASTNodeUP> arguments;
  CompilerType bad_type;

  if (CurToken().Is(Token::r_paren)) {
    // Empty argument list, nothing to do here.
    m_dil_lexer.Advance();
  } else {
    // Non-empty argument list, parse all the arguments.
    do {
      // Eat the comma if this is not the first iteration.
      if (arguments.size() > 0) {
        m_dil_lexer.Advance();
      }

      // Parse a builtin_func_argument. If failed to parse, bail out early and
      // don't try parsing the rest of the arguments.
      auto argument = ParseExpression();
      if (llvm::isa<ErrorNode>(argument)) {
        return std::make_unique<ErrorNode>();
      }

      arguments.push_back(std::move(argument));
    } while (CurToken().Is(Token::comma));

    Expect(Token::r_paren);
    m_dil_lexer.Advance();
  }

  // Check we have the correct number of arguments.
  if (arguments.size() != func_def->m_arguments.size()) {
    BailOut(ErrorCode::kInvalidOperandType,
            llvm::formatv(
                "no matching function for call to '{0}': requires {1} "
                "argument(s), but {2} argument(s) were provided",
                func_def->m_name, func_def->m_arguments.size(),
                arguments.size()),
            loc);
    return std::make_unique<ErrorNode>();
  }

  // Now check that all arguments are correct types and perform implicit
  // conversions if possible.
  for (size_t i = 0; i < arguments.size(); ++i) {
    // HACK: Void means "any" and we'll check in runtime. The argument will be
    // passed as is without any conversions.
    if (func_def->m_arguments[i].GetBasicTypeEnumeration()
        == lldb::eBasicTypeVoid) {
      continue;
    }
    arguments[i] = InsertImplicitConversion(std::move(arguments[i]),
                                            func_def->m_arguments[i]);
    if (llvm::isa<ErrorNode>(arguments[i])) {
      return std::make_unique<ErrorNode>();
    }
  }

  return std::make_unique<BuiltinFunctionCallNode>(
      loc, func_def->m_return_type, func_def->m_name, std::move(arguments));
}

ASTNodeUP DILParser::BuildCStyleCast(CompilerType type, ASTNodeUP rhs,
                                     uint32_t location) {
  // Casting to reference types gives an L-value result.
  bool is_rvalue = !type.IsReferenceType();
  return std::make_unique<CStyleCastNode>(location, type, std::move(rhs),
                                          is_rvalue);
}

ASTNodeUP DILParser::BuildCxxCast(Token::Kind kind, CompilerType type,
                                  ASTNodeUP rhs, uint32_t location) {
  assert((kind == Token::kw_static_cast ||
          kind == Token::kw_dynamic_cast ||
          kind == Token::kw_reinterpret_cast) &&
         "invalid C++-style cast type");

  // TODO: Implement custom builders for all C++-style casts.
  if (kind == Token::kw_dynamic_cast) {
    return BuildCxxDynamicCast(type, std::move(rhs), location);
  }
  if (kind == Token::kw_reinterpret_cast) {
    return BuildCxxReinterpretCast(type, std::move(rhs), location);
  }
  if (kind == Token::kw_static_cast) {
    return BuildCxxStaticCast(type, std::move(rhs), location);
  }
  return BuildCStyleCast(type, std::move(rhs), location);
}

ASTNodeUP DILParser::BuildCxxStaticCast(CompilerType type, ASTNodeUP rhs,
                                        uint32_t location) {
  // Casting to reference types gives an L-value result.
  bool is_rvalue = !type.IsReferenceType();
  return std::make_unique<CxxStaticCastNode>(location, type, std::move(rhs),
                                             CxxStaticCastKind::eNoOp,
                                             /*is_rvalue*/ is_rvalue);
}

ASTNodeUP DILParser::BuildCxxReinterpretCast(CompilerType type, ASTNodeUP rhs,
                                             uint32_t location) {
  CompilerType bad_type;

  // Casting to reference types gives an L-value result.
  bool is_rvalue = !type.IsReferenceType();
  return std::make_unique<CxxReinterpretCastNode>(location, type,
                                                  std::move(rhs), is_rvalue);
}

ASTNodeUP DILParser::BuildCxxDynamicCast(CompilerType type, ASTNodeUP rhs,
                                         uint32_t location) {
  // LLDB doesn't support dynamic_cast in the expression evaluator. We disable
  // it too to match the behaviour, but theoretically it can be implemented.
  BailOut(ErrorCode::kInvalidOperandType,
          "dynamic_cast is not supported in this context", location);
  return std::make_unique<ErrorNode>();
}

ASTNodeUP DILParser::BuildUnaryOp(UnaryOpKind kind, ASTNodeUP rhs,
                                  uint32_t location) {
  return std::make_unique<UnaryOpNode>(location, kind, std::move(rhs));
}

ASTNodeUP DILParser::BuildBinaryOp(BinaryOpKind kind, ASTNodeUP lhs,
                                   ASTNodeUP rhs, uint32_t location) {
  CompilerType result_type;
  CompilerType comp_assign_type;
  return std::make_unique<BinaryOpNode>(location, result_type, kind,
                                        std::move(lhs), std::move(rhs),
                                        comp_assign_type, nullptr);
}

ASTNodeUP DILParser::BuildTernaryOp(ASTNodeUP cond, ASTNodeUP lhs,
                                    ASTNodeUP rhs, uint32_t location) {
  CompilerType bad_type;
  // First check if the condition contextually converted to bool.
  auto cond_type = cond->GetDereferencedResultType();
  if (!cond_type.IsContextuallyConvertibleToBool()) {
    BailOut(
        ErrorCode::kInvalidOperandType,
        llvm::formatv(kValueIsNotConvertibleToBool, cond_type.TypeDescription()),
        location);
    return std::make_unique<ErrorNode>();
  }

  auto lhs_type = lhs->GetDereferencedResultType();
  auto rhs_type = rhs->GetDereferencedResultType();

  // If operands have the same type, don't do any promotions.
  if (lhs_type.CompareTypes(rhs_type)) {
    return std::make_unique<TernaryOpNode>(location, lhs_type, std::move(cond),
                                           std::move(lhs), std::move(rhs));
  }
  // If operands have the same canonical type, use the canonical type.
  if (lhs_type.GetCanonicalType().CompareTypes(rhs_type.GetCanonicalType())) {
    return std::make_unique<TernaryOpNode>(
        location, lhs_type.GetCanonicalType(), std::move(cond), std::move(lhs),
        std::move(rhs));
  }

  // If both operands have arithmetic type, apply the usual arithmetic
  // conversions to bring them to a common type.
  if (lhs_type.IsScalarOrUnscopedEnumerationType() &&
      rhs_type.IsScalarOrUnscopedEnumerationType()) {
    auto result_type = UsualArithmeticConversions(m_ctx_scope, lhs, rhs);
    return std::make_unique<TernaryOpNode>(
        location, result_type, std::move(cond), std::move(lhs), std::move(rhs));
  }

  // Apply array-to-pointer implicit conversions.
  if (lhs_type.IsArrayType()) {
    lhs = InsertArrayToPointerConversion(std::move(lhs));
    lhs_type = lhs->GetDereferencedResultType();
  }
  if (rhs_type.IsArrayType()) {
    rhs = InsertArrayToPointerConversion(std::move(rhs));
    rhs_type = rhs->GetDereferencedResultType();
  }

  // Check if operands have the same pointer type.
  if (lhs_type.CompareTypes(rhs_type)) {
    return std::make_unique<TernaryOpNode>(location, lhs_type, std::move(cond),
                                           std::move(lhs), std::move(rhs));
  }
  // Check if operands have the same canonical pointer type.
  if (lhs_type.GetCanonicalType().CompareTypes(rhs_type.GetCanonicalType())) {
    return std::make_unique<TernaryOpNode>(
        location, lhs_type.GetCanonicalType(), std::move(cond), std::move(lhs),
        std::move(rhs));
  }

  // If one operand is a pointer and the other is a nullptr or literal zero,
  // convert the nullptr operand to pointer type.
  if (lhs_type.IsPointerType() &&
      (rhs->is_literal_zero() || rhs_type.IsNullPtrType())) {
    rhs = std::make_unique<CStyleCastNode>(
        rhs->GetLocation(), lhs_type, std::move(rhs), TypePromotionCastKind::ePointer);

    return std::make_unique<TernaryOpNode>(location, lhs_type, std::move(cond),
                                           std::move(lhs), std::move(rhs));
  }
  if ((lhs->is_literal_zero() || lhs_type.IsNullPtrType()) &&
      rhs_type.IsPointerType()) {
    lhs = std::make_unique<CStyleCastNode>(
        lhs->GetLocation(), rhs_type, std::move(lhs), TypePromotionCastKind::ePointer);

    return std::make_unique<TernaryOpNode>(location, rhs_type, std::move(cond),
                                           std::move(lhs), std::move(rhs));
  }

  // If one operand is nullptr and the other one is literal zero, convert
  // the literal zero to a nullptr type.
  if (lhs_type.IsNullPtrType() && rhs->is_literal_zero()) {
    rhs = std::make_unique<CStyleCastNode>(
        rhs->GetLocation(), lhs_type, std::move(rhs), CStyleCastKind::eNullptr);

    return std::make_unique<TernaryOpNode>(location, lhs_type, std::move(cond),
                                           std::move(lhs), std::move(rhs));
  }
  if (lhs->is_literal_zero() && rhs_type.IsNullPtrType()) {
    lhs = std::make_unique<CStyleCastNode>(
        lhs->GetLocation(), rhs_type, std::move(lhs), CStyleCastKind::eNullptr);

    return std::make_unique<TernaryOpNode>(location, rhs_type, std::move(cond),
                                           std::move(lhs), std::move(rhs));
  }

  BailOut(ErrorCode::kInvalidOperandType,
          llvm::formatv("incompatible operand types ({0} and {1})",
                        lhs_type.TypeDescription(), rhs_type.TypeDescription()),
          location);
  return std::make_unique<ErrorNode>();
}

ASTNodeUP DILParser::BuildBinarySubscript(ASTNodeUP lhs, ASTNodeUP rhs,
                                          uint32_t location) {
  // C99 6.5.2.1p2: the expression e1[e2] is by definition precisely
  // equivalent to the expression *((e1)+(e2)).
  // We need to figure out which expression is "base" and which is "index".

  ASTNodeUP base;
  ASTNodeUP index;
  CompilerType bad_type;

  auto lhs_type = lhs->GetDereferencedResultType();
  auto rhs_type = rhs->GetDereferencedResultType();

  if (lhs_type.IsArrayType()) {
    base = InsertArrayToPointerConversion(std::move(lhs));
    index = std::move(rhs);
  } else if (lhs_type.IsPointerType()) {
    base = std::move(lhs);
    index = std::move(rhs);
  } else if (rhs_type.IsArrayType()) {
    base = InsertArrayToPointerConversion(std::move(rhs));
    index = std::move(lhs);
  } else if (rhs_type.IsPointerType()) {
    base = std::move(rhs);
    index = std::move(lhs);
  } else {
    // Check to see if this might be a synthetic value.
    const ASTNode *ast_node = lhs.get();
    if (llvm::isa<IdentifierNode>(ast_node)) {
      const IdentifierNode* id_node =
          static_cast<const IdentifierNode*>(ast_node);
      lldb::ValueObjectSP lhs_valobj_sp = id_node->valobj()->GetSP();
      if (lhs_valobj_sp->HasSyntheticValue()) {
        base = std::move(lhs);
        index = std::move(rhs);
      } else {
        BailOut(ErrorCode::kInvalidOperandType,
                "subscripted value is not an array or pointer", location);
        return std::make_unique<ErrorNode>();
      }
    } else {
      BailOut(ErrorCode::kInvalidOperandType,
              "subscripted value is not an array or pointer", location);
      return std::make_unique<ErrorNode>();
    }
  }

  // Index can be a typedef of a typedef of a typedef of a typedef...
  // Get canonical underlying type.
  auto index_type = index->GetDereferencedResultType();

  // Check if the index is of an integral type.
  if (!index_type.IsIntegerOrUnscopedEnumerationType()) {
    BailOut(ErrorCode::kInvalidOperandType, "array subscript is not an integer",
            location);
    return std::make_unique<ErrorNode>();
  }

  auto base_type = base->GetDereferencedResultType();
  if (base_type.IsPointerToVoid()) {
    BailOut(ErrorCode::kInvalidOperandType,
            "subscript of pointer to incomplete type 'void'", location);
    return std::make_unique<ErrorNode>();
  }

  return std::make_unique<ArraySubscriptNode>(
      location, base->GetDereferencedResultType().GetPointeeType(),
      std::move(base), std::move(index));
}

ASTNodeUP DILParser::BuildMemberOf(ASTNodeUP lhs, std::string member_id,
                                   bool is_arrow, uint32_t location) {
  CompilerType bad_type;
  auto lhs_type = lhs->GetDereferencedResultType();
  lldb::ValueObjectSP lhs_valobj_sp;
  lldb::ValueObjectSP deref_sp;

  const ASTNode *ast_node = lhs.get();
  ValueObject *valobj = ast_node->valobj();
  if (valobj)
    lhs_valobj_sp = valobj->GetSP();

  std::optional<uint32_t> bitfield_size;
  ConstString field_name(member_id.c_str());
  return std::make_unique<MemberOfNode>(location, std::move(lhs), bitfield_size,
                                        is_arrow, field_name);
}

void DILParser::Expect(Token::Kind kind) {
  if (CurToken().IsNot(kind)) {
    BailOut(ErrorCode::kUnknown,
            llvm::formatv("expected {0}, got: {1}", kind, CurToken()),
            CurToken().GetLocation());
  }
}

void DILParser::ExpectOneOf(std::vector<Token::Kind> kinds_vec) {
  if (!CurToken().IsOneOf(kinds_vec)) {
    BailOut(ErrorCode::kUnknown,
            llvm::formatv("expected any of ({0}), got: {1}",
                          llvm::iterator_range(kinds_vec), CurToken()),
            CurToken().GetLocation());
  }
}

void DILParser::BailOut(ErrorCode code, const std::string& error,
                        uint32_t loc) {
  if (m_error.Fail()) {
    // If error is already set, then the parser is in the "bail-out" mode. Don't
    // do anything and keep the original error.
    return;
  }

  m_error = Status((uint32_t) code, lldb::eErrorTypeGeneric,
                   FormatDiagnostics(m_input_expr, error, loc));
  // Advance the lexer token index to the end of the lexed tokens vector.
  m_dil_lexer.ResetTokenIdx(m_dil_lexer.NumLexedTokens() - 1);
}

void DILParser::BailOut(Status error) {
  if (m_error.Fail()) {
    // If error is already set, then the parser is in the "bail-out" mode. Don't
    // do anything and keep the original error.
    return;
  }
  m_error = std::move(error);
  // Advance the lexer token index to the end of the lexed tokens vector.
  m_dil_lexer.ResetTokenIdx(m_dil_lexer.NumLexedTokens() - 1);
}

bool DILParser::ImplicitConversionIsAllowed(CompilerType src, CompilerType dst,
                                            bool is_src_literal_zero) {
  if (dst.IsInteger() || dst.IsFloat()) {
    // Arithmetic types and enumerations can be implicitly converted to integers
    // and floating point types.
    if (src.IsScalarOrUnscopedEnumerationType()
        || src.IsScopedEnumerationType()) {
      return true;
    }
  }

  if (dst.IsPointerType()) {
    // Literal zero, `nullptr_t` and arrays can be implicitly converted to
    // pointers.
    if (is_src_literal_zero || src.IsNullPtrType()) {
      return true;
    }
    if (src.IsArrayType() &&
        src.GetArrayElementType(nullptr).CompareTypes(dst.GetPointeeType())) {
      return true;
    }
  }

  return false;
}

ASTNodeUP DILParser::InsertImplicitConversion(ASTNodeUP expr,
                                              CompilerType type) {
  auto expr_type = expr->GetDereferencedResultType();

  // If the expression already has the required type, nothing to do here.
  if (expr_type.CompareTypes(type)) {
    return expr;
  }

  // Check if the implicit conversion is possible and insert a cast.
  if (ImplicitConversionIsAllowed(expr_type, type, expr->is_literal_zero())) {
    if (type.GetCanonicalType().GetBasicTypeEnumeration() !=
        lldb::eBasicTypeInvalid) {
      return std::make_unique<CStyleCastNode>(
          expr->GetLocation(), type, std::move(expr), TypePromotionCastKind::eArithmetic);
    }

    if (type.IsPointerType()) {
      return std::make_unique<CStyleCastNode>(
          expr->GetLocation(), type, std::move(expr), TypePromotionCastKind::ePointer);
    }

    // TODO: What about if the conversion is not `kArithmetic` or
    // `kPointer`?
    llvm_unreachable("invalid implicit cast kind");
  }

  BailOut(ErrorCode::kInvalidOperandType,
          llvm::formatv("no known conversion from {0} to {1}",
                        expr_type.TypeDescription(), type.TypeDescription()),
          expr->GetLocation());
  CompilerType bad_type;
  return std::make_unique<ErrorNode>();
}

lldb::BasicType TypeDeclaration::GetBasicType() const {
  assert(m_is_builtin && "type declaration doesn't describe a builtin type");

  if (m_sign_specifier == SignSpecifier::kSigned &&
      m_type_specifier == TypeSpecifier::kChar) {
    // "signed char" isn't the same as "char".
    return lldb::eBasicTypeSignedChar;
  }

  if (m_sign_specifier == SignSpecifier::kUnsigned) {
    switch (m_type_specifier) {
      // "unsigned" is "unsigned int"
      case TypeSpecifier::kUnknown:  return lldb::eBasicTypeUnsignedInt;
      case TypeSpecifier::kChar:     return lldb::eBasicTypeUnsignedChar;
      case TypeSpecifier::kShort:    return lldb::eBasicTypeUnsignedShort;
      case TypeSpecifier::kInt:      return lldb::eBasicTypeUnsignedInt;
      case TypeSpecifier::kLong:     return lldb::eBasicTypeUnsignedLong;
      case TypeSpecifier::kLongLong: return lldb::eBasicTypeUnsignedLongLong;
      default:
        assert(false && "unknown unsigned basic type");
        return lldb::eBasicTypeInvalid;
    }
  }

  switch (m_type_specifier) {
    case TypeSpecifier::kUnknown:
      // "signed" is "signed int"
      assert(m_sign_specifier == SignSpecifier::kSigned &&
             "invalid basic type declaration");
      return lldb::eBasicTypeInt;
    case TypeSpecifier::kVoid:       return lldb::eBasicTypeVoid;
    case TypeSpecifier::kBool:       return lldb::eBasicTypeBool;
    case TypeSpecifier::kChar:       return lldb::eBasicTypeChar;
    case TypeSpecifier::kShort:      return lldb::eBasicTypeShort;
    case TypeSpecifier::kInt:        return lldb::eBasicTypeInt;
    case TypeSpecifier::kLong:       return lldb::eBasicTypeLong;
    case TypeSpecifier::kLongLong:   return lldb::eBasicTypeLongLong;
    case TypeSpecifier::kFloat:      return lldb::eBasicTypeFloat;
    case TypeSpecifier::kDouble:     return lldb::eBasicTypeDouble;
    case TypeSpecifier::kLongDouble: return lldb::eBasicTypeLongDouble;
    case TypeSpecifier::kWChar:      return lldb::eBasicTypeWChar;
    case TypeSpecifier::kChar16:     return lldb::eBasicTypeChar16;
    case TypeSpecifier::kChar32:     return lldb::eBasicTypeChar32;
  }

  return lldb::eBasicTypeInvalid;
}

}  // namespace lldb_private::dil
