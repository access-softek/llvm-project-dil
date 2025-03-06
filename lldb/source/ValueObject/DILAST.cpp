//===-- DILAST.cpp --------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/ValueObject/DILAST.h"
#include "lldb/API/SBType.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Symbol/VariableList.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/ValueObject/ValueObjectRegister.h"
#include "lldb/ValueObject/ValueObjectVariable.h"
#include "llvm/ADT/StringRef.h"
#include <vector>

namespace lldb_private::dil {

BinaryOpKind dil_token_kind_to_binary_op_kind(Token::Kind token_kind) {
  switch (token_kind) {
  case Token::star:
    return BinaryOpKind::Mul;
  case Token::amp:
    return BinaryOpKind::And;
  case Token::minus:
    return BinaryOpKind::Sub;
  case Token::slash:
    return BinaryOpKind::Div;
  case Token::percent:
    return BinaryOpKind::Rem;
  case Token::plus:
    return BinaryOpKind::Add;
  case Token::lessless:
    return BinaryOpKind::Shl;
  case Token::greatergreater:
    return BinaryOpKind::Shr;
  case Token::less:
    return BinaryOpKind::LT;
  case Token::greater:
    return BinaryOpKind::GT;
  case Token::lessequal:
    return BinaryOpKind::LE;
  case Token::greaterequal:
    return BinaryOpKind::GE;
  case Token::equalequal:
    return BinaryOpKind::EQ;
  case Token::exclaimequal:
    return BinaryOpKind::NE;
  case Token::caret:
    return BinaryOpKind::Xor;
  case Token::pipe:
    return BinaryOpKind::Or;
  case Token::ampamp:
    return BinaryOpKind::LAnd;
  case Token::pipepipe:
    return BinaryOpKind::LOr;
  case Token::equal:
    return BinaryOpKind::Assign;
  case Token::starequal:
    return BinaryOpKind::MulAssign;
  case Token::slashequal:
    return BinaryOpKind::DivAssign;
  case Token::percentequal:
    return BinaryOpKind::RemAssign;
  case Token::plusequal:
    return BinaryOpKind::AddAssign;
  case Token::minusequal:
    return BinaryOpKind::SubAssign;
  case Token::lesslessequal:
    return BinaryOpKind::ShlAssign;
  case Token::greatergreaterequal:
    return BinaryOpKind::ShrAssign;
  case Token::ampequal:
    return BinaryOpKind::AndAssign;
  case Token::caretequal:
    return BinaryOpKind::XorAssign;
  case Token::pipeequal:
    return BinaryOpKind::OrAssign;
  default:
    break;
  }
  llvm_unreachable("did you add an element to BinaryOpKind?");
}

bool binary_op_kind_is_comp_assign(BinaryOpKind kind) {
  switch (kind) {
  case BinaryOpKind::Assign:
  case BinaryOpKind::MulAssign:
  case BinaryOpKind::DivAssign:
  case BinaryOpKind::RemAssign:
  case BinaryOpKind::AddAssign:
  case BinaryOpKind::SubAssign:
  case BinaryOpKind::ShlAssign:
  case BinaryOpKind::ShrAssign:
  case BinaryOpKind::AndAssign:
  case BinaryOpKind::XorAssign:
  case BinaryOpKind::OrAssign:
    return true;

  default:
    return false;
  }
}

CompilerType ASTNode::GetDereferencedResultType() const {
  auto type = result_type();
  return type.IsReferenceType() ? type.GetNonReferenceType() : type;
}

llvm::Expected<lldb::ValueObjectSP> ErrorNode::Accept(Visitor *v) const {
  llvm_unreachable("Attempted to Visit a DIL ErrorNode");
}

llvm::Expected<lldb::ValueObjectSP>
ScalarLiteralNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP>
StringLiteralNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP> IdentifierNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP> SizeOfNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP>
BuiltinFunctionCallNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP> CStyleCastNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP>
CxxStaticCastNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP>
CxxReinterpretCastNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP> MemberOfNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP>
ArraySubscriptNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP> BinaryOpNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP> UnaryOpNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

llvm::Expected<lldb::ValueObjectSP> TernaryOpNode::Accept(Visitor *v) const {
  return v->Visit(this);
}

}  // namespace lldb_private::dil
