//===-- DILEval.cpp -------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/ValueObject/DILEval.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/ValueObject/DILAST.h"
#include "lldb/ValueObject/ValueObject.h"
#include "lldb/ValueObject/ValueObjectRegister.h"
#include "lldb/ValueObject/ValueObjectVariable.h"
#include "lldb/lldb-enumerations.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/Support/FormatAdapters.h"
#include "llvm/Support/FormatVariadic.h"
#include <memory>

namespace {
const char *kInvalidOperandsToUnaryExpression =
    "invalid argument type {0} to unary expression";

const char *kInvalidOperandsToBinaryExpression =
    "invalid operands to binary expression ({0} and {1})";

const char *kValueIsNotConvertibleToBool =
    "value of type {0} is not contextually convertible to 'bool'";
} // namespace

namespace lldb_private::dil {

llvm::Error Interpreter::BailOut(ErrorCode code, const std::string &message,
                                 uint32_t loc) {
  Status error = Status((uint32_t)code, lldb::eErrorTypeGeneric,
                        FormatDiagnostics(m_expr, message, loc));
  return error.ToError();
}

lldb::ValueObjectSP
GetDynamicOrSyntheticValue(lldb::ValueObjectSP in_valobj_sp,
                           lldb::DynamicValueType use_dynamic,
                           bool use_synthetic) {
  Status error;
  if (!in_valobj_sp) {
    error = Status("invalid value object");
    return in_valobj_sp;
  }
  lldb::ValueObjectSP value_sp = in_valobj_sp;
  Target *target = value_sp->GetTargetSP().get();
  // If this ValueObject holds an error, then it is valuable for that.
  if (value_sp->GetError().Fail())
    return value_sp;

  if (!target)
    return lldb::ValueObjectSP();

  if (use_dynamic != lldb::eNoDynamicValues) {
    lldb::ValueObjectSP dynamic_sp = value_sp->GetDynamicValue(use_dynamic);
    if (dynamic_sp)
      value_sp = dynamic_sp;
  }

  if (use_synthetic) {
    lldb::ValueObjectSP synthetic_sp = value_sp->GetSyntheticValue();
    if (synthetic_sp)
      value_sp = synthetic_sp;
  }

  if (!value_sp)
    error = Status("invalid value object");

  return value_sp;
}

template <typename T> bool Compare(BinaryOpKind kind, const T &l, const T &r) {
  switch (kind) {
    case BinaryOpKind::EQ:
      return l == r;
    case BinaryOpKind::NE:
      return l != r;
    case BinaryOpKind::LT:
      return l < r;
    case BinaryOpKind::LE:
      return l <= r;
    case BinaryOpKind::GT:
      return l > r;
    case BinaryOpKind::GE:
      return l >= r;

    default:
      assert(false && "invalid ast: invalid comparison operation");
      return false;
  }
}

static uint64_t GetUInt64(lldb::ValueObjectSP value_sp) {
  // GetValueAsUnsigned performs overflow according to the underlying type.
  // For example, if the underlying type is `int32_t` and the value is `-1`,
  // GetValueAsUnsigned will return 4294967295.
  return value_sp->GetCompilerType().IsSigned()
      ? value_sp->GetValueAsSigned(0)
      : value_sp->GetValueAsUnsigned(0);
}

static lldb::ValueObjectSP EvaluateArithmeticOpInteger(lldb::TargetSP target,
                                                       BinaryOpKind kind,
                                                       lldb::ValueObjectSP lhs,
                                                       lldb::ValueObjectSP rhs,
                                                       CompilerType rtype)
{
  assert(lhs->GetCompilerType().IsInteger() &&
         rhs->GetCompilerType().IsInteger() &&
         "invalid ast: both operands must be integers");
  assert((kind == BinaryOpKind::Shl || kind == BinaryOpKind::Shr ||
          lhs->GetCompilerType().CompareTypes(rhs->GetCompilerType())) &&
         "invalid ast: operands must have the same type");

  auto wrap = [target, rtype](auto value) {
    return ValueObject::CreateValueObjectFromAPInt(target, value, rtype,
                                                   "result");
  };

  llvm::Expected<llvm::APSInt> l_value = lhs->GetValueAsAPSInt();
  llvm::Expected<llvm::APSInt> r_value = rhs->GetValueAsAPSInt();

  if (l_value && r_value) {
    llvm::APSInt l = *l_value;
    llvm::APSInt r = *r_value;

    switch (kind) {
      case BinaryOpKind::Add:
        return wrap(l + r);
      case BinaryOpKind::Sub:
        return wrap(l - r);
      case BinaryOpKind::Div:
        return wrap(l / r);
      case BinaryOpKind::Mul:
        return wrap(l * r);
      case BinaryOpKind::Rem:
        return wrap(l % r);
      case BinaryOpKind::And:
        return wrap(l & r);
      case BinaryOpKind::Or:
        return wrap(l | r);
      case BinaryOpKind::Xor:
        return wrap(l ^ r);
      case BinaryOpKind::Shl:
        return wrap(l.shl(r));
      case BinaryOpKind::Shr:
        // Apply arithmetic shift on signed values and logical shift operation
        // on unsigned values.
        return wrap(l.isSigned() ? l.ashr(r) : l.lshr(r));

      default:
        assert(false && "invalid ast: invalid arithmetic operation");
        return lldb::ValueObjectSP();
    }
  } else {
    return lldb::ValueObjectSP();
  }
}

static lldb::ValueObjectSP EvaluateArithmeticOpFloat(lldb::TargetSP target,
                                                     BinaryOpKind kind,
                                                     lldb::ValueObjectSP lhs,
                                                     lldb::ValueObjectSP rhs,
                                                     CompilerType rtype) {
  assert((lhs->GetCompilerType().IsFloat() &&
          lhs->GetCompilerType().CompareTypes(rhs->GetCompilerType())) &&
         "invalid ast: operands must be floats and have the same type");

  auto wrap = [target, rtype](auto value) {
    return ValueObject::CreateValueObjectFromAPFloat(target, value, rtype, "result");
  };

  auto lval_or_err = lhs->GetValueAsAPFloat();
  auto rval_or_err = rhs->GetValueAsAPFloat();
  if (lval_or_err && rval_or_err) {
    llvm::APFloat l = *lval_or_err;
    llvm::APFloat r = *rval_or_err;

    switch (kind) {
      case BinaryOpKind::Add:
        return wrap(l + r);
      case BinaryOpKind::Sub:
        return wrap(l - r);
      case BinaryOpKind::Div:
        return wrap(l / r);
      case BinaryOpKind::Mul:
        return wrap(l * r);

      default:
        assert(false && "invalid ast: invalid arithmetic operation");
        return lldb::ValueObjectSP();
    }
  }
  return lldb::ValueObjectSP();
}

static lldb::ValueObjectSP EvaluateArithmeticOp(lldb::TargetSP target,
                                                BinaryOpKind kind,
                                                lldb::ValueObjectSP lhs,
                                                lldb::ValueObjectSP rhs,
                                                CompilerType rtype) {
  assert((rtype.IsInteger() || rtype.IsFloat()) &&
         "invalid ast: result type must either integer or floating point");

  // Evaluate arithmetic operation for two integral values.
  if (rtype.IsInteger()) {
    return EvaluateArithmeticOpInteger(target, kind, lhs, rhs, rtype);
  }

  // Evaluate arithmetic operation for two floating point values.
  if (rtype.IsFloat()) {
    return EvaluateArithmeticOpFloat(target, kind, lhs, rhs, rtype);
  }

  return lldb::ValueObjectSP();
}

static bool IsInvalidDivisionByMinusOne(lldb::ValueObjectSP lhs_sp,
                                        lldb::ValueObjectSP rhs_sp)
{
  assert(lhs_sp->GetCompilerType().IsInteger() &&
         rhs_sp->GetCompilerType().IsInteger() && "operands should be integers");

  // The result type should be signed integer.
  auto basic_type =
      rhs_sp->GetCompilerType().GetCanonicalType().GetBasicTypeEnumeration();
  if (basic_type != lldb::eBasicTypeInt && basic_type != lldb::eBasicTypeLong &&
      basic_type != lldb::eBasicTypeLongLong) {
    return false;
  }

  // The RHS should be equal to -1.
  if (rhs_sp->GetValueAsSigned(0) != -1) {
    return false;
  }

  // The LHS should be equal to the minimum value the result type can hold.
  uint64_t byte_size = 0;
  if (auto temp = rhs_sp->GetCompilerType().GetByteSize(rhs_sp->GetTargetSP().get()))
    byte_size = temp.value();
  auto bit_size = byte_size * CHAR_BIT;
  return lhs_sp->GetValueAsSigned(0) + (1LLU << (bit_size - 1)) == 0;
}

Status SetUbStatus(ErrorCode code) {
  llvm::StringRef err_str;
  switch ((int) code) {
    case (int) ErrorCode::kUBDivisionByZero:
      err_str ="Error: Division by zero detected.";
      break;
    case (int) ErrorCode::kUBDivisionByMinusOne:
      // If "a / b" isn't representable in its result type, then results of
      // "a / b" and "a % b" are undefined behaviour. This happens when "a"
      // is equal to the minimum value of the result type and "b" is equal
      // to -1.
      err_str ="Error: Invalid division by negative one  detected.";
      break;
    case (int) ErrorCode::kUBInvalidCast:
      err_str ="Error: Invalid type cast detected.";
      break;
    case (int) ErrorCode::kUBInvalidShift:
      err_str ="Error: Invalid shift detected.";
      break;
    case (int) ErrorCode::kUBNullPtrArithmetic:
      err_str ="Error: Attempt to perform arithmetic with null ptr  detected.";
      break;
    case (int) ErrorCode::kUBInvalidPtrDiff:
      err_str ="Error: Attempt to perform invalid ptr arithmetic detected.";
      break;
    default:
      err_str ="Error: Unknown undefined behavior error.";
      break;
  }
  return Status(err_str.str());
}

static lldb::ValueObjectSP LookupStaticIdentifier(
    VariableList &variable_list, std::shared_ptr<StackFrame> exe_scope,
    llvm::StringRef name_ref, llvm::StringRef unqualified_name) {
  // First look for an exact match (to the possibly qualified name)
  for (const lldb::VariableSP &var_sp : variable_list) {
    lldb::ValueObjectSP valobj_sp(
        ValueObjectVariable::Create(exe_scope.get(), var_sp));
    if (valobj_sp && valobj_sp->GetVariable() &&
        (valobj_sp->GetVariable()->NameMatches(ConstString(name_ref))))
      return valobj_sp;
  }

  // If the qualified name is the same as the unqualified name there's nothing
  // more to be done.
  if (name_ref == unqualified_name)
    return nullptr;

  // We didn't match the qualified name; try to match the unqualified name.
  for (const lldb::VariableSP &var_sp : variable_list) {
    lldb::ValueObjectSP valobj_sp(
        ValueObjectVariable::Create(exe_scope.get(), var_sp));
    if (valobj_sp && valobj_sp->GetVariable() &&
        (valobj_sp->GetVariable()->NameMatches(ConstString(unqualified_name))))
      return valobj_sp;
  }
  return nullptr;
}

struct EnumMember {
  CompilerType type;
  ConstString name;
  llvm::APSInt value;
};

static std::vector<EnumMember> GetEnumMembers(CompilerType type) {
  std::vector<EnumMember> enum_member_list;
  if (type.IsValid()) {
    type.ForEachEnumerator(
        [&enum_member_list](const CompilerType &integer_type, ConstString name,
                            const llvm::APSInt &value) -> bool {
          EnumMember enum_member = {integer_type, name, value};
          enum_member_list.push_back(enum_member);
          return true; // Keep iterating
        });
  }
  return enum_member_list;
}

CompilerType
ResolveTypeByName(const std::string &name,
                  std::shared_ptr<ExecutionContextScope> ctx_scope) {
  // Internally types don't have global scope qualifier in their names and
  // LLDB doesn't support queries with it too.
  llvm::StringRef name_ref(name);

  if (name_ref.starts_with("::"))
    name_ref = name_ref.drop_front(2);

  std::vector<CompilerType> result_type_list;
  lldb::TargetSP target_sp = ctx_scope->CalculateTarget();
  const char *type_name = name_ref.data();
  if (type_name && type_name[0] && target_sp) {
    ModuleList &images = target_sp->GetImages();
    ConstString const_type_name(type_name);
    TypeQuery query(type_name);
    TypeResults results;
    images.FindTypes(nullptr, query, results);
    for (const lldb::TypeSP &type_sp : results.GetTypeMap().Types())
      if (type_sp)
        result_type_list.push_back(type_sp->GetFullCompilerType());

    if (auto process_sp = target_sp->GetProcessSP()) {
      for (auto *runtime : process_sp->GetLanguageRuntimes()) {
        if (auto *vendor = runtime->GetDeclVendor()) {
          auto types = vendor->FindTypes(const_type_name, UINT32_MAX);
          for (auto type : types)
            result_type_list.push_back(type);
        }
      }
    }

    if (result_type_list.empty()) {
      for (auto type_system_sp : target_sp->GetScratchTypeSystems())
        if (auto compiler_type =
                type_system_sp->GetBuiltinTypeByName(const_type_name))
          result_type_list.push_back(compiler_type);
    }
  }

  // We've found multiple types, try finding the "correct" one.
  CompilerType full_match;
  std::vector<CompilerType> partial_matches;

  for (uint32_t i = 0; i < result_type_list.size(); ++i) {
    CompilerType type = result_type_list[i];
    llvm::StringRef type_name_ref = type.GetTypeName().GetStringRef();
    ;

    if (type_name_ref == name_ref)
      full_match = type;
    else if (type_name_ref.ends_with(name_ref))
      partial_matches.push_back(type);
  }

  // Full match is always correct.
  if (full_match.IsValid())
    return full_match;

  // If we have partial matches, pick a "random" one.
  if (partial_matches.size() > 0)
    return partial_matches.back();

  return {};
}

static lldb::VariableSP DILFindVariable(ConstString name,
                                        lldb::VariableListSP variable_list) {
  lldb::VariableSP exact_match;
  std::vector<lldb::VariableSP> possible_matches;

  for (lldb::VariableSP var_sp : *variable_list) {
    llvm::StringRef str_ref_name = var_sp->GetName().GetStringRef();
    // Check for global vars, which might start with '::'.
    str_ref_name.consume_front("::");

    if (str_ref_name == name.GetStringRef())
      possible_matches.push_back(var_sp);
    else if (var_sp->NameMatches(name))
      possible_matches.push_back(var_sp);
  }

  // Look for exact matches (favors local vars over global vars)
  auto exact_match_it =
      llvm::find_if(possible_matches, [&](lldb::VariableSP var_sp) {
        return var_sp->GetName() == name;
      });

  if (exact_match_it != possible_matches.end())
    return *exact_match_it;

  // Look for a global var exact match.
  for (auto var_sp : possible_matches) {
    llvm::StringRef str_ref_name = var_sp->GetName().GetStringRef();
    str_ref_name.consume_front("::");
    if (str_ref_name == name.GetStringRef())
      return var_sp;
  }

  // If there's a single non-exact match, take it.
  if (possible_matches.size() == 1)
    return possible_matches[0];

  return nullptr;
}

lldb::ValueObjectSP LookupGlobalIdentifier(
    llvm::StringRef name_ref, std::shared_ptr<StackFrame> stack_frame,
    lldb::TargetSP target_sp, lldb::DynamicValueType use_dynamic,
    CompilerType *scope_ptr) {
  // First look for match in "local" global variables
  lldb::VariableListSP variable_list(stack_frame->GetInScopeVariableList(true));
  name_ref.consume_front("::");

  lldb::ValueObjectSP value_sp;
  if (variable_list) {
    lldb::VariableSP var_sp =
        DILFindVariable(ConstString(name_ref), variable_list);
    if (var_sp)
      value_sp =
          stack_frame->GetValueObjectForFrameVariable(var_sp, use_dynamic);
  }
  if (value_sp)
    return value_sp;

  // Also check for static global vars.
  if (variable_list) {
    const char *type_name = "";
    if (scope_ptr)
      type_name = scope_ptr->GetCanonicalType().GetTypeName().AsCString();
    std::string name_with_type_prefix =
        llvm::formatv("{0}::{1}", type_name, name_ref).str();
    value_sp = LookupStaticIdentifier(*variable_list, stack_frame,
                                      name_with_type_prefix, name_ref);

    if (!value_sp)
      value_sp = LookupStaticIdentifier(*variable_list, stack_frame, name_ref,
                                        name_ref);
  }

  if (value_sp)
    return value_sp;

  // Check for match in modules global variables.
  VariableList modules_var_list;
  target_sp->GetImages().FindGlobalVariables(
      ConstString(name_ref), std::numeric_limits<uint32_t>::max(),
      modules_var_list);
  if (modules_var_list.Empty())
    return nullptr;

  for (const lldb::VariableSP &var_sp : modules_var_list) {
    std::string qualified_name = llvm::formatv("::{0}", name_ref).str();
    if (var_sp->NameMatches(ConstString(name_ref)) ||
        var_sp->NameMatches(ConstString(qualified_name))) {
      value_sp = ValueObjectVariable::Create(stack_frame.get(), var_sp);
      break;
    }
  }

  if (value_sp)
    return value_sp;

  return nullptr;
}

lldb::ValueObjectSP LookupIdentifier(llvm::StringRef name_ref,
                                     std::shared_ptr<StackFrame> stack_frame,
                                     lldb::DynamicValueType use_dynamic,
                                     CompilerType *scope_ptr) {
  lldb::ValueObjectSP value_sp;
  // Support $rax as a special syntax for accessing registers.
  // Will return an invalid value in case the requested register doesn't exist.
  if (name_ref.consume_front("$")) {
    lldb::RegisterContextSP reg_ctx(stack_frame->GetRegisterContext());
    if (!reg_ctx)
      return nullptr;

    if (const RegisterInfo *reg_info = reg_ctx->GetRegisterInfoByName(name_ref))
      value_sp =
          ValueObjectRegister::Create(stack_frame.get(), reg_ctx, reg_info);

    if (value_sp)
      return value_sp;

    return nullptr;
  }

  lldb::VariableListSP variable_list(
      stack_frame->GetInScopeVariableList(false));

  if (!name_ref.contains("::")) {
    if (!scope_ptr || !scope_ptr->IsValid()) {
      // Lookup in the current frame.
      // Try looking for a local variable in current scope.
      if (variable_list) {
        lldb::VariableSP var_sp =
            DILFindVariable(ConstString(name_ref), variable_list);
        if (var_sp)
          value_sp =
              stack_frame->GetValueObjectForFrameVariable(var_sp, use_dynamic);
      }
      if (!value_sp)
        value_sp = stack_frame->FindVariable(ConstString(name_ref));

      if (value_sp)
        return value_sp;

      // Try looking for an instance variable (class member).
      SymbolContext sc = stack_frame->GetSymbolContext(
          lldb::eSymbolContextFunction | lldb::eSymbolContextBlock);
      llvm::StringRef ivar_name = sc.GetInstanceVariableName();
      value_sp = stack_frame->FindVariable(ConstString(ivar_name));
      if (value_sp)
        value_sp = value_sp->GetChildMemberWithName(name_ref);

      if (value_sp)
        return value_sp;
    }
  }

  // Try looking up enum value.
  if (!value_sp && name_ref.contains("::")) {
    auto [enum_typename, enumerator_name] = name_ref.rsplit("::");

    auto type = ResolveTypeByName(enum_typename.str(), stack_frame);
    std::vector<EnumMember> enum_members = GetEnumMembers(type);

    for (size_t i = 0; i < enum_members.size(); i++) {
      EnumMember enum_member = enum_members[i];
      if (enum_member.name == enumerator_name) {
        uint64_t bytes = enum_member.value.getZExtValue();
        uint64_t byte_size = 0;
        if (auto temp = type.GetByteSize(stack_frame.get()))
          byte_size = temp.value();
        lldb::TargetSP target_sp = stack_frame->CalculateTarget();
        lldb::DataExtractorSP data_sp = std::make_shared<DataExtractor>(
            &bytes, byte_size, target_sp->GetArchitecture().GetByteOrder(),
            static_cast<uint8_t>(
                target_sp->GetArchitecture().GetAddressByteSize()));
        ExecutionContext exe_ctx(
            ExecutionContextRef(ExecutionContext(target_sp.get(), false)));
        value_sp = ValueObject::CreateValueObjectFromData("result", *data_sp,
                                                          exe_ctx, type);
        break;
      }
    }
  }

  if (value_sp)
    return value_sp;

  return nullptr;
}

Interpreter::Interpreter(lldb::TargetSP target, llvm::StringRef expr,
                         lldb::DynamicValueType use_dynamic,
                         std::shared_ptr<StackFrame> frame_sp)
    : m_target(std::move(target)), m_expr(expr), m_default_dynamic(use_dynamic),
      m_exe_ctx_scope(frame_sp) {}

void Interpreter::SetContextVars(
    std::unordered_map<std::string, lldb::ValueObjectSP> context_vars) {
  m_context_vars = std::move(context_vars);
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::DILEval(const ASTNode *tree, lldb::TargetSP target_sp) {
  // Evaluate an AST.
  auto value_or_error = DILEvalNode(tree);

  // Return the computed result-or-error.
  return value_or_error;
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::DILEvalNode(const ASTNode *node, FlowAnalysis *flow) {
  // Set up the evaluation context for the current node.
  m_flow_analysis_chain.push_back(flow);
  // Traverse an AST pointed by the `node`.
  auto value_or_error = node->Accept(this);
  // Cleanup the context.
  m_flow_analysis_chain.pop_back();
  // Return the computed value-or-error. The caller is responsible for
  // checking if an error occured during the evaluation.
  return value_or_error;
}

lldb::ValueObjectSP
Interpreter::EvaluateMemberOf(lldb::ValueObjectSP value,
                              const std::vector<uint32_t> &path,
                              bool use_synthetic, bool is_dynamic) {
  // The given `value` can be a pointer, but GetChildAtIndex works for pointers
  // too, so we don't need to dereference it explicitely. This also avoid having
  // an "ephemeral" parent lldb::ValueObjectSP, representing the dereferenced
  // value.
  lldb::ValueObjectSP member_val_sp = value;

  lldb::DynamicValueType use_dynamic =
      (!is_dynamic) ? lldb::eNoDynamicValues : lldb::eDynamicDontRunTarget;
  for (uint32_t idx : path) {
    member_val_sp = member_val_sp->GetChildAtIndex(idx, /*can_create*/ true);
  }
  if (!member_val_sp && is_dynamic) {
    lldb::ValueObjectSP dyn_val_sp = value->GetDynamicValue(use_dynamic);
    if (dyn_val_sp) {
      for (uint32_t idx : path) {
        dyn_val_sp = dyn_val_sp->GetChildAtIndex(idx, true);
      }
      member_val_sp = dyn_val_sp;
    }
  }
  assert(member_val_sp && "invalid ast: invalid member access");

  // If value is a reference, derefernce it to get to the underlying type. All
  // operations on a reference should be actually operations on the referent.
  Status error;
  if (member_val_sp->GetCompilerType().IsReferenceType()) {
    member_val_sp = member_val_sp->Dereference(error);
    assert(member_val_sp && error.Success() &&
           "unable to dereference member val");
  }

  return member_val_sp;
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const ScalarLiteralNode *node) {
  CompilerType result_type = node->result_type();
  Scalar value = node->GetValue();
  if (result_type.IsBoolean()) {
    unsigned int int_val = value.UInt();
    bool b_val = false;
    if (int_val == 1)
      b_val = true;
    return ValueObject::CreateValueObjectFromBool(m_target, b_val, "result");
  }

  if (result_type.IsFloat()) {
    llvm::APFloat val = value.GetAPFloat();
    return ValueObject::CreateValueObjectFromAPFloat(m_target, val, result_type,
                                                     "result");
  }

  if (result_type.IsInteger() || result_type.IsNullPtrType() ||
      result_type.IsPointerType()) {
    llvm::APInt val = value.GetAPSInt();
    return ValueObject::CreateValueObjectFromAPInt(m_target, val, result_type,
                                                   "result");
  }

  return lldb::ValueObjectSP();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const StringLiteralNode *node) {
  CompilerType result_type = node->result_type();
  std::string val = node->GetValue();
  ExecutionContext exe_ctx(m_target.get(), false);
  uint64_t byte_size = 0;
  if (auto temp = result_type.GetByteSize(m_target.get()))
    byte_size = temp.value();
  lldb::DataExtractorSP data_sp = std::make_shared<DataExtractor>(
      reinterpret_cast<const void*>(val.data()), byte_size,
      exe_ctx.GetByteOrder(), exe_ctx.GetAddressByteSize());
  return ValueObject::CreateValueObjectFromData("result", *data_sp, exe_ctx,
                                                result_type);
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const IdentifierNode *node) {
  lldb::DynamicValueType use_dynamic = node->GetUseDynamic();

  lldb::ValueObjectSP identifier =
      LookupIdentifier(node->GetName(), m_exe_ctx_scope, use_dynamic);

  if (!identifier)
    identifier = LookupGlobalIdentifier(node->GetName(), m_exe_ctx_scope,
                                        m_target, use_dynamic);

  if (!identifier) {
    std::string errMsg =
        llvm::formatv("use of undeclared identifier '{0}'", node->GetName());
    Status error = Status(
        (uint32_t)ErrorCode::kUndeclaredIdentifier, lldb::eErrorTypeGeneric,
        FormatDiagnostics(m_expr, errMsg, node->GetLocation()));
    return error.ToError();
  }

  if (identifier->GetCompilerType().IsReferenceType()) {
    Status error;
    identifier = identifier->Dereference(error);
    if (error.Fail())
      return error.ToError();
  }

  return identifier;
}

llvm::Expected<lldb::ValueObjectSP> Interpreter::Visit(const SizeOfNode *node) {
  auto operand = node->operand();

  uint64_t deref_byte_size = 0;
  uint64_t other_byte_size = 0;
  if (auto temp = operand.GetNonReferenceType().GetByteSize(m_target.get()))
    deref_byte_size = temp.value();
  if (auto temp = operand.GetByteSize(m_target.get()))
    other_byte_size = temp.value();
  // For reference type (int&) we need to look at the referenced type.
  size_t size = operand.IsReferenceType()
                ? deref_byte_size
                : other_byte_size;
  CompilerType type = node->GetDereferencedResultType();
  ExecutionContext exe_ctx(m_target.get(), false);
  uint64_t byte_size = 0;
  if (auto temp = type.GetByteSize(m_target.get()))
    byte_size = temp.value();
  lldb::DataExtractorSP data_sp = std::make_shared<DataExtractor>(
      reinterpret_cast<const void*>(&size), byte_size,
      exe_ctx.GetByteOrder(), exe_ctx.GetAddressByteSize());
  return ValueObject::CreateValueObjectFromData("result", *data_sp, exe_ctx,
                                                type);
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const BuiltinFunctionCallNode *node) {
  if (node->name() == "__log2") {
    assert(node->arguments().size() == 1 &&
           "invalid ast: expected exactly one argument to `__log2`");
    // Get the first (and the only) argument and evaluate it.
    auto &arg = node->arguments()[0];
    auto val_or_err = DILEvalNode(arg.get());
    if (!val_or_err) {
      return val_or_err;
    }
    lldb::ValueObjectSP val = *val_or_err;
    assert(val->GetCompilerType().IsInteger() &&
           "invalid ast: argument to __log2 must be an interger");

    // Use Log2_32 to match the behaviour of Visual Studio debugger.
    uint32_t ret =
        llvm::Log2_32(static_cast<uint32_t>(val->GetValueAsUnsigned(0)));
    CompilerType target_type;
    for (auto type_system_sp : m_target->GetScratchTypeSystems())
      if (auto compiler_type =
          type_system_sp->GetBasicTypeFromAST(lldb::eBasicTypeUnsignedInt)) {
        target_type = compiler_type;
        break;
      }

    ExecutionContext exe_ctx(m_target.get(), false);
    uint64_t byte_size = 0;
    if (auto temp = target_type.GetByteSize(m_target.get()))
      byte_size = temp.value();
    lldb::DataExtractorSP data_sp = std::make_shared<DataExtractor>(
        reinterpret_cast<const void*>(&ret), byte_size,
        exe_ctx.GetByteOrder(), exe_ctx.GetAddressByteSize());
    return ValueObject::CreateValueObjectFromData("result", *data_sp, exe_ctx,
                                                  target_type);
  }

  if (node->name() == "__findnonnull") {
    assert(node->arguments().size() == 2 &&
           "invalid ast: expected exactly two arguments to `__findnonnull`");

    auto &arg1 = node->arguments()[0];
    auto val_or_err = DILEvalNode(arg1.get());
    if (!val_or_err) {
      return val_or_err;
    }
    lldb::ValueObjectSP val1_sp = *val_or_err;

    // Resolve data address for the first argument.
    uint64_t addr;

    if (val1_sp->GetCompilerType().IsPointerType()) {
      addr = val1_sp->GetValueAsUnsigned(0);
    } else if (val1_sp->GetCompilerType().IsArrayType()) {
      addr = val1_sp->GetLoadAddress();
    } else {
      Status error = Status(
          (uint32_t)ErrorCode::kInvalidOperandType, lldb::eErrorTypeGeneric,
          FormatDiagnostics(
              m_expr,
              llvm::formatv("no known conversion from '{0}' to 'T*' for 1st "
                            "argument of __findnonnull()",
                            val1_sp->GetCompilerType().GetTypeName()),
              arg1->GetLocation()));
      return error.ToError();
    }

    auto &arg2 = node->arguments()[1];
    auto val2_or_err = DILEvalNode(arg2.get());
    if (!val2_or_err) {
      return val2_or_err;
    }
    lldb::ValueObjectSP val2_sp = *val2_or_err;
    int64_t size = val2_sp->GetValueAsSigned(0);

    if (size < 0 || size > 100000000) {
      Status error = Status(
          (uint32_t)ErrorCode::kInvalidOperandType, lldb::eErrorTypeGeneric,
          FormatDiagnostics(
              m_expr,
              llvm::formatv(
                  "passing in a buffer size ('{0}') that is negative or in "
                  "excess of 100 million to __findnonnull() is not allowed.",
                  size),
              arg2->GetLocation()));
      return error.ToError();
    }

    lldb::ProcessSP process = m_target->GetProcessSP();
    size_t ptr_size = m_target->GetArchitecture().GetAddressByteSize();

    uint64_t memory = 0;
    Status error;

    CompilerType target_type;
    for (auto type_system_sp : m_target->GetScratchTypeSystems())
      if (auto compiler_type =
          type_system_sp->GetBasicTypeFromAST(lldb::eBasicTypeInt)) {
        target_type = compiler_type;
        break;
      }
    ExecutionContext exe_ctx(m_target.get(), false);
    uint64_t byte_size = 0;
    if (auto temp = target_type.GetByteSize(m_target.get()))
      byte_size = temp.value();

    for (int i = 0; i < size; ++i) {
      size_t read =
          process->ReadMemory(addr + i * ptr_size, &memory, ptr_size, error);

      if (error.Fail() || read != ptr_size) {
        const char *message = error.AsCString();
        Status error =
            Status((uint32_t)ErrorCode::kUnknown, lldb::eErrorTypeGeneric,
                   FormatDiagnostics(
                       m_expr,
                       llvm::formatv("error calling __findnonnull(): {0}",
                                     message ? message : "cannot read memory"),
                       node->GetLocation()));
        return error.ToError();
      }

      if (memory != 0) {
        lldb::DataExtractorSP data_sp = std::make_shared<DataExtractor>(
            reinterpret_cast<const void*>(&i), byte_size,
            exe_ctx.GetByteOrder(), exe_ctx.GetAddressByteSize());
        return ValueObject::CreateValueObjectFromData("result", *data_sp,
                                                      exe_ctx, target_type);
      }
    }

    int ret = -1;

    lldb::DataExtractorSP data_sp = std::make_shared<DataExtractor>(
        reinterpret_cast<const void*>(&ret), byte_size,
        exe_ctx.GetByteOrder(), exe_ctx.GetAddressByteSize());
    return ValueObject::CreateValueObjectFromData("result", *data_sp, exe_ctx,
                                                  target_type);
  }

  Status error("invalid ast: unknown builtin function");
  return error.ToError();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const CStyleCastNode *node) {
  // Get the type and the value we need to cast.
  auto type = node->type();
  auto rhs_or_err = DILEvalNode(node->operand());
  if (!rhs_or_err) {
    return rhs_or_err;
  }
  lldb::ValueObjectSP rhs = *rhs_or_err;

  if (rhs->GetCompilerType().IsReferenceType()) {
    Status error;
    rhs = rhs->Dereference(error);
    if (error.Fail())
      return error.ToError();
  }

  switch (node->cast_kind()) {
    case CStyleCastKind::eEnumeration: {
      assert(type.IsEnumerationType() &&
             "invalid ast: target type should be an enumeration.");
      if (rhs->GetCompilerType().IsFloat())
        return rhs->CastToEnumType(type);

      if (rhs->GetCompilerType().IsInteger() ||
          rhs->GetCompilerType().IsEnumerationType())
        return rhs->CastToEnumType(type);

      Status error(
          "invalid ast: operand is not convertible to enumeration type");
      return error.ToError();
    }
    case CStyleCastKind::eNullptr: {
      assert(
          (type.GetCanonicalType().GetBasicTypeEnumeration() ==
           lldb::eBasicTypeNullPtr)
          && "invalid ast: target type should be a nullptr_t.");
      return ValueObject::CreateValueObjectFromNullptr(m_target, type,
                                                       "result");
    }
    case CStyleCastKind::eReference: {
      lldb::ValueObjectSP rhs_sp(GetDynamicOrSyntheticValue(rhs));
      return lldb::ValueObjectSP(rhs_sp->Cast(type.GetNonReferenceType()));
    }
    case CStyleCastKind::eNone: {

      switch (node->promo_kind()) {

        case TypePromotionCastKind::eArithmetic: {
          assert((type.GetCanonicalType().GetBasicTypeEnumeration() !=
                  lldb::eBasicTypeInvalid) &&
                 "invalid ast: target type should be a basic type.");
          // Pick an appropriate cast.
          if (rhs->GetCompilerType().IsPointerType()
              || rhs->GetCompilerType().IsNullPtrType()) {
            return rhs->CastToBasicType(type);
          }
          if (rhs->GetCompilerType().IsScalarType()) {
            return rhs->CastToBasicType(type);
          }
          if (rhs->GetCompilerType().IsEnumerationType()) {
            return rhs->CastToBasicType(type);
          }
          Status error(
              "invalid ast: operand is not convertible to arithmetic type");
          return error.ToError();
        }
        case TypePromotionCastKind::ePointer: {
          assert(type.IsPointerType() &&
                 "invalid ast: target type should be a pointer.");
          uint64_t addr = rhs->GetCompilerType().IsArrayType()
                          ? rhs->GetLoadAddress()
                          : GetUInt64(rhs);
          llvm::StringRef name = "result";
          ExecutionContext exe_ctx(m_target.get(), false);
          return ValueObject::CreateValueObjectFromAddress(
              name, addr, exe_ctx, type,
              /* do_deref */ false);
        }
        case TypePromotionCastKind::eNone:
          return lldb::ValueObjectSP();
      }
    }
  }

  Status error("invalid ast: unexpected c-style cast kind");
  return error.ToError();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const CxxStaticCastNode *node) {
  // Get the type and the value we need to cast.
  auto type = node->type();
  auto orig_type = node->orig_type();
  auto rhs_or_err = DILEvalNode(node->operand());
  if (!rhs_or_err) {
    return rhs_or_err;
  }
  lldb::ValueObjectSP rhs = *rhs_or_err;

  if (rhs->GetCompilerType().IsReferenceType()) {
    Status error;
    rhs = rhs->Dereference(error);
    if (error.Fail())
      return error.ToError();
  }

  CompilerType rhs_type = rhs->GetCompilerType();
  switch (node->cast_kind()) {
    case CxxStaticCastKind::eNoOp: {
      if (!orig_type.CompareTypes(rhs_type) && !type.CompareTypes(rhs_type)) {
        Status error = Status(
            (uint32_t)ErrorCode::kNotImplemented, lldb::eErrorTypeGeneric,
            FormatDiagnostics(
                m_expr,
                llvm::formatv("static_cast from {0} to"
                              " {1} is not implemented yet",
                              rhs->GetCompilerType().TypeDescription(),
                              orig_type.TypeDescription()),
                node->GetLocation()));
        return error.ToError();
      }
      lldb::ValueObjectSP rhs_sp(GetDynamicOrSyntheticValue(rhs));
      return lldb::ValueObjectSP(rhs_sp->Cast(type));
    }

    case CxxStaticCastKind::eEnumeration: {
      if (rhs->GetCompilerType().IsFloat())
        return rhs->CastToEnumType(type);
      if (rhs->GetCompilerType().IsInteger() ||
          rhs->GetCompilerType().IsEnumerationType())
        return rhs->CastToEnumType(type);
      Status error(
          "invalid ast: operand is not convertible to enumeration type");
      return error.ToError();
    }

    case CxxStaticCastKind::eNullptr: {
      return ValueObject::CreateValueObjectFromNullptr(m_target, type,
                                                       "result");
    }

    case CxxStaticCastKind::eDerivedToBase: {
      llvm::Expected<lldb::ValueObjectSP> result =
          rhs->CastDerivedToBaseType(type, node->idx());
      if (result)
        return *result;
      return result;
    }

    case CxxStaticCastKind::eBaseToDerived: {
      llvm::Expected<lldb::ValueObjectSP> result =
          rhs->CastBaseToDerivedType(type, node->offset());
      if (result)
        return *result;
      return result;
    }
    case CxxStaticCastKind::eNone: {

      switch (node->promo_kind()) {

        case TypePromotionCastKind::eArithmetic: {
          assert(type.IsScalarType());
          if (rhs->GetCompilerType().IsPointerType()
              || rhs->GetCompilerType().IsNullPtrType()) {
            assert(type.IsBoolean() && "invalid ast: target type should be bool");
            return rhs->CastToBasicType(type);
          }
          if (rhs->GetCompilerType().IsScalarType())
            return rhs->CastToBasicType(type);
          if (rhs->GetCompilerType().IsEnumerationType())
            return rhs->CastToBasicType(type);

          Status error(
              "invalid ast: operand is not convertible to arithmetic type");
          return error.ToError();
        }

        case TypePromotionCastKind::ePointer: {
          assert(type.IsPointerType() &&
                 "invalid ast: target type should be a pointer.");

          uint64_t addr = rhs->GetCompilerType().IsArrayType()
                          ? rhs-> GetLoadAddress()
                          : rhs->GetValueAsUnsigned(0);
          llvm::StringRef name = "result";
          ExecutionContext exe_ctx(m_target.get(), false);
          return ValueObject::CreateValueObjectFromAddress(
              name, addr, exe_ctx, type,
              /* do_deref */ false);
        }
        case TypePromotionCastKind::eNone:
          return lldb::ValueObjectSP();
      }
    }
  }
  return lldb::ValueObjectSP();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const CxxReinterpretCastNode *node) {
  // Get the type and the value we need to cast.
  auto type = node->type();
  auto rhs_or_err = DILEvalNode(node->operand());
  if (!rhs_or_err) {
    return rhs_or_err;
  }
  lldb::ValueObjectSP rhs = *rhs_or_err;

  if (rhs->GetCompilerType().IsReferenceType()) {
    Status error;
    rhs = rhs->Dereference(error);
    if (error.Fail())
      return error.ToError();
  }

  if (type.IsInteger()) {
    if (rhs->GetCompilerType().IsPointerType() ||
        rhs->GetCompilerType().IsNullPtrType())
      return rhs->CastToBasicType(type);

    CompilerType base_type = type.IsTypedefType() ? type.GetTypedefedType()
                             : type;
    CompilerType rhs_base_type = rhs->GetCompilerType().IsTypedefType() ?
                                 rhs->GetCompilerType().GetTypedefedType() :
                                 rhs->GetCompilerType();
    assert(base_type.CompareTypes(rhs_base_type) &&
           "invalid ast: operands should have the same type");
    // Cast value to handle type aliases.
    lldb::ValueObjectSP rhs_sp(GetDynamicOrSyntheticValue(rhs));
    return lldb::ValueObjectSP(rhs_sp->Cast(type));
  }

  if (type.IsEnumerationType()) {
    CompilerType base_type =
        type.IsTypedefType() ? type.GetTypedefedType() : type;
    CompilerType rhs_base_type = rhs->GetCompilerType().IsTypedefType()
                                     ? rhs->GetCompilerType().GetTypedefedType()
                                     : rhs->GetCompilerType();
    assert(base_type.CompareTypes(rhs_base_type) &&
           "invalid ast: operands should have the same type");
    // Cast value to handle type aliases.
    lldb::ValueObjectSP rhs_sp(GetDynamicOrSyntheticValue(rhs));
    return lldb::ValueObjectSP(rhs_sp->Cast(type));
  }

  if (type.IsPointerType()) {
    assert((rhs->GetCompilerType().IsInteger() ||
            rhs->GetCompilerType().IsEnumerationType() ||
            rhs->GetCompilerType().IsPointerType() ||
            rhs->GetCompilerType().IsArrayType()) &&
           "invalid ast: unexpected operand to reinterpret_cast");
    uint64_t addr = rhs->GetCompilerType().IsArrayType()
                    ? rhs->GetLoadAddress()
                    : rhs->GetValueAsUnsigned(0);
    llvm::StringRef name = "result";
    ExecutionContext exe_ctx(m_target.get(), false);
    return ValueObject::CreateValueObjectFromAddress(name, addr, exe_ctx, type,
                                                     /* do_deref */ false);
  }

  if (type.IsReferenceType()) {
    lldb::ValueObjectSP rhs_sp(GetDynamicOrSyntheticValue(rhs));
    return lldb::ValueObjectSP(rhs_sp->Cast(type.GetNonReferenceType()));
  }

  Status error("invalid ast: unexpected reinterpret_cast kind");
  return error.ToError();
}

static bool GetFieldIndex(CompilerType type, const std::string &name,
                          std::vector<uint32_t> *idx_path) {
  bool found = false;
  uint32_t num_fields = type.GetNumFields();
  for (uint32_t i = 0; i < num_fields; ++i) {
    uint64_t bit_offset = 0;
    uint32_t bitfield_bit_size = 0;
    bool is_bitfield = false;
    std::string name_sstr;
    CompilerType field_type(type.GetFieldAtIndex(
        i, name_sstr, &bit_offset, &bitfield_bit_size, &is_bitfield));
    auto field_name =
        name_sstr.length() == 0 ? std::optional<std::string>() : name_sstr;
    if (field_type.IsValid() && name_sstr == name) {
      idx_path->push_back(i + type.GetNumberOfNonEmptyBaseClasses());
      found = true;
      break;
    } else if (field_type.IsAnonymousType()) {
      found = GetFieldIndex(field_type, name, idx_path);
      if (found) {
        idx_path->push_back(i + type.GetNumberOfNonEmptyBaseClasses());
        break;
      }
    }
  }
  return found;
}

static bool SearchBaseClassesForField(lldb::ValueObjectSP base_sp,
                                      CompilerType base_type,
                                      const std::string &name,
                                      std::vector<uint32_t> *idx_path,
                                      bool use_synthetic, bool is_dynamic) {
  bool found = false;
  uint32_t num_non_empty_bases = 0;
  uint32_t num_direct_bases = base_type.GetNumDirectBaseClasses();
  for (uint32_t i = 0; i < num_direct_bases; ++i) {
    uint32_t bit_offset;
    CompilerType base_class =
        base_type.GetDirectBaseClassAtIndex(i, &bit_offset);
    std::vector<uint32_t> field_idx_path;
    if (GetFieldIndex(base_class, name, &field_idx_path)) {
      for (uint32_t j : field_idx_path)
        idx_path->push_back(j + base_class.GetNumberOfNonEmptyBaseClasses());
      idx_path->push_back(i);
      return true;
    }

    found = SearchBaseClassesForField(base_sp, base_class, name, idx_path,
                                      use_synthetic, is_dynamic);
    if (found) {
      idx_path->push_back(i);
      return true;
    }

    if (base_class.GetNumFields() > 0)
      num_non_empty_bases += 1;
  }
  return false;
}

lldb::ValueObjectSP Interpreter::FindMemberWithName(lldb::ValueObjectSP base,
                                                    ConstString name,
                                                    bool is_arrow) {
  bool is_synthetic = false;
  bool is_dynamic = true;
  // See if GetChildMemberWithName works.
  lldb::ValueObjectSP field_obj =
      base->GetChildMemberWithName(name.GetStringRef());
  if (field_obj && field_obj->GetName() == name)
    return field_obj;

  // Check for synthetic member.
  lldb::ValueObjectSP child_sp = base->GetSyntheticValue();
  if (child_sp) {
    is_synthetic = true;
    field_obj = child_sp->GetChildMemberWithName(name);
    if (field_obj && field_obj->GetName() == name)
      return field_obj;
  }

  // Check indices of immediate member fields of base's type.
  CompilerType base_type = base->GetCompilerType();
  std::vector<uint32_t> field_idx_path;
  if (GetFieldIndex(base_type, name.GetString(), &field_idx_path)) {
    std::reverse(field_idx_path.begin(), field_idx_path.end());
    // Traverse the path & verify the final object is correct.
    field_obj = base;
    for (uint32_t i : field_idx_path)
      field_obj = field_obj->GetChildAtIndex(i, true);
    if (field_obj && field_obj->GetName() == name)
      return field_obj;
  }

  // Go through base classes and look for field there.
  std::vector<uint32_t> base_class_idx_path;
  bool found =
      SearchBaseClassesForField(base, base_type, name.GetString(),
                                &base_class_idx_path, is_synthetic, is_dynamic);
  if (found && !base_class_idx_path.empty()) {
    std::reverse(base_class_idx_path.begin(), base_class_idx_path.end());
    field_obj =
        EvaluateMemberOf(base, base_class_idx_path, is_synthetic, is_dynamic);
    if (field_obj && field_obj->GetName() == name)
      return field_obj;
  }

  // Field not found.
  return lldb::ValueObjectSP();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const MemberOfNode *node) {
  // TODO: Implement address-of elision for member-of:
  //
  //  &(*ptr).foo -> (ptr + foo_offset)
  //  &ptr->foo -> (ptr + foo_offset)
  //
  // This requires calculating the offset of "foo" and generally possible only
  // for members from non-virtual bases.

  Status error;
  auto base_or_err = DILEvalNode(node->base());
  if (!base_or_err) {
    return base_or_err;
  }
  lldb::ValueObjectSP base = *base_or_err;

  // Perform basic type checking.
  CompilerType base_type = base->GetCompilerType();
  if (node->is_arrow() && !base_type.IsPointerType() &&
      !base_type.IsArrayType()) {
    lldb::ValueObjectSP deref_sp = base->Dereference(error);
    if (error.Success()) {
      base = deref_sp;
      base_type = deref_sp->GetCompilerType().GetPointerType();
    } else {
      Status error = Status(
          (uint32_t)ErrorCode::kInvalidOperandType, lldb::eErrorTypeGeneric,
          FormatDiagnostics(
              m_expr,
              llvm::formatv("member reference type {0} is not a pointer; "
                            "did you mean to use '.'?",
                            base_type.TypeDescription()),
              node->GetLocation()));
      return error.ToError();
    }
  } else if (!node->is_arrow() && base_type.IsPointerType()) {
    Status error = Status(
        (uint32_t)ErrorCode::kInvalidOperandType, lldb::eErrorTypeGeneric,
        FormatDiagnostics(
            m_expr,
            llvm::formatv("member reference type {0} is a pointer; "
                          "did you mean to use '->'?",
                          base_type.TypeDescription()),
            node->GetLocation()));
    return error.ToError();
  }

  // User specified array->elem; need to get to element[0] to look for fields.
  if (node->is_arrow() && base_type.IsArrayType())
    base = base->GetChildAtIndex(0);

  lldb::ValueObjectSP field_obj =
      FindMemberWithName(base, node->field_name(), node->is_arrow());
  if (field_obj) {
    if (field_obj->GetCompilerType().IsReferenceType()) {
      lldb::ValueObjectSP tmp_obj = field_obj->Dereference(error);
      if (error.Fail())
        return error.ToError();
      return tmp_obj;
    }
    return field_obj;
  }

  if (node->is_arrow() && base_type.IsPointerType())
    base_type = base_type.GetPointeeType();
  error = Status(
      uint32_t(ErrorCode::kInvalidOperandType), lldb::eErrorTypeGeneric,
      FormatDiagnostics(
          m_expr,
          llvm::formatv("no member named '{0}' in {1}",
                        node->field_name().GetStringRef(),
                        base_type.GetFullyUnqualifiedType().TypeDescription()),
          node->GetLocation()));
  return error.ToError();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const ArraySubscriptNode *node) {
  auto base_or_err = DILEvalNode(node->base());
  if (!base_or_err) {
    return base_or_err;
  }
  lldb::ValueObjectSP base = *base_or_err;
  auto index_or_err = DILEvalNode(node->index());
  if (!index_or_err) {
    return index_or_err;
  }
  lldb::ValueObjectSP index = *index_or_err;

  // Check to see if either the base or the index are references; if they
  // are, dereference them.
  Status error;
  if (base->GetCompilerType().IsReferenceType()) {
    base = base->Dereference(error);
    if (error.Fail())
      return error.ToError();
  }
  if (index->GetCompilerType().IsReferenceType()) {
    index = index->Dereference(error);
    if (error.Fail())
      return error.ToError();
  }

  // Check to see if 'base' has a synthetic value; if so, try using that.
  if (base->HasSyntheticValue()) {
    lldb::ValueObjectSP synthetic = base->GetSyntheticValue();
    if (synthetic && synthetic != base) {
      uint64_t child_idx = index->GetValueAsUnsigned(0);
      if (static_cast<uint32_t>(child_idx) <
          synthetic->GetNumChildrenIgnoringErrors()) {
        lldb::ValueObjectSP child_valobj_sp =
            synthetic->GetChildAtIndex(child_idx);
        if (child_valobj_sp) {
          return child_valobj_sp;
        }
      }
    }
  }

  // Verify that the 'index' is not out-of-range for the declared type.
  lldb::ValueObjectSP synthetic = base->GetSyntheticValue();
  lldb::VariableSP base_var = base->GetVariable();
  if (synthetic) {
    uint32_t num_children = synthetic->GetNumChildrenIgnoringErrors();
    if (index->GetValueAsSigned(0) >= num_children) {
      Status error = Status(
          (uint32_t)ErrorCode::kSubscriptOutOfRange, lldb::eErrorTypeGeneric,
          FormatDiagnostics(
              m_expr,
              llvm::formatv("array index {0} is not valid for \"({1}) {2}\"",
                            index->GetValueAsSigned(0),
                            base->GetTypeName().AsCString("<invalid type>"),
                            base->GetName().AsCString()),
              node->GetLocation()));
      return error.ToError();
    }
  }

  std::string base_name(base->GetCompilerType().GetTypeName(false).AsCString());
  assert((base->GetCompilerType().IsPointerType() ||
          base_name.find("std::tuple") != std::string::npos)
         && "array subscript: base must be a pointer or a tuple");
  assert(index->GetCompilerType().IsIntegerOrUnscopedEnumerationType() &&
         "array subscript: index must be integer or unscoped enum");

  CompilerType item_type = base->GetCompilerType().GetPointeeType();
  lldb::addr_t base_addr = base->GetValueAsUnsigned(0);

  llvm::StringRef name = "result";
  ExecutionContext exe_ctx(m_target.get(), false);
  // Create a pointer and add the index, i.e. "base + index".
  lldb::ValueObjectSP value =
      PointerAdd(ValueObject::CreateValueObjectFromAddress(
          name, base_addr, exe_ctx, item_type.GetPointerType(),
          /* do_deref */ false),
                 index->GetValueAsSigned(0));

  // If we're in the address-of context, skip the dereference and cancel the
  // pending address-of operation as well.
  if (flow_analysis() && flow_analysis()->AddressOfIsPending()) {
    flow_analysis()->DiscardAddressOf();
    return value;
  }

  lldb::ValueObjectSP val2 = value->Dereference(error);
  if (error.Fail())
    return error.ToError();
  return val2;
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
      if (auto compiler_type =
              type_system_sp->GetBasicTypeFromAST(basic_type)) {
        basic_types.insert({basic_type, compiler_type});
        return compiler_type;
      }
  }
  CompilerType empty_type;
  return empty_type;
}

static CompilerType
DoIntegralPromotion(CompilerType from,
                    std::shared_ptr<ExecutionContextScope> ctx) {
  if (!from.IsInteger() && !from.IsUnscopedEnumerationType())
    return from;

  if (!from.IsPromotableIntegerType())
    return from;

  if (from.IsUnscopedEnumerationType())
    return DoIntegralPromotion(from.GetEnumerationIntegerType(), ctx);
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
    for (auto &type : promote_types) {
      uint64_t byte_size = 0;
      if (auto temp = type.GetByteSize(ctx.get()))
        byte_size = temp.value();
      if (from_size < byte_size ||
          (from_size == byte_size &&
           is_signed == (bool)(type.GetTypeInfo() & lldb::eTypeIsSigned))) {
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

static lldb::ValueObjectSP
ArrayToPointerConversion(lldb::ValueObjectSP valobj,
                         std::shared_ptr<ExecutionContextScope> ctx) {
  assert(valobj->IsArrayType() &&
         "an argument to array-to-pointer conversion must be an array");

  uint64_t addr = valobj->GetLoadAddress();
  llvm::StringRef name = "result";
  ExecutionContext exe_ctx;
  ctx->CalculateExecutionContext(exe_ctx);
  return ValueObject::CreateValueObjectFromAddress(
      name, addr, exe_ctx,
      valobj->GetCompilerType().GetArrayElementType(ctx.get()).GetPointerType(),
      /* do_deref */ false);
}

static lldb::ValueObjectSP
UnaryConversion(lldb::ValueObjectSP valobj,
                std::shared_ptr<ExecutionContextScope> ctx) {
  // Perform usual conversions for unary operators. At the moment this includes
  // array-to-pointer and the integral promotion for eligible types.
  CompilerType in_type = valobj->GetCompilerType();
  CompilerType result_type;
  if (valobj->IsBitfield()) {
    // Promote bitfields. If `int` can represent the bitfield value, it is
    // converted to `int`. Otherwise, if `unsigned int` can represent it, it
    // is converted to `unsigned int`. Otherwise, it is treated as its
    // underlying type.
    uint32_t bitfield_size = valobj->GetBitfieldBitSize();
    // Some bitfields have undefined size (e.g. result of ternary operation).
    // The AST's `bitfield_size` of those is 0, and no promotion takes place.
    if (bitfield_size > 0 && in_type.IsInteger()) {
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
          (in_type.IsSigned() && bitfield_size == int_bit_size))
        valobj = valobj->CastToBasicType(int_type);
      else if (bitfield_size <= uint_byte_size * CHAR_BIT)
        valobj = valobj->CastToBasicType(uint_type);
    }
  }

  if (in_type.IsArrayType()) {
    valobj = ArrayToPointerConversion(valobj, ctx);
  }

  if (valobj->GetCompilerType().IsInteger() ||
      valobj->GetCompilerType().IsUnscopedEnumerationType()) {
    CompilerType promoted_type =
        DoIntegralPromotion(valobj->GetCompilerType(), ctx);
    if (!promoted_type.CompareTypes(valobj->GetCompilerType()))
      return valobj->CastToBasicType(promoted_type);
  }

  return valobj;
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
                          lldb::ValueObjectSP &lhs, lldb::ValueObjectSP &rhs,
                          bool convert_lhs, bool convert_rhs) {
  CompilerType l_type = lhs->GetCompilerType();
  CompilerType r_type = rhs->GetCompilerType();
  if (r_type.IsSigned() && !l_type.IsSigned()) {
    uint64_t l_size = 0;
    uint64_t r_size = 0;
    if (auto temp = l_type.GetByteSize(ctx.get()))
      l_size = temp.value();
    ;
    if (auto temp = r_type.GetByteSize(ctx.get()))
      r_size = temp.value();
    if (l_size <= r_size) {
      if (r_size == l_size) {
        auto r_type_unsigned = GetBasicType(
            ctx, BasicTypeToUnsigned(
                     r_type.GetCanonicalType().GetBasicTypeEnumeration()));
        if (convert_rhs)
          rhs = rhs->CastToBasicType(r_type_unsigned);
      }
    }
  }
  if (convert_lhs)
    lhs = lhs->CastToBasicType(rhs->GetCompilerType());
}

static void ArithmeticConversions(lldb::ValueObjectSP &lhs,
                                  lldb::ValueObjectSP &rhs,
                                  std::shared_ptr<ExecutionContextScope> ctx,
                                  bool is_comp_assign = false) {
  CompilerType lhs_type = lhs->GetCompilerType();
  CompilerType rhs_type = rhs->GetCompilerType();

  if (lhs_type.CompareTypes(rhs_type))
    return;

  if (lhs_type.IsFloat() || rhs_type.IsFloat()) {
    if (lhs_type.IsFloat() && rhs_type.IsFloat()) {
      int order = lhs_type.GetBasicTypeEnumeration() -
                  rhs_type.GetBasicTypeEnumeration();
      if (order > 0) {
        rhs = rhs->CastToBasicType(lhs_type);
        return;
      }
      if (!is_comp_assign)
        lhs = lhs->CastToBasicType(rhs_type);
      return;
    }

    if (lhs_type.IsFloat() && rhs_type.IsInteger()) {
      rhs = rhs->CastToBasicType(lhs_type);
      return;
    }

    if (rhs_type.IsFloat() && !is_comp_assign)
      lhs = lhs->CastToBasicType(rhs_type);
  }

  if (lhs_type.IsInteger() && rhs_type.IsInteger()) {
    using Rank = std::tuple<size_t, bool>;
    Rank l_rank = {ConversionRank(lhs_type), !lhs_type.IsSigned()};
    Rank r_rank = {ConversionRank(rhs_type), !rhs_type.IsSigned()};

    if (l_rank < r_rank) {
      PerformIntegerConversions(ctx, lhs, rhs, !is_comp_assign, true);
    } else if (l_rank > r_rank) {
      PerformIntegerConversions(ctx, rhs, lhs, true, !is_comp_assign);
    }
  }
}

bool IsCompAssign(BinaryOpKind kind) {
  return (
      (kind == BinaryOpKind::AddAssign) || (kind == BinaryOpKind::SubAssign) ||
      (kind == BinaryOpKind::MulAssign) || (kind == BinaryOpKind::DivAssign) ||
      (kind == BinaryOpKind::RemAssign) || (kind == BinaryOpKind::AndAssign) ||
      (kind == BinaryOpKind::OrAssign) || (kind == BinaryOpKind::XorAssign) ||
      (kind == BinaryOpKind::ShlAssign) || (kind == BinaryOpKind::ShrAssign));
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const BinaryOpNode *node) {
  // Short-circuit logical operators.
  if (node->kind() == BinaryOpKind::LAnd || node->kind() == BinaryOpKind::LOr) {
    Status error;
    auto lhs_or_err = DILEvalNode(node->lhs());
    if (!lhs_or_err) {
      return lhs_or_err;
    }
    lldb::ValueObjectSP lhs = *lhs_or_err;
    if (lhs->GetCompilerType().IsReferenceType()) {
      lhs = lhs->Dereference(error);
      if (error.Fail())
        return error.ToError();
    }
    assert(lhs->GetCompilerType().IsContextuallyConvertibleToBool() &&
           "invalid ast: must be convertible to bool");

    // For "&&" break if LHS is "false", for "||" if LHS is "true".
    auto lvalue_or_err = lhs->GetValueAsBool();
    if (!lvalue_or_err)
      return lvalue_or_err.takeError();

    bool lhs_val = *lvalue_or_err;
    bool break_early =
        (node->kind() == BinaryOpKind::LAnd) ? !lhs_val : lhs_val;

    if (break_early) {
      return ValueObject::CreateValueObjectFromBool(m_target, lhs_val,
                                                    "result");
    }

    // Breaking early didn't happen, evaluate the RHS and use it as a result.
    auto rhs_or_err = DILEvalNode(node->rhs());
    if (!rhs_or_err) {
      return rhs_or_err;
    }
    lldb::ValueObjectSP rhs = *rhs_or_err;
    if (rhs->GetCompilerType().IsReferenceType()) {
      rhs = rhs->Dereference(error);
      if (error.Fail())
        return error.ToError();
    }
    assert(rhs->GetCompilerType().IsContextuallyConvertibleToBool() &&
           "invalid ast: must be convertible to bool");

    auto rvalue_or_err = rhs->GetValueAsBool();
    if (!rvalue_or_err)
      return rvalue_or_err.takeError();

    return ValueObject::CreateValueObjectFromBool(m_target, *rvalue_or_err,
                                                  "result");
  }

  // All other binary operations require evaluating both operands.
  auto lhs_or_err = DILEvalNode(node->lhs());
  if (!lhs_or_err) {
    return lhs_or_err;
  }
  lldb::ValueObjectSP lhs = *lhs_or_err;
  auto rhs_or_err = DILEvalNode(node->rhs());
  if (!rhs_or_err) {
    return rhs_or_err;
  }
  lldb::ValueObjectSP rhs = *rhs_or_err;

  // If either operand is a MemberOf node, we have not yet done the correct
  // type checking & conversions, so we need to do them now.
  if (llvm::isa<MemberOfNode>(node->lhs()) ||
      llvm::isa<MemberOfNode>(node->rhs())) {
    bool is_comp_assign = IsCompAssign(node->kind());
    if (!is_comp_assign)
      lhs = UnaryConversion(lhs, m_exe_ctx_scope);
    rhs = UnaryConversion(rhs, m_exe_ctx_scope);
    if (!is_comp_assign)
      ArithmeticConversions(lhs, rhs, m_exe_ctx_scope, is_comp_assign);
  }

  // For math operations, be sure to dereference the operands.
  if ((node->kind() == BinaryOpKind::Add)
      || (node->kind() == BinaryOpKind::Sub)
      || (node->kind() == BinaryOpKind::Mul)
      || (node->kind() == BinaryOpKind::Div)
      || (node->kind() == BinaryOpKind::Rem)) {
    Status error;
    if (lhs->GetCompilerType().IsReferenceType()) {
      lhs = lhs->Dereference(error);
      if (error.Fail())
        return error.ToError();
    }
    if (rhs->GetCompilerType().IsReferenceType()) {
      rhs = rhs->Dereference(error);
      if (error.Fail())
        return error.ToError();
    }
  }

  switch (node->kind()) {
    case BinaryOpKind::Add:
      return EvaluateBinaryAddition(lhs, rhs, node->GetLocation());
    case BinaryOpKind::Sub:
      // The result type of subtraction is required because it holds the
      // correct "ptrdiff_t" type in the case of subtracting two pointers.
      return EvaluateBinarySubtraction(lhs, rhs,
                                       node->GetDereferencedResultType());
    case BinaryOpKind::Mul:
      return EvaluateBinaryMultiplication(lhs, rhs);
    case BinaryOpKind::Div:
      return EvaluateBinaryDivision(lhs, rhs, node->GetLocation());
    case BinaryOpKind::Rem:
      return EvaluateBinaryRemainder(lhs, rhs);
    case BinaryOpKind::And:
    case BinaryOpKind::Or:
    case BinaryOpKind::Xor:
      return EvaluateBinaryBitwise(node->kind(), lhs, rhs);
    case BinaryOpKind::Shl:
    case BinaryOpKind::Shr:
      return EvaluateBinaryShift(node->kind(), lhs, rhs);

    // Comparison operations.
    case BinaryOpKind::EQ:
    case BinaryOpKind::NE:
    case BinaryOpKind::LT:
    case BinaryOpKind::LE:
    case BinaryOpKind::GT:
    case BinaryOpKind::GE:
      return EvaluateComparison(node->kind(), lhs, rhs);

    case BinaryOpKind::Assign:
      return EvaluateAssignment(lhs, rhs);
    case BinaryOpKind::AddAssign:
      return EvaluateBinaryAddAssign(lhs, rhs, node->GetLocation());
    case BinaryOpKind::SubAssign:
      return EvaluateBinarySubAssign(lhs, rhs);
    case BinaryOpKind::MulAssign:
      return EvaluateBinaryMulAssign(lhs, rhs);
    case BinaryOpKind::DivAssign:
      return EvaluateBinaryDivAssign(lhs, rhs, node->GetLocation());
    case BinaryOpKind::RemAssign:
      return EvaluateBinaryRemAssign(lhs, rhs);

    case BinaryOpKind::AndAssign:
    case BinaryOpKind::OrAssign:
    case BinaryOpKind::XorAssign:
      return EvaluateBinaryBitwiseAssign(node->kind(), lhs, rhs);
    case BinaryOpKind::ShlAssign:
    case BinaryOpKind::ShrAssign:
      return EvaluateBinaryShiftAssign(node->kind(), lhs, rhs,
                                       node->comp_assign_type());

    default:
      break;
  }

  // Unsupported/invalid operation.
  Status error("invalid ast: unexpected binary operator");
  return error.ToError();
}

llvm::Error Interpreter::CheckIncrementDecrement(const UnaryOpNode *node,
                                                 CompilerType rhs_type) {

  // In C++ the requirement here is that the expression is "assignable". However
  // in the debugger context side-effects are not allowed and the only case
  // where increment/decrement are permitted is when modifying the "context
  // variable".
  // Technically, `++(++$var)` could be allowed too, since both increments
  // modify the context variable. However, MSVC debugger doesn't allow it, so we
  // don't implement it too.
  if (node->rhs()->is_rvalue()) {
    return BailOut(ErrorCode::kInvalidOperandType,
                   llvm::formatv("expression is not assignable"),
                   node->GetLocation());
  }
  if (!node->rhs()->is_context_var() && !AllowSideEffects()) {
    return BailOut(
        ErrorCode::kInvalidOperandType,
        llvm::formatv("side effects are not supported in this context: "
                      "trying to modify data at the target process"),
        node->GetLocation());
  }
  auto kind = node->kind();
  llvm::StringRef op_name =
      (kind == UnaryOpKind::PreInc || kind == UnaryOpKind::PostInc)
          ? "increment"
          : "decrement";
  if (rhs_type.IsEnumerationType()) {
    return BailOut(ErrorCode::kInvalidOperandType,
                   llvm::formatv("cannot {0} expression of enum type '{1}'",
                                 op_name, rhs_type.GetTypeName()),
                   node->GetLocation());
  }
  if (!rhs_type.IsScalarType() && !rhs_type.IsPointerType()) {
    return BailOut(ErrorCode::kInvalidOperandType,
                   llvm::formatv("cannot {0} value of type '{1}'", op_name,
                                 rhs_type.GetTypeName()),
                   node->GetLocation());
  }
  return llvm::Error::success();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const UnaryOpNode *node) {
  FlowAnalysis rhs_flow(
      /* address_of_is_pending */ node->kind() == UnaryOpKind::AddrOf);

  Status error;
  auto rhs_or_err = DILEvalNode(node->rhs(), &rhs_flow);
  if (!rhs_or_err) {
    return rhs_or_err;
  }
  lldb::ValueObjectSP rhs = *rhs_or_err;

  if (rhs->GetCompilerType().IsReferenceType()) {
    rhs = rhs->Dereference(error);
    if (error.Fail())
      return error.ToError();
  }
  CompilerType rhs_type = rhs->GetCompilerType();

  switch (node->kind()) {
    case UnaryOpKind::Deref: {
      if (rhs_type.IsArrayType()) {
        rhs = ArrayToPointerConversion(rhs, m_exe_ctx_scope);
      }

      lldb::ValueObjectSP dynamic_rhs = rhs->GetDynamicValue(m_default_dynamic);
      if (dynamic_rhs)
        rhs = dynamic_rhs;

      if (rhs->GetCompilerType().IsPointerType())
        return EvaluateDereference(rhs);
      lldb::ValueObjectSP child_sp = rhs->Dereference(error);
      if (error.Success())
        rhs = child_sp;

      return rhs;
    }
    case UnaryOpKind::AddrOf: {
      if (node->rhs()->is_rvalue()) {
        return BailOut(
            ErrorCode::kInvalidOperandType,
            llvm::formatv("cannot take the address of an rvalue of type {0}",
                          rhs_type.TypeDescription()),
            node->GetLocation());
      }
      if (rhs->IsBitfield()) {
        return BailOut(ErrorCode::kInvalidOperandType,
                       "address of bit-field requested", node->GetLocation());
      }
      // If the address-of operation wasn't cancelled during the evaluation of
      // RHS (e.g. because of the address-of-a-dereference elision), apply it
      // here.
      if (rhs_flow.AddressOfIsPending()) {
        Status error;
        lldb::ValueObjectSP value = rhs->AddressOf(error);
        if (error.Fail())
          return error.ToError();
        return value;
      }
      return rhs;
    }
    case UnaryOpKind::Plus:
      rhs = UnaryConversion(rhs, m_exe_ctx_scope);
      rhs_type = rhs->GetCompilerType();
      if (!rhs_type.IsScalarType() &&
          // Unary plus is allowed for pointers.
          !rhs_type.IsPointerType()) {
        return BailOut(ErrorCode::kInvalidOperandType,
                       llvm::formatv(kInvalidOperandsToUnaryExpression,
                                     rhs_type.TypeDescription()),
                       node->GetLocation());
      }
      return rhs;
    case UnaryOpKind::Minus:
      rhs = UnaryConversion(rhs, m_exe_ctx_scope);
      rhs_type = rhs->GetCompilerType();
      if (!rhs_type.IsScalarType())
        return BailOut(ErrorCode::kInvalidOperandType,
                       llvm::formatv(kInvalidOperandsToUnaryExpression,
                                     rhs_type.TypeDescription()),
                       node->GetLocation());
      return EvaluateUnaryMinus(rhs);
    case UnaryOpKind::LNot:
      if (!rhs_type.IsContextuallyConvertibleToBool())
        return BailOut(ErrorCode::kInvalidOperandType,
                       llvm::formatv(kInvalidOperandsToUnaryExpression,
                                     rhs_type.TypeDescription()),
                       node->GetLocation());
      return EvaluateUnaryNegation(rhs);
    case UnaryOpKind::Not:
      rhs = UnaryConversion(rhs, m_exe_ctx_scope);
      rhs_type = rhs->GetCompilerType();
      if (!rhs_type.IsInteger())
        return BailOut(ErrorCode::kInvalidOperandType,
                       llvm::formatv(kInvalidOperandsToUnaryExpression,
                                     rhs_type.TypeDescription()),
                       node->GetLocation());
      return EvaluateUnaryBitwiseNot(rhs);
    case UnaryOpKind::PreInc:
      if (llvm::Error err = CheckIncrementDecrement(node, rhs_type))
        return err;
      return EvaluateUnaryPrefixIncrement(rhs);
    case UnaryOpKind::PreDec:
      if (llvm::Error err = CheckIncrementDecrement(node, rhs_type))
        return err;
      return EvaluateUnaryPrefixDecrement(rhs);
    case UnaryOpKind::PostInc: {
      if (llvm::Error err = CheckIncrementDecrement(node, rhs_type))
        return err;
      // In postfix inc/dec the result is the original value.
      lldb::ValueObjectSP val2 = rhs->Clone(ConstString("cloned-object"));
      EvaluateUnaryPrefixIncrement(rhs);
      return val2;
    }
    case UnaryOpKind::PostDec: {
      if (llvm::Error err = CheckIncrementDecrement(node, rhs_type))
        return err;
      // In postfix inc/dec the result is the original value.
      lldb::ValueObjectSP val2 = rhs->Clone(ConstString("cloned-object"));
      EvaluateUnaryPrefixDecrement(rhs);
      return val2;
    }
  }

  // Unsupported/invalid operation.
  Status error2("invalid ast: unexpected binary operator");
  return error2.ToError();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::Visit(const TernaryOpNode *node) {
  auto cond_or_err = DILEvalNode(node->cond());
  if (!cond_or_err) {
    return cond_or_err;
  }
  lldb::ValueObjectSP cond = *cond_or_err;
  assert(cond->GetCompilerType().IsContextuallyConvertibleToBool() &&
         "invalid ast: must be convertible to bool");

  // Pass down the flow analysis because the conditional operator is a "flow
  // control" construct -- LHS/RHS might be lvalues and eligible for some
  // optimizations (e.g. "&*" elision).
  auto value_or_err = cond->GetValueAsBool();
  if (value_or_err) {
    if (*value_or_err) {
      auto lhs_or_err = DILEvalNode(node->lhs(), flow_analysis());
      if (!lhs_or_err)
        return lhs_or_err;
      lldb::ValueObjectSP lhs = *lhs_or_err;
      if (llvm::isa<MemberOfNode>(node->lhs()))
        lhs = UnaryConversion(lhs, m_exe_ctx_scope);
      return lhs;
    }

    auto rhs_or_err = DILEvalNode(node->rhs(), flow_analysis());
    if (!rhs_or_err)
      return rhs_or_err;
    lldb::ValueObjectSP rhs = *rhs_or_err;
    return rhs;
  }
  return value_or_err.takeError();
}

lldb::ValueObjectSP Interpreter::EvaluateComparison(BinaryOpKind kind,
                                                    lldb::ValueObjectSP lhs,
                                                    lldb::ValueObjectSP rhs) {
  // Evaluate arithmetic operation for two integral values.
  if (lhs->GetCompilerType().IsInteger() && rhs->GetCompilerType().IsInteger()) {
    llvm::Expected<llvm::APSInt> l = lhs->GetValueAsAPSInt();
    llvm::Expected<llvm::APSInt> r = rhs->GetValueAsAPSInt();

    if (l && r) {
      bool ret = Compare(kind, *l, *r);
      return ValueObject::CreateValueObjectFromBool(m_target, ret, "result");
    }
  }

  // Evaluate arithmetic operation for two floating point values.
  if (lhs->GetCompilerType().IsFloat() && rhs->GetCompilerType().IsFloat()) {
    llvm::Expected<llvm::APFloat> l = lhs->GetValueAsAPFloat();
    llvm::Expected<llvm::APFloat> r = rhs->GetValueAsAPFloat();
    if (l && r) {
      bool ret = Compare(kind, *l, *r);
      return ValueObject::CreateValueObjectFromBool(m_target, ret, "result");
    }
  }

  // Evaluate arithmetic operation for two scoped enum values.
  if (lhs->GetCompilerType().IsScopedEnumerationType()
      && rhs->GetCompilerType().IsScopedEnumerationType()) {
    llvm::Expected<llvm::APSInt> l = lhs->GetValueAsAPSInt();
    llvm::Expected<llvm::APSInt> r = rhs->GetValueAsAPSInt();
    if (l && r) {
      bool ret = Compare(kind, *l, *r);
      return ValueObject::CreateValueObjectFromBool(m_target, ret, "result");
    }
  }

  // Must be pointer/integer and/or nullptr comparison.
  size_t ptr_size = m_target->GetArchitecture().GetAddressByteSize() * 8;
  llvm::Expected<llvm::APSInt> l = lhs->GetValueAsAPSInt();
  llvm::Expected<llvm::APSInt> r = rhs->GetValueAsAPSInt();

  if (l && r) {
    bool ret =
        Compare(kind, llvm::APSInt(l->sextOrTrunc(ptr_size), true),
                llvm::APSInt(r->sextOrTrunc(ptr_size), true));
    return ValueObject::CreateValueObjectFromBool(m_target, ret, "result");
  }

  return lldb::ValueObjectSP();
}

lldb::ValueObjectSP Interpreter::EvaluateDereference(lldb::ValueObjectSP rhs) {
  // If rhs is a reference, dereference it first.
  Status error;
  if (rhs->GetCompilerType().IsReferenceType())
    rhs = rhs->Dereference(error);

  assert(rhs->GetCompilerType().IsPointerType()
         && "invalid ast: must be a pointer type");

  if (rhs->GetDerefValobj())
    return rhs->GetDerefValobj()->GetSP();

  CompilerType pointer_type = rhs->GetCompilerType();
  lldb::addr_t base_addr = rhs->GetValueAsUnsigned(0);

  llvm::StringRef name = "result";
  ExecutionContext exe_ctx(m_target.get(), false);
  lldb::ValueObjectSP value =
          ValueObject::CreateValueObjectFromAddress(name, base_addr, exe_ctx,
                                                    pointer_type,
                                                    /* do_deref */ false);

  // If we're in the address-of context, skip the dereference and cancel the
  // pending address-of operation as well.
  if (flow_analysis() && flow_analysis()->AddressOfIsPending()) {
    flow_analysis()->DiscardAddressOf();
    return value;
  }

  return value->Dereference(error);
}

lldb::ValueObjectSP Interpreter::EvaluateUnaryMinus(lldb::ValueObjectSP rhs) {
  assert((rhs->GetCompilerType().IsInteger() || rhs->GetCompilerType().IsFloat())
         && "invalid ast: must be an arithmetic type");

  if (rhs->GetCompilerType().IsInteger()) {
    llvm::Expected<llvm::APSInt> r = rhs->GetValueAsAPSInt();
    if (r) {
      llvm::APSInt v = *r;
      v.negate();
      return ValueObject::CreateValueObjectFromAPInt(m_target, v,
                                                     rhs->GetCompilerType(),
                                                     "result");
    }
  }
  if (rhs->GetCompilerType().IsFloat()) {
    auto value_or_err = rhs->GetValueAsAPFloat();
    if (value_or_err) {
      llvm::APFloat v = *value_or_err;
      v.changeSign();
      return ValueObject::CreateValueObjectFromAPFloat(m_target, v,
                                                       rhs->GetCompilerType(),
                                                       "result");
    }
  }

  return lldb::ValueObjectSP();
}

lldb::ValueObjectSP
Interpreter::EvaluateUnaryNegation(lldb::ValueObjectSP rhs) {
  assert(rhs->GetCompilerType().IsContextuallyConvertibleToBool() &&
         "invalid ast: must be convertible to bool");
  auto value_or_err = rhs->GetValueAsBool();
  if (value_or_err)
    return ValueObject::CreateValueObjectFromBool(m_target,
                                                  !(*value_or_err),
                                                  "result");
  else
    return lldb::ValueObjectSP();
}

lldb::ValueObjectSP
Interpreter::EvaluateUnaryBitwiseNot(lldb::ValueObjectSP rhs) {
  assert(rhs->GetCompilerType().IsInteger() && "invalid ast: must be an integer");
  auto value_or_err = rhs->GetValueAsAPSInt();
  if (value_or_err) {
    llvm::APSInt v = *value_or_err;
    v.flipAllBits();
    return ValueObject::CreateValueObjectFromAPInt(m_target, v,
                                                   rhs->GetCompilerType(),
                                                   "result");
  }
  return lldb::ValueObjectSP();
}

lldb::ValueObjectSP
Interpreter::EvaluateUnaryPrefixIncrement(lldb::ValueObjectSP rhs) {
  assert((rhs->GetCompilerType().IsInteger() || rhs->GetCompilerType().IsFloat()
          || rhs->GetCompilerType().IsPointerType()) &&
         "invalid ast: must be either arithmetic type or pointer");

  Status status;
  if (rhs->GetCompilerType().IsInteger()) {
    llvm::Expected<llvm::APSInt> r = rhs->GetValueAsAPSInt();
    if (r) {
      llvm::APSInt v = *r;
      ++v;  // Do the increment.

      rhs->SetValueFromInteger(v, status);
      if (status.Success())
        return rhs;
      return lldb::ValueObjectSP();
    }
  }
  if (rhs->GetCompilerType().IsFloat()) {
    auto value_or_err = rhs->GetValueAsAPFloat();
    if (value_or_err) {
      llvm::APFloat v = *value_or_err;
      // Do the increment.
      v = v + llvm::APFloat(v.getSemantics(), 1ULL);

      rhs->SetValueFromInteger(v.bitcastToAPInt(), status);
      if (status.Success())
        return rhs;
      return lldb::ValueObjectSP();
    }
  }
  if (rhs->GetCompilerType().IsPointerType()) {
    uint64_t v = rhs->GetValueAsUnsigned(0);
    uint64_t byte_size = 0;
    if (auto temp =
            rhs->GetCompilerType().GetPointeeType().GetByteSize(
                rhs->GetTargetSP().get()))
      byte_size = temp.value();
    v += byte_size; // Do the increment.

    rhs->SetValueFromInteger(llvm::APInt(64, v), status);
    if (status.Success())
      return rhs;
  }

  return lldb::ValueObjectSP();
}

lldb::ValueObjectSP
Interpreter::EvaluateUnaryPrefixDecrement(lldb::ValueObjectSP rhs) {
  assert((rhs->GetCompilerType().IsInteger() ||
          rhs->GetCompilerType().IsFloat() ||
          rhs->GetCompilerType().IsPointerType()) &&
         "invalid ast: must be either arithmetic type or pointer");

  Status status;
  if (rhs->GetCompilerType().IsInteger()) {
    llvm::Expected<llvm::APSInt> r = rhs->GetValueAsAPSInt();
    if (r) {
      llvm::APSInt v = *r;
      --v;  // Do the decrement.

      rhs->SetValueFromInteger(v, status);
      if (status.Success())
        return rhs;
      return lldb::ValueObjectSP();
    }
  }
  if (rhs->GetCompilerType().IsFloat()) {
    auto value_or_err = rhs->GetValueAsAPFloat();
    if (value_or_err) {
      llvm::APFloat v = *value_or_err;
      // Do the decrement.
      v = v - llvm::APFloat(v.getSemantics(), 1ULL);

      rhs->SetValueFromInteger(v.bitcastToAPInt(), status);
      if (status.Success())
        return rhs;
      return lldb::ValueObjectSP();
    }
  }
  if (rhs->GetCompilerType().IsPointerType()) {
    uint64_t v = rhs->GetValueAsUnsigned(0);
    uint64_t byte_size = 0;
    if (auto temp =
            rhs->GetCompilerType().GetPointeeType().GetByteSize(
                rhs->GetTargetSP().get()))
      byte_size = temp.value();
    v -= byte_size;  // Do the decrement.

    rhs->SetValueFromInteger(llvm::APInt(64, v), status);
    if (status.Success())
      return rhs;
  }

  return lldb::ValueObjectSP();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::EvaluateBinaryAddition(lldb::ValueObjectSP lhs,
                                    lldb::ValueObjectSP rhs, uint32_t loc) {
  // Addition of two arithmetic types.
  if (lhs->GetCompilerType().IsScalarType()
      && rhs->GetCompilerType().IsScalarType()) {
    return EvaluateArithmeticOp(m_target, BinaryOpKind::Add, lhs, rhs,
                                lhs->GetCompilerType().GetCanonicalType());
  }

  // Here one of the operands must be a pointer and the other one an integer.
  lldb::ValueObjectSP ptr, offset;
  if (lhs->GetCompilerType().IsPointerType()) {
    ptr = lhs;
    offset = rhs;
  } else {
    ptr = rhs;
    offset = lhs;
  }
  assert(ptr->GetCompilerType().IsPointerType() &&
         "invalid ast: ptr must be a pointer");
  assert(offset->GetCompilerType().IsInteger() &&
         "invalid ast: offset must be an integer");

  if (ptr->GetValueAsUnsigned(0) == 0 && offset->GetValueAsUnsigned(0) != 0) {
    // Binary addition with null pointer causes mismatches between LLDB and
    // lldb-eval if the offset different than zero.
    return SetUbStatus(ErrorCode::kUBNullPtrArithmetic).ToError();
  }

  return PointerAdd(ptr, offset->GetValueAsUnsigned(0));
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::EvaluateBinarySubtraction(lldb::ValueObjectSP lhs,
                                       lldb::ValueObjectSP rhs,
                                       CompilerType result_type) {
  if (lhs->GetCompilerType().IsScalarType()
      && rhs->GetCompilerType().IsScalarType()) {
    assert(lhs->GetCompilerType().CompareTypes(rhs->GetCompilerType()) &&
           "invalid ast: operand must have the same type");
    return EvaluateArithmeticOp(m_target, BinaryOpKind::Sub, lhs, rhs,
                                lhs->GetCompilerType().GetCanonicalType());
  }
  assert(lhs->GetCompilerType().IsPointerType()
         && "invalid ast: lhs must be a pointer");

  // "pointer - integer" operation.
  if (rhs->GetCompilerType().IsInteger()) {
    return PointerAdd(lhs, - rhs->GetValueAsUnsigned(0));
  }

  uint64_t lhs_byte_size = 0;
  uint64_t rhs_byte_size = 0;
  if (auto temp =
      lhs->GetCompilerType().GetPointeeType().GetByteSize(
          lhs->GetTargetSP().get()))
    lhs_byte_size = temp.value();
  if (auto temp = rhs->GetCompilerType().GetPointeeType().GetByteSize(
          rhs->GetTargetSP().get()))
    rhs_byte_size = temp.value();
  // "pointer - pointer" operation.
  assert(rhs->GetCompilerType().IsPointerType()
         && "invalid ast: rhs must an integer or a pointer");
  assert((lhs_byte_size == rhs_byte_size) &&
         "invalid ast: pointees should be the same size");

  // Since pointers have compatible types, both have the same pointee size.
  uint64_t item_size = lhs_byte_size;
  // Pointer difference is a signed value.
  int64_t diff = static_cast<int64_t>(lhs->GetValueAsUnsigned(0) -
                                      rhs->GetValueAsUnsigned(0));

  if (diff % item_size != 0 && diff < 0) {
    // If address difference isn't divisible by pointee size then performing
    // the operation is undefined behaviour. Note: mismatches were encountered
    // only for negative difference (diff < 0).
    return SetUbStatus(ErrorCode::kUBInvalidPtrDiff).ToError();
  }

  diff /= static_cast<int64_t>(item_size);

  // Pointer difference is ptrdiff_t.
  ExecutionContext exe_ctx(m_target.get(), false);
  uint64_t byte_size = 0;
  if (auto temp = result_type.GetByteSize(m_target.get()))
    byte_size = temp.value();
  lldb::DataExtractorSP data_sp = std::make_shared<DataExtractor>(
      reinterpret_cast<const void*>(&diff), byte_size,
      exe_ctx.GetByteOrder(), exe_ctx.GetAddressByteSize());
  return ValueObject::CreateValueObjectFromData("result", *data_sp, exe_ctx,
                                                result_type);
}

lldb::ValueObjectSP
Interpreter::EvaluateBinaryMultiplication(lldb::ValueObjectSP lhs,
                                          lldb::ValueObjectSP rhs) {
  assert((lhs->GetCompilerType().IsScalarType() &&
          lhs->GetCompilerType().CompareTypes(rhs->GetCompilerType())) &&
         "invalid ast: operands must be arithmetic and have the same type");

  return EvaluateArithmeticOp(m_target, BinaryOpKind::Mul, lhs, rhs,
                              lhs->GetCompilerType().GetCanonicalType());
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::EvaluateBinaryDivision(lldb::ValueObjectSP lhs,
                                    lldb::ValueObjectSP rhs, uint32_t loc) {
  if (!lhs->GetCompilerType().IsScalarType() ||
      !lhs->GetCompilerType().CompareTypes(rhs->GetCompilerType())) {
    Status error = Status(
        (uint32_t)ErrorCode::kInvalidOperandType, lldb::eErrorTypeGeneric,
        FormatDiagnostics(
            m_expr,
            llvm::formatv("invalid operands to binary expression ({0} and {1})",
                          lhs->GetCompilerType().TypeDescription(),
                          rhs->GetCompilerType().TypeDescription()),
            loc));
    return error.ToError();
  }

  // Check for zero only for integer division.
  if (rhs->GetCompilerType().IsInteger() && rhs->GetValueAsUnsigned(0) == 0) {
    // This is UB and the compiler would generate a warning:
    //
    //  warning: division by zero is undefined [-Wdivision-by-zero]
    //
    return SetUbStatus(ErrorCode::kUBDivisionByZero).ToError();

    return rhs;
  }

  if (rhs->GetCompilerType().IsInteger() && IsInvalidDivisionByMinusOne(lhs, rhs))
  {
    return SetUbStatus(ErrorCode::kUBDivisionByMinusOne).ToError();
  }

  return EvaluateArithmeticOp(m_target, BinaryOpKind::Div, lhs, rhs,
                              lhs->GetCompilerType().GetCanonicalType());
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::EvaluateBinaryRemainder(lldb::ValueObjectSP lhs,
                                     lldb::ValueObjectSP rhs) {
  assert((lhs->GetCompilerType().IsInteger()
          && lhs->GetCompilerType().CompareTypes(rhs->GetCompilerType()))
         && "invalid ast: operands must be integers and have the same type");

  if (rhs->GetValueAsUnsigned(0) == 0) {
    // This is UB and the compiler would generate a warning:
    //
    //  warning: remainder by zero is undefined [-Wdivision-by-zero]
    //
    return SetUbStatus(ErrorCode::kUBDivisionByZero).ToError();

    return rhs;
  }

  if (IsInvalidDivisionByMinusOne(lhs, rhs)) {
    return SetUbStatus(ErrorCode::kUBDivisionByMinusOne).ToError();
  }

  return EvaluateArithmeticOpInteger(m_target, BinaryOpKind::Rem, lhs, rhs,
                                     lhs->GetCompilerType());
}

lldb::ValueObjectSP
Interpreter::EvaluateBinaryBitwise(BinaryOpKind kind, lldb::ValueObjectSP lhs,
                                   lldb::ValueObjectSP rhs) {
  assert((lhs->GetCompilerType().IsInteger()
          && lhs->GetCompilerType().CompareTypes(rhs->GetCompilerType()))
         &&"invalid ast: operands must be integers and have the same type");
  assert((kind == BinaryOpKind::And || kind == BinaryOpKind::Or ||
          kind == BinaryOpKind::Xor) &&
         "invalid ast: operation must be '&', '|' or '^'");

  return EvaluateArithmeticOpInteger(m_target, kind, lhs, rhs,
                                     lhs->GetCompilerType().GetCanonicalType());
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::EvaluateBinaryShift(BinaryOpKind kind, lldb::ValueObjectSP lhs,
                                 lldb::ValueObjectSP rhs) {
  assert(lhs->GetCompilerType().IsInteger() &&
         rhs->GetCompilerType().IsInteger() &&
         "invalid ast: operands must be integers");
  assert((kind == BinaryOpKind::Shl || kind == BinaryOpKind::Shr) &&
         "invalid ast: operation must be '<<' or '>>'");

  uint64_t lhs_byte_size = 0;
  if (auto temp = lhs->GetCompilerType().GetByteSize(lhs->GetTargetSP().get()))
    lhs_byte_size = temp.value();
  // Performing shift operation is undefined behaviour if the right operand
  // isn't in interval [0, bit-size of the left operand).
  llvm::Expected<llvm::APSInt> r = rhs->GetValueAsAPSInt();
  if (r && (r->isNegative() ||
            (rhs->GetValueAsUnsigned(0) >= lhs_byte_size * CHAR_BIT))) {
    return SetUbStatus(ErrorCode::kUBInvalidShift).ToError();
  }

  return EvaluateArithmeticOpInteger(m_target, kind, lhs, rhs,
                                     lhs->GetCompilerType());
}

lldb::ValueObjectSP Interpreter::EvaluateAssignment(lldb::ValueObjectSP lhs,
                                                    lldb::ValueObjectSP rhs) {
  assert(lhs->GetCompilerType().CompareTypes(rhs->GetCompilerType()) &&
         "invalid ast: operands must have the same type");

  Status status;
  lhs->SetValueFromInteger(rhs, status);
  if (status.Success())
    return lhs;
  return lldb::ValueObjectSP();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::EvaluateBinaryAddAssign(lldb::ValueObjectSP lhs,
                                     lldb::ValueObjectSP rhs, uint32_t loc) {
  lldb::ValueObjectSP ret;

  if (lhs->GetCompilerType().IsPointerType()) {
    assert(rhs->GetCompilerType().IsInteger() &&
           "invalid ast: rhs must be an integer");
    auto ret_or_err = EvaluateBinaryAddition(lhs, rhs, loc);
    if (!ret_or_err)
      return ret_or_err;
    ret = *ret_or_err;
  } else {
    assert(lhs->GetCompilerType().IsScalarType()
           && "invalid ast: lhs must be an arithmetic type");
    assert(rhs->GetCompilerType().IsBasicType() &&
           "invalid ast: rhs must be a basic type");
    ret = lhs->CastToBasicType(rhs->GetCompilerType());
    auto ret_or_err = EvaluateBinaryAddition(ret, rhs, loc);
    if (!ret_or_err)
      return ret_or_err;
    ret = *ret_or_err;
    ret = ret->CastToBasicType(lhs->GetCompilerType());
  }

  Status status;
  lhs->SetValueFromInteger(ret, status);
  if (status.Success())
    return lhs;
  return lldb::ValueObjectSP();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::EvaluateBinarySubAssign(lldb::ValueObjectSP lhs,
                                     lldb::ValueObjectSP rhs) {
  lldb::ValueObjectSP ret;

  if (lhs->GetCompilerType().IsPointerType()) {
    assert(rhs->GetCompilerType().IsInteger() &&
           "invalid ast: rhs must be an integer");
    auto ret_or_err =
        EvaluateBinarySubtraction(lhs, rhs, lhs->GetCompilerType());
    if (!ret_or_err)
      return ret_or_err;
    ret = *ret_or_err;
  } else {
    assert(lhs->GetCompilerType().IsScalarType()
           && "invalid ast: lhs must be an arithmetic type");
    assert(rhs->GetCompilerType().IsBasicType() &&
           "invalid ast: rhs must be a basic type");
    ret = lhs->CastToBasicType(rhs->GetCompilerType());
    auto ret_or_err =
        EvaluateBinarySubtraction(ret, rhs, ret->GetCompilerType());
    if (!ret_or_err)
      return ret_or_err;
    ret = *ret_or_err;
    ret = ret->CastToBasicType(lhs->GetCompilerType());
  }

  Status status;
  lhs->SetValueFromInteger(ret, status);
  if (status.Success())
    return lhs;
  return lldb::ValueObjectSP();
}

lldb::ValueObjectSP
Interpreter::EvaluateBinaryMulAssign(lldb::ValueObjectSP lhs,
                                     lldb::ValueObjectSP rhs) {
  assert(lhs->GetCompilerType().IsScalarType()
         && "invalid ast: lhs must be an arithmetic type");
  assert(rhs->GetCompilerType().IsBasicType()
         && "invalid ast: rhs must be a basic type");

  lldb::ValueObjectSP ret = lhs->CastToBasicType(rhs->GetCompilerType());
  ret = EvaluateBinaryMultiplication(ret, rhs);
  ret = ret->CastToBasicType(lhs->GetCompilerType());

  Status status;
  lhs->SetValueFromInteger(ret, status);
  if (status.Success())
    return lhs;
  return lldb::ValueObjectSP();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::EvaluateBinaryDivAssign(lldb::ValueObjectSP lhs,
                                     lldb::ValueObjectSP rhs, uint32_t loc) {
  assert(lhs->GetCompilerType().IsScalarType()
         && "invalid ast: lhs must be an arithmetic type");
  assert(rhs->GetCompilerType().IsBasicType()
         && "invalid ast: rhs must be a basic type");

  lldb::ValueObjectSP ret = lhs->CastToBasicType(rhs->GetCompilerType());
  auto ret_or_err = EvaluateBinaryDivision(ret, rhs, loc);
  if (!ret_or_err)
    return ret_or_err;
  ret = *ret_or_err;
  ret = ret->CastToBasicType(lhs->GetCompilerType());

  Status status;
  lhs->SetValueFromInteger(ret, status);
  if (status.Success())
    return lhs;
  return lldb::ValueObjectSP();
}

llvm::Expected<lldb::ValueObjectSP>
Interpreter::EvaluateBinaryRemAssign(lldb::ValueObjectSP lhs,
                                     lldb::ValueObjectSP rhs) {
  assert(lhs->GetCompilerType().IsScalarType()
         && "invalid ast: lhs must be an arithmetic type");
  assert(rhs->GetCompilerType().IsBasicType()
         && "invalid ast: rhs must be a basic type");

  lldb::ValueObjectSP ret = lhs->CastToBasicType(rhs->GetCompilerType());

  auto ret_or_err = EvaluateBinaryRemainder(ret, rhs);
  if (!ret_or_err)
    return ret_or_err;
  ret = *ret_or_err;
  ret = ret->CastToBasicType(lhs->GetCompilerType());

  Status status;
  lhs->SetValueFromInteger(ret, status);
  if (status.Success())
    return lhs;
  return lldb::ValueObjectSP();
}

lldb::ValueObjectSP Interpreter::EvaluateBinaryBitwiseAssign(
    BinaryOpKind kind, lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs) {
  switch (kind) {
    case BinaryOpKind::AndAssign:
      kind = BinaryOpKind::And;
      break;
    case BinaryOpKind::OrAssign:
      kind = BinaryOpKind::Or;
      break;
    case BinaryOpKind::XorAssign:
      kind = BinaryOpKind::Xor;
      break;
    default:
      assert(false && "invalid BinaryOpKind: must be '&=', '|=' or '^='");
      break;
  }
  assert(lhs->GetCompilerType().IsScalarType()
         && "invalid ast: lhs must be an arithmetic type");
  assert(rhs->GetCompilerType().IsBasicType() &&
         "invalid ast: rhs must be a basic type");

  lldb::ValueObjectSP ret = lhs->CastToBasicType(rhs->GetCompilerType());
  ret = EvaluateBinaryBitwise(kind, ret, rhs);
  ret = ret->CastToBasicType(lhs->GetCompilerType());

  Status status;
  lhs->SetValueFromInteger(ret, status);
  if (status.Success())
    return lhs;
  return lldb::ValueObjectSP();
}

llvm::Expected<lldb::ValueObjectSP> Interpreter::EvaluateBinaryShiftAssign(
    BinaryOpKind kind, lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs,
    CompilerType comp_assign_type) {
  switch (kind) {
    case BinaryOpKind::ShlAssign:
      kind = BinaryOpKind::Shl;
      break;
    case BinaryOpKind::ShrAssign:
      kind = BinaryOpKind::Shr;
      break;
    default:
      assert(false && "invalid BinaryOpKind: must be '<<=' or '>>='");
      break;
  }
  assert(lhs->GetCompilerType().IsScalarType()
         && "invalid ast: lhs must be an arithmetic type");
  assert(rhs->GetCompilerType().IsBasicType() &&
         "invalid ast: rhs must be a basic type");
  assert(comp_assign_type.IsInteger() &&
         "invalid ast: comp_assign_type must be an integer");

  lldb::ValueObjectSP ret = lhs->CastToBasicType(comp_assign_type);
  auto ret_or_err = EvaluateBinaryShift(kind, ret, rhs);
  if (!ret_or_err)
    return ret_or_err;
  ret = *ret_or_err;
  ret = ret->CastToBasicType(lhs->GetCompilerType());

  Status status;
  lhs->SetValueFromInteger(ret, status);
  if (status.Success())
    return lhs;
  return lldb::ValueObjectSP();
}

lldb::ValueObjectSP Interpreter::PointerAdd(lldb::ValueObjectSP lhs,
                                            int64_t offset) {
  uint64_t byte_size = 0;
  if (auto temp = lhs->GetCompilerType().GetPointeeType().GetByteSize(
          lhs->GetTargetSP().get()))
    byte_size = temp.value();
  uintptr_t addr = lhs->GetValueAsUnsigned(0) + offset * byte_size;

  llvm::StringRef name = "result";
  ExecutionContext exe_ctx(m_target.get(), false);
  return ValueObject::CreateValueObjectFromAddress(name, addr, exe_ctx,
                                                   lhs->GetCompilerType(),
                                                    /* do_deref */ false);
}

lldb::ValueObjectSP
Interpreter::ResolveContextVar(const std::string &name) const {
  auto it = m_context_vars.find(name);
  return it != m_context_vars.end() ? it->second : lldb::ValueObjectSP();
}

}  // namespace lldb_private::dil
