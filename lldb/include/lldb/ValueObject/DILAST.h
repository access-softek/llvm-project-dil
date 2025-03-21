//===-- DILAST.h ------------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_VALUEOBJECT_DILAST_H
#define LLDB_VALUEOBJECT_DILAST_H

#include "lldb/Symbol/Type.h"
#include "lldb/Symbol/TypeList.h"
#include "lldb/Target/LanguageRuntime.h"
#include "lldb/Utility/ConstString.h"
#include "lldb/ValueObject/DILLexer.h"
#include "lldb/ValueObject/ValueObject.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Support/Casting.h"
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lldb_private::dil {

/// The various types DIL AST nodes (used by the DIL parser).
enum class NodeKind {
  eErrorNode,
  eScalarLiteralNode,
  eStringLiteralNode,
  eIdentifierNode,
  eSizeOfNode,
  eBuiltinFunctionCallNode,
  eCStyleCastNode,
  eCxxStaticCastNode,
  eCxxReinterpretCastNode,
  eMemberOfNode,
  eArraySubscriptNode,
  eBinaryOpNode,
  eUnaryOpNode,
  eTernaryOpNode,
};


/// Type promotion cast kinds in DIL.
enum class TypePromotionCastKind {
  eArithmetic,
  ePointer,
  eNone,
};

/// The C-Style casts allowed by DIL.
enum class CStyleCastKind {
  eEnumeration,
  eNullptr,
  eReference,
  eNone,
};

/// The Cxx static casts allowed by DIL.
enum class CxxStaticCastKind {
  eNoOp,
  eEnumeration,
  eNullptr,
  eBaseToDerived,
  eDerivedToBase,
  eNone,
};

/// The binary operators recognized by DIL.
enum class BinaryOpKind {
  Mul,       // "*"
  Div,       // "/"
  Rem,       // "%"
  Add,       // "+"
  Sub,       // "-"
  Shl,       // "<<"
  Shr,       // ">>"
  LT,        // "<"
  GT,        // ">"
  LE,        // "<="
  GE,        // ">="
  EQ,        // "=="
  NE,        // "!="
  And,       // "&"
  Xor,       // "^"
  Or,        // "|"
  LAnd,      // "&&"
  LOr,       // "||"
  Assign,    // "="
  MulAssign, // "*="
  DivAssign, // "/="
  RemAssign, // "%="
  AddAssign, // "+="
  SubAssign, // "-="
  ShlAssign, // "<<="
  ShrAssign, // ">>="
  AndAssign, // "&="
  XorAssign, // "^="
  OrAssign,  // "|="
};

/// The Unary operators recognized by DIL.
enum class UnaryOpKind {
  PostInc, // "++"
  PostDec, // "--"
  PreInc,  // "++"
  PreDec,  // "--"
  AddrOf,  // "&"
  Deref,   // "*"
  Plus,    // "+"
  Minus,   // "-"
  Not,     // "~"
  LNot,    // "!"
};

/// Helper functions for DIL AST node parsing.

/// Translates DIL tokens to BinaryOpKind.
BinaryOpKind
    dil_token_kind_to_binary_op_kind(Token::Kind token_kind);

/// Returns bool indicating whether or not the input kind is an assignment.
bool binary_op_kind_is_comp_assign(BinaryOpKind kind);

/// Given a string representing a type, returns the CompilerType corresponding
/// to the named type, if it exists.
CompilerType
ResolveTypeByName(const std::string &name,
                  std::shared_ptr<ExecutionContextScope> ctx_scope);

/// Forward declaration, for use in DIL AST nodes. Definition is at the very
/// end of this file.
class Visitor;

/// The rest of the classes in this file, except for the Visitor class at the
/// very end, define all the types of AST nodes used by the DIL parser and
/// expression evaluator. The DIL parser parses the input string and creates the
/// AST parse tree from the AST nodes. The resulting AST node tree gets passed
/// to the DIL expression evaluator, which evaluates the DIL AST nodes and
/// creates/returns a ValueObjectSP containing the result.

/// Base class for AST nodes used by the Data Inspection Language (DIL) parser.
/// All of the specialized types of AST nodes inherit from this (virtual) base
/// class.
class ASTNode {
public:
  ASTNode(uint32_t location, NodeKind kind)
      : m_location(location), m_kind(kind) {}
  virtual ~ASTNode() = default;

  virtual llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const = 0;

  virtual bool is_rvalue() const { return false; }
  virtual bool is_bitfield() const { return false; }
  virtual bool is_context_var() const { return false; }
  virtual bool is_literal_zero() const { return false; }
  virtual uint32_t bitfield_size() const { return 0; }
  virtual CompilerType result_type() const = 0;
  virtual ValueObject *valobj() const { return nullptr; }

  uint32_t GetLocation() const { return m_location; }
  NodeKind GetKind() const { return m_kind; }

  // The expression result type, but dereferenced in case it's a reference. This
  // is for convenience, since for the purposes of the semantic analysis only
  // the dereferenced type matters.
  CompilerType GetDereferencedResultType() const;

private:
  uint32_t m_location;
  const NodeKind m_kind;
};

using ASTNodeUP = std::unique_ptr<ASTNode>;

class ErrorNode : public ASTNode {
public:
  ErrorNode() : ASTNode(0, NodeKind::eErrorNode) {}
  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  CompilerType result_type() const override {
    CompilerType bad_type;
    return bad_type;
  }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eErrorNode;
  }
};

class ScalarLiteralNode : public ASTNode {
public:
  ScalarLiteralNode(uint32_t location, CompilerType type, Scalar value)
      : ASTNode(location, NodeKind::eScalarLiteralNode), m_type(type),
        m_value(value) {}

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return true; }
  bool is_literal_zero() const override {
    return m_value.IsZero() && !m_type.IsBoolean();
  }
  CompilerType result_type() const override { return m_type; }

  Scalar GetValue() const & { return m_value; }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eScalarLiteralNode;
  }

private:
  CompilerType m_type;
  Scalar m_value;
};

class StringLiteralNode : public ASTNode {
public:
  StringLiteralNode(uint32_t location, CompilerType type, std::string value)
      : ASTNode(location, NodeKind::eStringLiteralNode), m_type(type),
        m_value(value) {}

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return true; }
  CompilerType result_type() const override { return m_type; }

  std::string GetValue() const & { return m_value; }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eStringLiteralNode;
  }

private:
  CompilerType m_type;
  std::string m_value;
};

class IdentifierNode : public ASTNode {
public:
  IdentifierNode(uint32_t location, std::string name,
                 lldb::DynamicValueType use_dynamic,
                 lldb::ValueObjectSP id_valobj, bool is_rvalue,
                 bool is_context_var)
      : ASTNode(location, NodeKind::eIdentifierNode), m_is_rvalue(is_rvalue),
        m_is_context_var(is_context_var), m_name(std::move(name)),
        m_use_dynamic(use_dynamic), m_id_valobj(std::move(id_valobj)) {}

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return m_is_rvalue; }
  bool is_context_var() const override { return m_is_context_var; };
  CompilerType result_type() const override {
    return m_id_valobj->GetCompilerType();
  }
  ValueObject *valobj() const override { return m_id_valobj.get(); }

  lldb::DynamicValueType GetUseDynamic() const { return m_use_dynamic; }
  std::string GetName() const { return m_name; }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eIdentifierNode;
  }

private:
  bool m_is_rvalue;
  bool m_is_context_var;
  std::string m_name;
  lldb::DynamicValueType m_use_dynamic;
  lldb::ValueObjectSP m_id_valobj;
};

class SizeOfNode : public ASTNode {
public:
  SizeOfNode(uint32_t location, CompilerType type, CompilerType operand)
      : ASTNode(location, NodeKind::eSizeOfNode), m_type(type),
        m_operand(operand) {}

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return true; }
  CompilerType result_type() const override { return m_type; }

  CompilerType operand() const { return m_operand; }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eSizeOfNode;
  }

private:
  CompilerType m_type;
  CompilerType m_operand;
};

class BuiltinFunctionCallNode : public ASTNode {
public:
  BuiltinFunctionCallNode(uint32_t location, CompilerType result_type,
                          std::string name, std::vector<ASTNodeUP> arguments)
      : ASTNode(location, NodeKind::eBuiltinFunctionCallNode),
        m_result_type(result_type), m_name(std::move(name)),
        m_arguments(std::move(arguments)) {}

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return true; }
  CompilerType result_type() const override { return m_result_type; }

  std::string name() const { return m_name; }
  const std::vector<ASTNodeUP> &arguments() const { return m_arguments; };

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eBuiltinFunctionCallNode;
  }

private:
  CompilerType m_result_type;
  std::string m_name;
  std::vector<ASTNodeUP> m_arguments;
};

class CStyleCastNode : public ASTNode {
public:
  CStyleCastNode(uint32_t location, CompilerType type, ASTNodeUP operand,
                 CStyleCastKind kind)
      : ASTNode(location, NodeKind::eCStyleCastNode), m_type(type),
        m_operand(std::move(operand)), m_cast_kind(kind) {
    m_promo_kind = TypePromotionCastKind::eNone;
  }

  CStyleCastNode(uint32_t location, CompilerType type, ASTNodeUP operand,
                 TypePromotionCastKind kind)
      : ASTNode(location, NodeKind::eCStyleCastNode), m_type(type),
        m_operand(std::move(operand)), m_promo_kind(kind) {
    m_cast_kind = CStyleCastKind::eNone;
  }

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override {
    return m_cast_kind != CStyleCastKind::eReference;
  }
  CompilerType result_type() const override { return m_type; }
  ValueObject *valobj() const override { return m_operand->valobj(); }

  CompilerType type() const { return m_type; }
  ASTNode *operand() const { return m_operand.get(); }
  CStyleCastKind cast_kind() const { return m_cast_kind; }
  TypePromotionCastKind promo_kind() const { return m_promo_kind; }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eCStyleCastNode;
  }

private:
  CompilerType m_type;
  ASTNodeUP m_operand;
  CStyleCastKind m_cast_kind;
  TypePromotionCastKind m_promo_kind;
};

class CxxStaticCastNode : public ASTNode {
public:
  CxxStaticCastNode(uint32_t location, CompilerType type, ASTNodeUP operand,
                    CxxStaticCastKind kind, bool is_rvalue,
                    CompilerType orig_type)
      : ASTNode(location, NodeKind::eCxxStaticCastNode), m_type(type),
        m_operand(std::move(operand)), m_cast_kind(kind),
        m_is_rvalue(is_rvalue), m_orig_type(orig_type) {
    assert(kind != CxxStaticCastKind::eBaseToDerived &&
           kind != CxxStaticCastKind::eDerivedToBase &&
           "invalid constructor for base-to-derived and derived-to-base casts");
    m_promo_kind = TypePromotionCastKind::eNone;
  }

  CxxStaticCastNode(uint32_t location, CompilerType type, ASTNodeUP operand,
                    CxxStaticCastKind kind, bool is_rvalue)
      : ASTNode(location, NodeKind::eCxxStaticCastNode), m_type(type),
        m_operand(std::move(operand)), m_cast_kind(kind),
        m_is_rvalue(is_rvalue), m_orig_type(type) {
    assert(kind != CxxStaticCastKind::eBaseToDerived &&
           kind != CxxStaticCastKind::eDerivedToBase &&
           "invalid constructor for base-to-derived and derived-to-base casts");
    m_promo_kind = TypePromotionCastKind::eNone;
  }

  CxxStaticCastNode(uint32_t location, CompilerType type, ASTNodeUP operand,
                    TypePromotionCastKind kind, bool is_rvalue)
      : ASTNode(location, NodeKind::eCxxStaticCastNode), m_type(type),
        m_operand(std::move(operand)), m_promo_kind(kind),
        m_is_rvalue(is_rvalue), m_orig_type(type) {
    m_cast_kind = CxxStaticCastKind::eNone;
  }

  CxxStaticCastNode(uint32_t location, CompilerType type, ASTNodeUP operand,
                    std::vector<uint32_t> idx, bool is_rvalue)
      : ASTNode(location, NodeKind::eCxxStaticCastNode), m_type(type),
        m_operand(std::move(operand)), m_idx(std::move(idx)),
        m_cast_kind(CxxStaticCastKind::eDerivedToBase), m_is_rvalue(is_rvalue),
        m_orig_type(type) {
    m_promo_kind = TypePromotionCastKind::eNone;
  }

  CxxStaticCastNode(uint32_t location, CompilerType type, ASTNodeUP operand,
                    uint64_t offset, bool is_rvalue)
      : ASTNode(location, NodeKind::eCxxStaticCastNode), m_type(type),
        m_operand(std::move(operand)), m_offset(offset),
        m_cast_kind(CxxStaticCastKind::eBaseToDerived), m_is_rvalue(is_rvalue),
        m_orig_type(type) {
    m_promo_kind = TypePromotionCastKind::eNone;
  }

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return m_is_rvalue; }
  CompilerType result_type() const override { return m_type; }
  ValueObject *valobj() const override { return m_operand->valobj(); }

  CompilerType type() const { return m_type; }
  CompilerType orig_type() const { return m_orig_type; }
  ASTNode *operand() const { return m_operand.get(); }
  const std::vector<uint32_t> &idx() const { return m_idx; }
  uint64_t offset() const { return m_offset; }
  CxxStaticCastKind cast_kind() const { return m_cast_kind; }
  TypePromotionCastKind promo_kind() const { return m_promo_kind; }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eCxxStaticCastNode;
  }

private:
  CompilerType m_type;
  ASTNodeUP m_operand;
  std::vector<uint32_t> m_idx;
  uint64_t m_offset = 0;
  CxxStaticCastKind m_cast_kind;
  TypePromotionCastKind m_promo_kind;
  bool m_is_rvalue;
  CompilerType m_orig_type;
};

class CxxReinterpretCastNode : public ASTNode {
public:
  CxxReinterpretCastNode(uint32_t location, CompilerType type,
                         ASTNodeUP operand, bool is_rvalue)
      : ASTNode(location, NodeKind::eCxxReinterpretCastNode), m_type(type),
        m_operand(std::move(operand)), m_is_rvalue(is_rvalue) {}

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return m_is_rvalue; }
  CompilerType result_type() const override { return m_type; }
  ValueObject *valobj() const override { return m_operand->valobj(); }

  CompilerType type() const { return m_type; }
  ASTNode *operand() const { return m_operand.get(); }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eCxxReinterpretCastNode;
  }

private:
  CompilerType m_type;
  ASTNodeUP m_operand;
  bool m_is_rvalue;
};

class MemberOfNode : public ASTNode {
public:
  MemberOfNode(uint32_t location, ASTNodeUP base,
               std::optional<uint32_t> bitfield_size, bool is_arrow,
               ConstString name)
      : ASTNode(location, NodeKind::eMemberOfNode), m_base(std::move(base)),
        m_bitfield_size(bitfield_size), m_is_arrow(is_arrow),
        m_field_name(name) {
    CompilerType empty_type;
    m_result_type = empty_type;
  }

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return false; }
  bool is_bitfield() const override { return m_bitfield_size ? true : false; }
  uint32_t bitfield_size() const override {
    return m_bitfield_size ? m_bitfield_size.value() : 0;
  }
  CompilerType result_type() const override { return m_result_type; }

  ASTNode *base() const { return m_base.get(); }
  bool is_arrow() const { return m_is_arrow; }
  ConstString field_name() const { return m_field_name; }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eMemberOfNode;
  }

private:
  CompilerType m_result_type;
  ASTNodeUP m_base;
  std::optional<uint32_t> m_bitfield_size;
  bool m_is_arrow;
  ConstString m_field_name;
};

class ArraySubscriptNode : public ASTNode {
public:
  ArraySubscriptNode(uint32_t location, CompilerType result_type,
                     ASTNodeUP base, ASTNodeUP index)
      : ASTNode(location, NodeKind::eArraySubscriptNode),
        m_result_type(result_type), m_base(std::move(base)),
        m_index(std::move(index)) {}

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return false; }
  CompilerType result_type() const override { return m_result_type; }
  ValueObject *valobj() const override {
    ValueObject *base_obj = m_base->valobj();
    ValueObject *idx_obj = m_index->valobj();
    Status error;
    int idx = 0;
    if (idx_obj && idx_obj->GetCompilerType().IsReferenceType())
      idx_obj = idx_obj->Dereference(error).get();
    if (idx_obj && error.Success())
      idx = idx_obj->GetValueAsUnsigned(0);
    if (base_obj->GetChildAtIndex(idx))
      return (base_obj->GetChildAtIndex(idx)).get();
    return base_obj;
  }

  ASTNode *base() const { return m_base.get(); }
  ASTNode *index() const { return m_index.get(); }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eArraySubscriptNode;
  }

private:
  CompilerType m_result_type;
  ASTNodeUP m_base;
  ASTNodeUP m_index;
};

class BinaryOpNode : public ASTNode {
public:
  BinaryOpNode(uint32_t location, CompilerType result_type, BinaryOpKind kind,
               ASTNodeUP lhs, ASTNodeUP rhs, CompilerType comp_assign_type,
               ValueObject *val_obj_ptr = nullptr)
      : ASTNode(location, NodeKind::eBinaryOpNode), m_result_type(result_type),
        m_kind(kind), m_lhs(std::move(lhs)), m_rhs(std::move(rhs)),
        m_comp_assign_type(comp_assign_type) {
    if (val_obj_ptr)
      m_val_obj_sp = val_obj_ptr->GetSP();
  }

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override {
    return !binary_op_kind_is_comp_assign(m_kind);
  }
  CompilerType result_type() const override { return m_result_type; }
  ValueObject *valobj() const override {
    if (m_val_obj_sp)
      return m_val_obj_sp.get();
    ValueObject *rhs_valobj = m_rhs->valobj();
    ValueObject *lhs_valobj = m_lhs->valobj();
    if (lhs_valobj)
      return lhs_valobj;
    if (rhs_valobj)
      return rhs_valobj;
    return m_val_obj_sp.get();
  }

  BinaryOpKind kind() const { return m_kind; }
  ASTNode *lhs() const { return m_lhs.get(); }
  ASTNode *rhs() const { return m_rhs.get(); }
  CompilerType comp_assign_type() const { return m_comp_assign_type; }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eBinaryOpNode;
  }

private:
  CompilerType m_result_type;
  BinaryOpKind m_kind;
  ASTNodeUP m_lhs;
  ASTNodeUP m_rhs;
  CompilerType m_comp_assign_type;
  lldb::ValueObjectSP m_val_obj_sp;
};

class UnaryOpNode : public ASTNode {
public:
  UnaryOpNode(uint32_t location, UnaryOpKind kind, ASTNodeUP rhs)
      : ASTNode(location, NodeKind::eUnaryOpNode), m_kind(kind),
        m_rhs(std::move(rhs)) {}

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override { return m_kind != UnaryOpKind::Deref; }
  CompilerType result_type() const override { return m_result_type; }
  ValueObject *valobj() const override { return m_rhs->valobj(); }

  UnaryOpKind kind() const { return m_kind; }
  ASTNode *rhs() const { return m_rhs.get(); }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eUnaryOpNode;
  }

private:
  CompilerType m_result_type;
  UnaryOpKind m_kind;
  ASTNodeUP m_rhs;
};

class TernaryOpNode : public ASTNode {
public:
  TernaryOpNode(uint32_t location, CompilerType result_type, ASTNodeUP cond,
                ASTNodeUP lhs, ASTNodeUP rhs)
      : ASTNode(location, NodeKind::eTernaryOpNode), m_result_type(result_type),
        m_cond(std::move(cond)), m_lhs(std::move(lhs)), m_rhs(std::move(rhs)) {}

  llvm::Expected<lldb::ValueObjectSP> Accept(Visitor *v) const override;
  bool is_rvalue() const override {
    return m_lhs->is_rvalue() || m_rhs->is_rvalue();
  }
  bool is_bitfield() const override {
    return m_lhs->is_bitfield() || m_rhs->is_bitfield();
  }
  CompilerType result_type() const override { return m_result_type; }

  ASTNode *cond() const { return m_cond.get(); }
  ASTNode *lhs() const { return m_lhs.get(); }
  ASTNode *rhs() const { return m_rhs.get(); }

  static bool classof(const ASTNode *node) {
    return node->GetKind() == NodeKind::eTernaryOpNode;
  }

private:
  CompilerType m_result_type;
  ASTNodeUP m_cond;
  ASTNodeUP m_lhs;
  ASTNodeUP m_rhs;
};

/// This class contains one Visit method for each specialized type of
/// DIL AST node. The Visit methods are used to dispatch a DIL AST node to
/// the correct function in the DIL expression evaluator for evaluating that
/// type of AST node.
class Visitor {
public:
  virtual ~Visitor() = default;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const ScalarLiteralNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const StringLiteralNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const IdentifierNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP> Visit(const SizeOfNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const BuiltinFunctionCallNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const CStyleCastNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const CxxStaticCastNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const CxxReinterpretCastNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const MemberOfNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const ArraySubscriptNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const BinaryOpNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const UnaryOpNode *node) = 0;
  virtual llvm::Expected<lldb::ValueObjectSP>
  Visit(const TernaryOpNode *node) = 0;
};

} // namespace lldb_private::dil

#endif // LLDB_VALUEOBJECT_DILAST_H
