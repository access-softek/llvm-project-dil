//===-- DILParser.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_VALUEOBJECT_DILPARSER_H
#define LLDB_VALUEOBJECT_DILPARSER_H

#include "lldb/Target/ExecutionContextScope.h"
#include "lldb/Utility/DiagnosticsRendering.h"
#include "lldb/Utility/Status.h"
#include "lldb/ValueObject/DILAST.h"
#include "lldb/ValueObject/DILLexer.h"
#include "lldb/ValueObject/DILLiteralParsers.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>


namespace lldb_private::dil {

/// Struct to hold information about member fields. Used by the parser for the
/// Data Inspection Language (DIL).
struct MemberInfo {
  std::optional<std::string> name;
  CompilerType type;
  std::optional<uint32_t> bitfield_size_in_bits;
  bool is_synthetic;
  bool is_dynamic;
  lldb::ValueObjectSP val_obj_sp;
};

/// Finds the member field with the given name and type, stores the child index
/// corresponding to the field in the idx vector and returns a MemberInfo
/// struct with appropriate information about the field.
std::optional<MemberInfo>
GetFieldWithNameIndexPath(lldb::ValueObjectSP lhs_val_sp,
                          CompilerType type,
                          const std::string &name,
                          std::vector<uint32_t> *idx,
                          CompilerType empty_type,
                          bool use_synthetic, bool is_dynamic, bool is_arrow);

std::tuple<std::optional<MemberInfo>, std::vector<uint32_t>>
GetMemberInfo(lldb::ValueObjectSP lhs_val_sp, CompilerType type,
              const std::string &name, bool use_synthetic, bool is_arrow);

std::string TypeDescription(CompilerType type);

enum class ErrorCode : unsigned char {
  kOk = 0,
  kInvalidExpressionSyntax,
  kInvalidNumericLiteral,
  kInvalidOperandType,
  kLexerError,
  kUndeclaredIdentifier,
  kNotImplemented,
  kUBDivisionByZero,
  kUBDivisionByMinusOne,
  kUBInvalidCast,
  kUBInvalidShift,
  kUBNullPtrArithmetic,
  kUBInvalidPtrDiff,
  kSubscriptOutOfRange,
  kUnknown,
};

// The following is modeled on class OptionParseError.
class DILDiagnosticError
    : public llvm::ErrorInfo<DILDiagnosticError, DiagnosticError> {
  DiagnosticDetail m_detail;

public:
  using llvm::ErrorInfo<DILDiagnosticError, DiagnosticError>::ErrorInfo;
  DILDiagnosticError(DiagnosticDetail detail)
      : ErrorInfo(make_error_code(std::errc::invalid_argument)),
        m_detail(std::move(detail)) {}

  DILDiagnosticError(llvm::StringRef expr, const std::string &message,
                     uint32_t loc, uint16_t err_len);

  std::unique_ptr<CloneableError> Clone() const override {
    return std::make_unique<DILDiagnosticError>(m_detail);
  }

  llvm::ArrayRef<DiagnosticDetail> GetDetails() const override {
    return {m_detail};
  }

  std::string message() const override { return m_detail.rendered; }
};

Status SetUbStatus(ErrorCode code);

/// TypeDeclaration builds information about the literal type definition as
/// type is being parsed. It doesn't perform semantic analysis for non-basic
/// types -- e.g. "char&&&" is a valid type declaration.
/// NOTE: CV qualifiers are ignored.
class TypeDeclaration {
 public:
  enum class TypeSpecifier {
    kUnknown,
    kVoid,
    kBool,
    kChar,
    kShort,
    kInt,
    kLong,
    kLongLong,
    kFloat,
    kDouble,
    kLongDouble,
    kWChar,
    kChar16,
    kChar32,
  };

  enum class SignSpecifier {
    kUnknown,
    kSigned,
    kUnsigned,
  };

  bool IsEmpty() const { return !m_is_builtin && !m_is_user_type; }

  lldb::BasicType GetBasicType() const;

 public:
  // Indicates user-defined typename (e.g. "MyClass", "MyTmpl<int>").
  std::string m_user_typename;

  // Basic type specifier ("void", "char", "int", "long", "long long", etc).
  TypeSpecifier m_type_specifier = TypeSpecifier::kUnknown;

  // Signedness specifier ("signed", "unsigned").
  SignSpecifier m_sign_specifier = SignSpecifier::kUnknown;

  // Does the type declaration includes "int" specifier?
  // This is different than `type_specifier_` and is used to detect "int"
  // duplication for types that can be combined with "int" specifier (e.g.
  // "short int", "long int").
  bool m_has_int_specifier = false;

  // Indicates whether there was an error during parsing.
  bool m_has_error = false;

  // Indicates whether this declaration describes a builtin type.
  bool m_is_builtin = false;

  // Indicates whether this declaration describes a user type.
  bool m_is_user_type = false;
}; // class TypeDeclaration

class BuiltinFunctionDef {
 public:
  BuiltinFunctionDef(std::string name, CompilerType return_type,
                     std::vector<CompilerType> arguments)
      : m_name(std::move(name)),
        m_return_type(std::move(return_type)),
        m_arguments(std::move(arguments)) {}

  std::string m_name;
  CompilerType m_return_type;
  std::vector<CompilerType> m_arguments;
}; // class BuiltinFunctionDef

/// Pure recursive descent parser for C++ like expressions.
/// EBNF grammar for the parser is described in lldb/docs/dil-expr-lang.ebnf
class DILParser {
 public:
   static llvm::Expected<ASTNodeUP> Parse(llvm::StringRef dil_input_expr,
                                          DILLexer lexer,
                                          std::shared_ptr<StackFrame> frame_sp,
                                          lldb::DynamicValueType use_dynamic,
                                          bool use_synthetic, bool fragile_ivar,
                                          bool check_ptr_vs_member);

   ~DILParser() = default;

   bool UseSynthetic() { return m_use_synthetic; }

   lldb::DynamicValueType UseDynamic() { return m_use_dynamic; }

   using PtrOperator = std::tuple<Token::Kind, uint32_t>;

 private:
   explicit DILParser(llvm::StringRef dil_input_expr, DILLexer lexer,
                      std::shared_ptr<StackFrame> frame_sp,
                      lldb::DynamicValueType use_dynamic, bool use_synthetic,
                      bool fragile_ivar, bool check_ptr_vs_member,
                      llvm::Error &error);

   ASTNodeUP Run();

   ASTNodeUP ParseExpression();
   ASTNodeUP ParseAssignmentExpression();
   ASTNodeUP ParseLogicalOrExpression();
   ASTNodeUP ParseLogicalAndExpression();
   ASTNodeUP ParseInclusiveOrExpression();
   ASTNodeUP ParseExclusiveOrExpression();
   ASTNodeUP ParseAndExpression();
   ASTNodeUP ParseEqualityExpression();
   ASTNodeUP ParseRelationalExpression();
   ASTNodeUP ParseShiftExpression();
   ASTNodeUP ParseAdditiveExpression();
   ASTNodeUP ParseMultiplicativeExpression();
   ASTNodeUP ParseCastExpression();
   ASTNodeUP ParseUnaryExpression();
   ASTNodeUP ParsePostfixExpression();
   ASTNodeUP ParsePrimaryExpression();

   std::optional<CompilerType> ParseTypeId(bool must_be_type_id = false);
   void ParseTypeSpecifierSeq(TypeDeclaration *type_decl);
   bool ParseTypeSpecifier(TypeDeclaration *type_decl);
   std::string ParseNestedNameSpecifier();
   std::string ParseTypeName();

   std::string ParseTemplateArgumentList();
   std::string ParseTemplateArgument();

   PtrOperator ParsePtrOperator();
   CompilerType
   ResolveTypeDeclarators(CompilerType type,
                          const std::vector<PtrOperator> &ptr_operators);

   bool IsSimpleTypeSpecifierKeyword(Token token) const;
   bool IsCvQualifier(Token token) const;
   bool IsPtrOperator(Token token) const;
   bool HandleSimpleTypeSpecifier(TypeDeclaration *type_decl);

   std::string ParseIdExpression();
   std::string ParseUnqualifiedId();
   ASTNodeUP ParseNumericLiteral();
   ASTNodeUP ParseBooleanLiteral();
   ASTNodeUP ParseCharLiteral();
   ASTNodeUP ParseStringLiteral();
   ASTNodeUP ParsePointerLiteral();
   ASTNodeUP ParseNumericConstant();
   ASTNodeUP ParseFloatingLiteral(NumericLiteralParser &literal, Token &token);
   ASTNodeUP ParseIntegerLiteral(NumericLiteralParser &literal, Token &token);
   ASTNodeUP ParseBuiltinFunction(uint32_t loc,
                                  std::unique_ptr<BuiltinFunctionDef> func_def);

   bool ImplicitConversionIsAllowed(CompilerType src, CompilerType dst,
                                    bool is_src_literal_zero = false);
   ASTNodeUP InsertImplicitConversion(ASTNodeUP expr, CompilerType type);

   void BailOut(const std::string &error, uint32_t loc, uint16_t err_len);

   void Expect(Token::Kind kind);

   void ExpectOneOf(std::vector<Token::Kind> kinds_vec);

   ASTNodeUP BuildCStyleCast(CompilerType type, ASTNodeUP rhs,
                             uint32_t location);
   ASTNodeUP BuildCxxCast(Token::Kind kind, CompilerType type, ASTNodeUP rhs,
                          uint32_t location);
   ASTNodeUP BuildCxxDynamicCast(CompilerType type, ASTNodeUP rhs,
                                 uint32_t location);
   ASTNodeUP BuildCxxStaticCast(CompilerType type, ASTNodeUP rhs,
                                uint32_t location);
   ASTNodeUP BuildCxxStaticCastToScalar(CompilerType type, ASTNodeUP rhs,
                                        uint32_t location);
   ASTNodeUP BuildCxxStaticCastToEnum(CompilerType type, ASTNodeUP rhs,
                                      uint32_t location);
   ASTNodeUP BuildCxxStaticCastToPointer(CompilerType type, ASTNodeUP rhs,
                                         uint32_t location);
   ASTNodeUP BuildCxxStaticCastToNullPtr(CompilerType type, ASTNodeUP rhs,
                                         uint32_t location);
   ASTNodeUP BuildCxxStaticCastToReference(CompilerType type, ASTNodeUP rhs,
                                           uint32_t location);
   ASTNodeUP BuildCxxStaticCastForInheritedTypes(CompilerType type,
                                                 ASTNodeUP rhs,
                                                 uint32_t location);
   ASTNodeUP BuildCxxReinterpretCast(CompilerType type, ASTNodeUP rhs,
                                     uint32_t location);
   ASTNodeUP BuildUnaryOp(UnaryOpKind kind, ASTNodeUP rhs, uint32_t location);
   ASTNodeUP BuildIncrementDecrement(UnaryOpKind kind, ASTNodeUP rhs,
                                     uint32_t location);
   ASTNodeUP BuildBinaryOp(BinaryOpKind kind, ASTNodeUP lhs, ASTNodeUP rhs,
                           uint32_t location);
   CompilerType PrepareBinaryAddition(ASTNodeUP &lhs, ASTNodeUP &rhs,
                                      uint32_t location, bool is_comp_assign);
   CompilerType PrepareBinarySubtraction(ASTNodeUP &lhs, ASTNodeUP &rhs,
                                         uint32_t location,
                                         bool is_comp_assign);
   CompilerType PrepareBinaryMulDiv(ASTNodeUP &lhs, ASTNodeUP &rhs,
                                    bool is_comp_assign);
   CompilerType PrepareBinaryRemainder(ASTNodeUP &lhs, ASTNodeUP &rhs,
                                       bool is_comp_assign);
   CompilerType PrepareBinaryBitwise(ASTNodeUP &lhs, ASTNodeUP &rhs,
                                     bool is_comp_assign);
   CompilerType PrepareBinaryShift(ASTNodeUP &lhs, ASTNodeUP &rhs,
                                   bool is_comp_assign);
   CompilerType PrepareBinaryComparison(BinaryOpKind kind, ASTNodeUP &lhs,
                                        ASTNodeUP &rhs, uint32_t location);
   CompilerType PrepareBinaryLogical(const ASTNodeUP &lhs,
                                     const ASTNodeUP &rhs);
   ASTNodeUP BuildBinarySubscript(ASTNodeUP lhs, ASTNodeUP rhs,
                                  uint32_t location);
   CompilerType PrepareCompositeAssignment(CompilerType comp_assign_type,
                                           const ASTNodeUP &lhs,
                                           uint32_t location);
   ASTNodeUP BuildTernaryOp(ASTNodeUP cond, ASTNodeUP lhs, ASTNodeUP rhs,
                            uint32_t location);
   ASTNodeUP BuildMemberOf(ASTNodeUP lhs, std::string member_id, bool is_arrow,
                           uint32_t location);

   bool AllowSideEffects() const { return m_allow_side_effects; }

   void SetAllowSideEffects(bool allow_side_effects) {
     m_allow_side_effects = allow_side_effects;
   }

  void TentativeParsingRollback(uint32_t saved_idx) {
    if (m_error)
      llvm::consumeError(std::move(m_error));
    m_dil_lexer.ResetTokenIdx(saved_idx);
  }

  Token CurToken() { return m_dil_lexer.GetCurrentToken(); }

  // Parser doesn't own the evaluation context. The produced AST may depend on
  // it (for example, for source locations), so it's expected that expression
  // context will outlive the parser.
  std::shared_ptr<StackFrame> m_ctx_scope;

  llvm::StringRef m_input_expr;

  DILLexer m_dil_lexer;
  // Holds an error if it occures during parsing.
  llvm::Error &m_error;

  bool m_allow_side_effects = true;

  lldb::DynamicValueType m_use_dynamic;
  bool m_use_synthetic;
  bool m_fragile_ivar;
  bool m_check_ptr_vs_member;
}; // class DILParser

}  // namespace lldb_private::dil

namespace llvm {
template <>
struct format_provider<lldb_private::dil::TypeDeclaration::TypeSpecifier> {
  static void format(const lldb_private::dil::TypeDeclaration::TypeSpecifier &t,
                     raw_ostream &OS, llvm::StringRef Options) {
    switch (t) {
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kVoid:
      OS << "void";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kBool:
      OS << "bool";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kChar:
      OS << "char";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kShort:
      OS << "short";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kInt:
      OS << "int";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kLong:
      OS << "long";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kLongLong:
      OS << "long long";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kFloat:
      OS << "float";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kDouble:
      OS << "double";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kLongDouble:
      OS << "long double";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kWChar:
      OS << "wchar_t";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kChar16:
      OS << "char16_t";
      break;
    case lldb_private::dil::TypeDeclaration::TypeSpecifier::kChar32:
      OS << "char32_t";
      break;
    default:
      OS << "invalid type specifier";
      break;
    }
  }
};

template <>
struct format_provider<lldb_private::dil::TypeDeclaration::SignSpecifier> {
  static void format(const lldb_private::dil::TypeDeclaration::SignSpecifier &t,
                     raw_ostream &OS, llvm::StringRef Options) {
    switch (t) {
    case lldb_private::dil::TypeDeclaration::SignSpecifier::kSigned:
      OS << "signed";
      break;
    case lldb_private::dil::TypeDeclaration::SignSpecifier::kUnsigned:
      OS << "unsigned";
      break;
    default:
      OS << "invalid sign specifier";
      break;
    }
  }
};
} // namespace llvm

#endif  // LLDB_VALUEOBJECT_DILPARSER_H
