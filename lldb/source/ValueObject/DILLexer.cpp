//===-- DILLexer.cpp ------------------------------------------------------===//
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

#include "lldb/ValueObject/DILLexer.h"
#include "clang/Basic/CharInfo.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/Unicode.h"
#include <tuple>

namespace lldb_private::dil {

llvm::StringRef Token::GetTokenName(Kind kind) {
  switch (kind){
    case Token::amp: return "amp";
    case Token::ampamp: return "ampamp";
    case Token::ampequal: return "ampequal";
    case Token::arrow: return "arrow";
    case Token::caret: return "caret";
    case Token::caretequal: return "caretequal";
    case Token::colon: return "colon";
    case Token::coloncolon: return "coloncolon";
    case Token::comma: return "comma";
    case Token::eof: return "eof";
    case Token::equal:
      return "equal";
    case Token::equalequal: return "equalequal";
    case Token::exclaim: return "exclaim";
    case Token::exclaimequal: return "exclaimequal";
    case Token::flt: return "flt";
    case Token::greater: return "greater";
    case Token::greaterequal: return "greaterequal";
    case Token::greatergreater: return "greatergreater";
    case Token::greatergreaterequal: return "greatergreaterequal";
    case Token::identifier: return "identifier";
    case Token::integer: return "integer";
    case Token::kw_bool:
      return "bool";
    case Token::kw_char:
      return "char";
    case Token::kw_char16_t:
      return "char16_t";
    case Token::kw_char32_t:
      return "char32_t";
    case Token::kw_const:
      return "const";
    case Token::kw_double:
      return "double";
    case Token::kw_dynamic_cast:
      return "dynamic_cast";
    case Token::kw_false:
      return "false";
    case Token::kw_float:
      return "float";
    case Token::kw_int:
      return "int";
    case Token::kw_long:
      return "long";
    case Token::kw_namespace:
      return "namespace";
    case Token::kw_nullptr:
      return "nullptr";
    case Token::kw_reinterpret_cast:
      return "reinterpret_cast";
    case Token::kw_short:
      return "short";
    case Token::kw_signed:
      return "signed";
    case Token::kw_sizeof:
      return "sizeof";
    case Token::kw_static_cast:
      return "static_cast";
    case Token::kw_this:
      return "this";
    case Token::kw_true:
      return "true";
    case Token::kw_unsigned:
      return "unsigned";
    case Token::kw_void:
      return "void";
    case Token::kw_volatile:
      return "volatile";
    case Token::kw_wchar_t:
      return "wchar_t";
    case Token::less: return "less";
    case Token::lessequal: return "lessequal";
    case Token::lessless: return "lessless";
    case Token::lesslessequal: return "lesslessequal";
    case Token::l_square: return "l_square";
    case Token::l_paren: return "l_paren";
    case Token::minus: return "minus";
    case Token::minusequal: return "minusequal";
    case Token::minusminus: return "minusminus";
    case Token::numeric_constant: return "numeric_constant";
    case Token::percent: return "percent";
    case Token::percentequal: return "percentequal";
    case Token::period: return "period";
    case Token::pipe: return "pipe";
    case Token::pipeequal: return "pipeequal";
    case Token::pipepipe: return "pipepipe";
    case Token::plus: return "plus";
    case Token::plusequal: return "plusequal";
    case Token::plusplus: return "plusplus";
    case Token::question: return "question";
    case Token::r_paren: return "r_paren";
    case Token::r_square: return "r_square";
    case Token::slash: return "slash";
    case Token::slashequal: return "slashequal";
    case Token::star: return "star";
    case Token::starequal: return "starequal";
    case Token::string_literal: return "string_literal";
    case Token::tilde: return "tilde";
    case Token::utf8_string_literal: return "utf8_string_literal";
    case Token::wide_string_literal: return "wide_string_literal";
    case Token::char_constant: return "char_constant";
    case Token::wide_char_constant: return "wide_char_constant";
    case Token::utf8_char_constant: return "utf8_char_constant";
  }
}

static bool IsLetter(char c) {
  return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z');
}

static bool IsDigit(char c) { return ('0' <= c && c <= '9'); }

inline bool IsOperator(unsigned char c) {
  using namespace clang::charinfo;
  return (InfoTable[c] & (CHAR_PUNCT | CHAR_PERIOD)) != 0;
}

static bool IsValidIdentifierContinuation(char c) {
  if (c == '$')
    return true;
  return !IsOperator(c) && !clang::isWhitespace(c);
}

static std::optional<llvm::StringRef> IsWord(llvm::StringRef &remainder) {
  llvm::StringRef::iterator cur_pos = remainder.begin();
  llvm::StringRef::iterator start = cur_pos;

  if (IsDigit(*cur_pos))
    return std::nullopt;

  while (cur_pos < remainder.end()) {
    uint8_t c = *cur_pos;
    if (c < 0x80) {
      if (IsValidIdentifierContinuation(c)) {
        cur_pos++;
        continue;
      } else
        break;
    }
    if (llvm::isLegalUTF8Sequence((const llvm::UTF8 *)cur_pos,
                                  (const llvm::UTF8 *)remainder.end())) {
      cur_pos += llvm::getNumBytesForUTF8(*cur_pos);
      continue;
    }
    break;
  }

  if (cur_pos == start)
    return std::nullopt;

  auto length = cur_pos - start;
  remainder = remainder.drop_front(length);
  return llvm::StringRef(start, length);
}

static void ConsumeNumberBody(uint32_t &length, char &prev_ch,
                              llvm::StringRef::iterator &cur_pos,
                              llvm::StringRef expr) {
  while (cur_pos != expr.end() &&
         (IsDigit(*cur_pos) || IsLetter(*cur_pos) || *cur_pos == '_')) {
    prev_ch = *cur_pos;
    length++;
    cur_pos++;
  }
}

static std::optional<llvm::StringRef> IsNumber(llvm::StringRef expr,
                                               llvm::StringRef &remainder) {
  llvm::StringRef::iterator cur_pos = remainder.begin();
  llvm::StringRef::iterator start = cur_pos;
  uint32_t length = 0;
  char prev_ch = 0;
  dil::NumberKind kind = dil::NumberKind::eInteger;
  if (*start == '.') {
    auto next_pos = start + 1;
    if (next_pos == expr.end() || !IsDigit(*next_pos))
      return std::nullopt;
  }
  if (IsDigit(*start) || *start == '.') {
    ConsumeNumberBody(length, prev_ch, cur_pos, expr);
    // We're not at the end of the string, and we should be looking at a '.'
    if (*cur_pos == '.') {
      kind = dil::NumberKind::eFloat;
      prev_ch = *cur_pos;
      length++;
      cur_pos++;
      ConsumeNumberBody(length, prev_ch, cur_pos, expr);
    }
    // Check the exponent part
    if ((*cur_pos == '-' || *cur_pos == '+') &&
        (prev_ch == 'E' || prev_ch == 'e' || prev_ch == 'P' ||
         prev_ch == 'p')) {
      prev_ch = *cur_pos;
      length++;
      cur_pos++;
      ConsumeNumberBody(length, prev_ch, cur_pos, expr);
    }
    llvm::StringRef number = expr.substr(start - expr.begin(),
                                         cur_pos - start);
    if (remainder.consume_front(number))
      return number;
  }
  return std::nullopt;
}

llvm::Expected<DILLexer> DILLexer::Create(llvm::StringRef expr) {
  std::vector<Token> tokens;
  llvm::StringRef remainder = expr;
  uint32_t position = 0;
  do {
    if (llvm::Expected<Token> t = Lex(expr, remainder, position)) {
      tokens.push_back(std::move(*t));
    } else {
      return t.takeError();
    }
  } while (tokens.back().GetKind() != Token::eof);

  return DILLexer(expr, std::move(tokens));
}

llvm::Expected<Token> DILLexer::Lex(llvm::StringRef expr,
                                    llvm::StringRef &remainder,
                                    uint32_t &position) {
  llvm::StringRef::iterator start = remainder.begin();
  remainder = remainder.ltrim();
  position += remainder.begin() - start;

  // Check to see if we've reached the end of our input string.
  if (remainder.empty())
    return Token(Token::eof, "", position);

  std::optional<llvm::StringRef> maybe_number = IsNumber(expr, remainder);
  if (maybe_number) {
    std::string number = (*maybe_number).str();
    auto token = Token(Token::numeric_constant, number, position);
    position += number.size();
    return token;
  } else {
    std::optional<llvm::StringRef> maybe_word = IsWord(remainder);
    if (maybe_word) {
      llvm::StringRef word = *maybe_word;
      Token::Kind kind = llvm::StringSwitch<Token::Kind>(word)
                            .Case("bool", Token::kw_bool)
                            .Case("char", Token::kw_char)
                            .Case("char16_t", Token::kw_char16_t)
                            .Case("char32_t", Token::kw_char32_t)
                            .Case("const", Token::kw_const)
                            .Case("double", Token::kw_double)
                            .Case("dynamic_cast", Token::kw_dynamic_cast)
                            .Case("false", Token::kw_false)
                            .Case("float", Token::kw_float)
                            .Case("int", Token::kw_int)
                            .Case("long", Token::kw_long)
                            .Case("namespace", Token::kw_namespace)
                            .Case("nullptr", Token::kw_nullptr)
                            .Case("reinterpret_cast", Token::kw_reinterpret_cast)
                            .Case("short", Token::kw_short)
                            .Case("signed", Token::kw_signed)
                            .Case("sizeof", Token::kw_sizeof)
                            .Case("static_cast", Token::kw_static_cast)
                            .Case("this", Token::kw_this)
                            .Case("true", Token::kw_true)
                            .Case("unsigned", Token::kw_unsigned)
                            .Case("void", Token::kw_void)
                            .Case("volatile", Token::kw_volatile)
                            .Case("wchar_t", Token::kw_wchar_t)
                            .Default(Token::identifier);
      auto token = Token(kind, word.str(), position);
      position += llvm::sys::unicode::columnWidthUTF8(word.str());
      return token;
    }
  }

  constexpr std::pair<Token::Kind, const char *> operators[] = {
    {Token::l_square, "["},
    {Token::r_square, "]"},
    {Token::l_paren, "("},
    {Token::r_paren, ")"},
    {Token::ampamp, "&&"},
    {Token::ampequal, "&="},
    {Token::amp, "&"},
    {Token::pipepipe, "||"},
    {Token::pipeequal, "|="},
    {Token::pipe, "|"},
    {Token::plusplus, "++"},
    {Token::plusequal, "+="},
    {Token::plus, "+"},
    {Token::minusminus, "--"},
    {Token::minusequal, "-="},
    {Token::arrow, "->"},
    {Token::minus, "-"},
    {Token::caretequal, "^="},
    {Token::caret, "^"},
    {Token::equalequal, "=="},
    {Token::equal, "="},
    {Token::exclaimequal, "!="},
    {Token::exclaim, "!"},
    {Token::percentequal, "%="},
    {Token::percent, "%"},
    {Token::slashequal, "/="},
    {Token::slash, "/"},
    {Token::starequal, "*="},
    {Token::star, "*"},
    {Token::coloncolon, "::"},
    {Token::colon, ":"},
    {Token::lesslessequal, "<<="},
    {Token::lessless, "<<"},
    {Token::lessequal, "<="},
    {Token::less, "<"},
    {Token::greatergreaterequal, ">>="},
    {Token::greatergreater, ">>"},
    {Token::greaterequal, ">="},
    {Token::greater, ">"},
    {Token::period, "."},
    {Token::question, "?"},
    {Token::comma, ","},
    {Token::tilde, "~"},
  };
  for (auto [kind, str] : operators) {
    if (remainder.consume_front(str)) {
      auto token = Token(kind, str, position);
      position += strlen(str);
      return token;
    }
  }

  // Unrecognized character(s) in string; unable to lex it.
  return llvm::createStringError("Unable to lex input string");
}

} // namespace lldb_private::dil
