//===-- DILEval.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_VALUEOBJECT_DILEVAL_H
#define LLDB_VALUEOBJECT_DILEVAL_H

#include "lldb/ValueObject/DILAST.h"
#include "lldb/ValueObject/DILParser.h"
#include <memory>
#include <vector>

namespace lldb_private::dil {

/// Given the name of an identifier (variable name, member name, type name,
/// etc.), find the ValueObject for that name (if it exists) and create and
/// return an IdentifierInfo object containing all the relevant information
/// about that object (for DIL parsing and evaluating).
lldb::ValueObjectSP LookupIdentifier(llvm::StringRef name_ref,
                                     std::shared_ptr<StackFrame> stack_frame,
                                     lldb::DynamicValueType use_dynamic,
                                     CompilerType *scope_ptr = nullptr);

lldb::ValueObjectSP LookupGlobalIdentifier(
    llvm::StringRef name_ref, std::shared_ptr<StackFrame> stack_frame,
    lldb::TargetSP target_sp, lldb::DynamicValueType use_dynamic,
    CompilerType *scope_ptr = nullptr);

/// Get the appropriate ValueObjectSP, consulting the use_dynamic and
/// use_synthetic options passed.
lldb::ValueObjectSP GetDynamicOrSyntheticValue(
    lldb::ValueObjectSP valobj_sp,
    lldb::DynamicValueType use_dynamic = lldb::eNoDynamicValues,
    bool use_synthetic = false);

class FlowAnalysis {
 public:
  FlowAnalysis(bool address_of_is_pending)
      : m_address_of_is_pending(address_of_is_pending) {}

  bool AddressOfIsPending() const { return m_address_of_is_pending; }
  void DiscardAddressOf() { m_address_of_is_pending = false; }

 private:
  bool m_address_of_is_pending;
};

class Interpreter : Visitor {
public:
  Interpreter(lldb::TargetSP target, llvm::StringRef expr,
              lldb::DynamicValueType use_dynamic,
              std::shared_ptr<StackFrame> frame_sp);

  llvm::Expected<lldb::ValueObjectSP> DILEval(const ASTNode *tree,
                                              lldb::TargetSP target_sp);

  void SetContextVars(
      std::unordered_map<std::string, lldb::ValueObjectSP> context_vars);

  bool AllowSideEffects() const { return m_allow_side_effects; }

  void SetAllowSideEffects(bool allow_side_effects) {
    m_allow_side_effects = allow_side_effects;
  }

 protected:
   llvm::Error BailOut(ErrorCode code, const std::string &message,
                       uint32_t loc);
   llvm::Expected<lldb::ValueObjectSP>
   DILEvalNode(const ASTNode *node, FlowAnalysis *flow = nullptr);

   lldb::ValueObjectSP EvaluateMemberOf(lldb::ValueObjectSP value,
                                        const std::vector<uint32_t> &path,
                                        bool use_synthetic, bool is_dynamic);

   lldb::ValueObjectSP FindMemberWithName(lldb::ValueObjectSP base,
                                          ConstString name, bool is_arrow);

 private:
   llvm::Expected<lldb::ValueObjectSP>
   Visit(const ScalarLiteralNode *node) override;
   llvm::Expected<lldb::ValueObjectSP>
   Visit(const StringLiteralNode *node) override;
   llvm::Expected<lldb::ValueObjectSP>
   Visit(const IdentifierNode *node) override;
   llvm::Expected<lldb::ValueObjectSP> Visit(const SizeOfNode *node) override;
   llvm::Expected<lldb::ValueObjectSP>
   Visit(const BuiltinFunctionCallNode *node) override;
   llvm::Expected<lldb::ValueObjectSP>
   Visit(const CStyleCastNode *node) override;
   llvm::Expected<lldb::ValueObjectSP>
   Visit(const CxxStaticCastNode *node) override;
   llvm::Expected<lldb::ValueObjectSP>
   Visit(const CxxReinterpretCastNode *node) override;
   llvm::Expected<lldb::ValueObjectSP> Visit(const MemberOfNode *node) override;
   llvm::Expected<lldb::ValueObjectSP>
   Visit(const ArraySubscriptNode *node) override;
   llvm::Expected<lldb::ValueObjectSP> Visit(const BinaryOpNode *node) override;
   llvm::Expected<lldb::ValueObjectSP> Visit(const UnaryOpNode *node) override;
   llvm::Expected<lldb::ValueObjectSP>
   Visit(const TernaryOpNode *node) override;

   llvm::Error PrepareIncrementDecrement(const UnaryOpNode *node,
                                         CompilerType rhs_type);
   llvm::Error PrepareBinaryLogical(lldb::ValueObjectSP &lhs,
                                    lldb::ValueObjectSP &rhs, uint32_t location,
                                    bool is_comp_assign);
   llvm::Error PrepareBinaryAddition(lldb::ValueObjectSP &lhs,
                                     lldb::ValueObjectSP &rhs,
                                     uint32_t location, bool is_comp_assign);
   llvm::Error PrepareBinarySubtraction(lldb::ValueObjectSP &lhs,
                                        lldb::ValueObjectSP &rhs,
                                        uint32_t location, bool is_comp_assign);
   llvm::Error PrepareBinaryOpScalar(lldb::ValueObjectSP &lhs,
                                     lldb::ValueObjectSP &rhs,
                                     uint32_t location, bool is_comp_assign);
   llvm::Error PrepareBinaryOpInteger(lldb::ValueObjectSP &lhs,
                                      lldb::ValueObjectSP &rhs,
                                      uint32_t location, bool is_comp_assign);
   llvm::Error PrepareBinaryShift(lldb::ValueObjectSP &lhs,
                                  lldb::ValueObjectSP &rhs, uint32_t location,
                                  bool is_comp_assign);
   llvm::Error PrepareBinaryComparison(BinaryOpKind kind,
                                       lldb::ValueObjectSP &lhs,
                                       lldb::ValueObjectSP &rhs,
                                       uint32_t location, bool is_comp_assign);
   llvm::Error PrepareAssignment(lldb::ValueObjectSP &lhs,
                                 lldb::ValueObjectSP &rhs, uint32_t location);
   llvm::Error CheckCompositeAssignment(const BinaryOpNode *node);
   llvm::Error PrepareCxxStaticCastForInheritedTypes(
       CompilerType type, lldb::ValueObjectSP rhs, uint32_t location,
       std::vector<uint32_t> &idx, uint64_t &offset,
       CxxStaticCastKind &cast_kind);

   lldb::ValueObjectSP EvaluateComparison(BinaryOpKind kind,
                                          lldb::ValueObjectSP lhs,
                                          lldb::ValueObjectSP rhs);

   lldb::ValueObjectSP EvaluateDereference(lldb::ValueObjectSP rhs);

   lldb::ValueObjectSP EvaluateUnaryMinus(lldb::ValueObjectSP rhs);
   lldb::ValueObjectSP EvaluateUnaryNegation(lldb::ValueObjectSP rhs);
   lldb::ValueObjectSP EvaluateUnaryBitwiseNot(lldb::ValueObjectSP rhs);
   lldb::ValueObjectSP EvaluateUnaryPrefixIncrement(lldb::ValueObjectSP rhs);
   lldb::ValueObjectSP EvaluateUnaryPrefixDecrement(lldb::ValueObjectSP rhs);

   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinaryAddition(lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs);
   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinarySubtraction(lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs);
   lldb::ValueObjectSP EvaluateBinaryMultiplication(lldb::ValueObjectSP lhs,
                                                    lldb::ValueObjectSP rhs);
   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinaryDivision(lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs,
                          uint32_t loc);
   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinaryRemainder(lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs);
   lldb::ValueObjectSP EvaluateBinaryBitwise(BinaryOpKind kind,
                                             lldb::ValueObjectSP lhs,
                                             lldb::ValueObjectSP rhs);
   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinaryShift(BinaryOpKind kind, lldb::ValueObjectSP lhs,
                       lldb::ValueObjectSP rhs);

   lldb::ValueObjectSP EvaluateAssignment(lldb::ValueObjectSP lhs,
                                          lldb::ValueObjectSP rhs);
   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinaryAddAssign(lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs,
                           uint32_t loc);
   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinarySubAssign(lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs);
   lldb::ValueObjectSP EvaluateBinaryMulAssign(lldb::ValueObjectSP lhs,
                                               lldb::ValueObjectSP rhs);
   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinaryDivAssign(lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs,
                           uint32_t loc);
   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinaryRemAssign(lldb::ValueObjectSP lhs, lldb::ValueObjectSP rhs);
   lldb::ValueObjectSP EvaluateBinaryBitwiseAssign(BinaryOpKind kind,
                                                   lldb::ValueObjectSP lhs,
                                                   lldb::ValueObjectSP rhs);
   llvm::Expected<lldb::ValueObjectSP>
   EvaluateBinaryShiftAssign(BinaryOpKind kind, lldb::ValueObjectSP lhs,
                             lldb::ValueObjectSP rhs,
                             CompilerType comp_assign_type);

   lldb::ValueObjectSP PointerAdd(lldb::ValueObjectSP lhs, int64_t offset);
   lldb::ValueObjectSP ResolveContextVar(const std::string &name) const;

   FlowAnalysis *flow_analysis() { return m_flow_analysis_chain.back(); }

 private:
  // Used by the interpreter to create objects, perform casts, etc.
  lldb::TargetSP m_target;

  llvm::StringRef m_expr;

  // Flow analysis chain represents the expression evaluation flow for the
  // current code branch. Each node in the chain corresponds to an AST node,
  // describing the semantics of the evaluation for it. Currently, flow analysis
  // propagates the information about the pending address-of operator, so that
  // combination of address-of and a subsequent dereference can be eliminated.
  // End of the chain (i.e. `back()`) contains the flow analysis instance for
  // the current node. It may be `nullptr` if no relevant information is
  // available, the caller/user is supposed to check.
  std::vector<FlowAnalysis*> m_flow_analysis_chain;

  std::unordered_map<std::string, lldb::ValueObjectSP> m_context_vars;

  lldb::ValueObjectSP m_scope;

  lldb::DynamicValueType m_default_dynamic;

  std::shared_ptr<StackFrame> m_exe_ctx_scope;

  bool m_allow_side_effects = true;
};

}  // namespace lldb_private::dil

#endif  // LLDB_VALUEOBJECT_DILEVAL_H
