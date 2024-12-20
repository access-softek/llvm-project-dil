//===-- DILEval.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_VALUEOBJECT_DILEVAL_H_
#define LLDB_VALUEOBJECT_DILEVAL_H_

#include <memory>
#include <vector>

#include "lldb/ValueObject/DILAST.h"
#include "lldb/ValueObject/DILParser.h"

namespace lldb_private {

namespace dil {

class FlowAnalysis {
 public:
  FlowAnalysis(bool address_of_is_pending)
      : m_address_of_is_pending(address_of_is_pending) {}

  bool AddressOfIsPending() const { return m_address_of_is_pending; }
  void DiscardAddressOf() { m_address_of_is_pending = false; }

 private:
  bool m_address_of_is_pending;
};

class DILInterpreter : Visitor {
 public:
  DILInterpreter(lldb::TargetSP target,
                 std::shared_ptr<DILSourceManager> sm);
  DILInterpreter(lldb::TargetSP target,
                 std::shared_ptr<DILSourceManager> sm,
                 lldb::ValueObjectSP scope);
  DILInterpreter(lldb::TargetSP target,
                 std::shared_ptr<DILSourceManager> sm,
                 lldb::DynamicValueType use_dynamic);

  lldb::ValueObjectSP DILEval(const DILASTNode* tree, lldb::TargetSP target_sp,
                              Status& error);

  void SetContextVars(
      std::unordered_map<std::string, lldb::ValueObjectSP> context_vars);

 protected:
  lldb::ValueObjectSP DILEvalNode(const DILASTNode* node,
                                  FlowAnalysis* flow = nullptr);

  bool Success() { return m_error.Success(); }

  Status EvalError() { return std::move(m_error); };

  lldb::ValueObjectSP EvaluateMemberOf(lldb::ValueObjectSP value,
                                      const std::vector<uint32_t>& path,
                                      bool use_synthetic,
                                      bool is_dynamic);

 private:
  void SetError(ErrorCode error_code, std::string error,
                uint32_t loc);

  void Visit(const ErrorNode* node) override;
  void Visit(const ScalarLiteralNode* node) override;
  void Visit(const StringLiteralNode* node) override;
  void Visit(const IdentifierNode* node) override;
  void Visit(const SizeOfNode* node) override;
  void Visit(const BuiltinFunctionCallNode* node) override;
  void Visit(const CStyleCastNode* node) override;
  void Visit(const CxxStaticCastNode* node) override;
  void Visit(const CxxReinterpretCastNode* node) override;
  void Visit(const MemberOfNode* node) override;
  void Visit(const ArraySubscriptNode* node) override;
  void Visit(const BinaryOpNode* node) override;
  void Visit(const UnaryOpNode* node) override;
  void Visit(const TernaryOpNode* node) override;

  lldb::ValueObjectSP EvaluateComparison(BinaryOpKind kind,
                                         lldb::ValueObjectSP lhs,
                                         lldb::ValueObjectSP rhs);

  lldb::ValueObjectSP EvaluateDereference(lldb::ValueObjectSP rhs);

  lldb::ValueObjectSP EvaluateUnaryMinus(lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateUnaryNegation(lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateUnaryBitwiseNot(lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateUnaryPrefixIncrement(lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateUnaryPrefixDecrement(lldb::ValueObjectSP rhs);

  lldb::ValueObjectSP EvaluateBinaryAddition(lldb::ValueObjectSP lhs,
                                             lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinarySubtraction(lldb::ValueObjectSP lhs,
                                                lldb::ValueObjectSP rhs,
                                                CompilerType result_type);
  lldb::ValueObjectSP EvaluateBinaryMultiplication(lldb::ValueObjectSP lhs,
                                                   lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryDivision(lldb::ValueObjectSP lhs,
                                             lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryRemainder(lldb::ValueObjectSP lhs,
                                              lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryBitwise(BinaryOpKind kind,
                                            lldb::ValueObjectSP lhs,
                                            lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryShift(BinaryOpKind kind,
                                          lldb::ValueObjectSP lhs,
                                          lldb::ValueObjectSP rhs);

  lldb::ValueObjectSP EvaluateAssignment(lldb::ValueObjectSP lhs,
                                         lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryAddAssign(lldb::ValueObjectSP lhs,
                                              lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinarySubAssign(lldb::ValueObjectSP lhs,
                                              lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryMulAssign(lldb::ValueObjectSP lhs,
                                              lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryDivAssign(lldb::ValueObjectSP lhs,
                                              lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryRemAssign(lldb::ValueObjectSP lhs,
                                              lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryBitwiseAssign(BinaryOpKind kind,
                                                  lldb::ValueObjectSP lhs,
                                                  lldb::ValueObjectSP rhs);
  lldb::ValueObjectSP EvaluateBinaryShiftAssign(BinaryOpKind kind,
                                                lldb::ValueObjectSP lhs,
                                                lldb::ValueObjectSP rhs,
                                                CompilerType comp_assign_type);

  lldb::ValueObjectSP PointerAdd(lldb::ValueObjectSP lhs, int64_t offset);
  lldb::ValueObjectSP ResolveContextVar(const std::string& name) const;

  FlowAnalysis* flow_analysis() { return m_flow_analysis_chain.back(); }

 private:
  // Used by the interpreter to create objects, perform casts, etc.
  lldb::TargetSP m_target;

  std::shared_ptr<DILSourceManager> m_sm;

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

  lldb::ValueObjectSP m_result;

  lldb::ValueObjectSP m_scope;

  lldb::DynamicValueType m_default_dynamic;

  Status m_error;
};

}  // namespace dil

}  // namespace lldb_private

#endif  // LLDB_VALUEOBJECT_DILEVAL_H_
