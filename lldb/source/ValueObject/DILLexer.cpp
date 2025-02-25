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
//#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/Support/UnicodeCharRanges.h"
#include <tuple>

namespace lldb_private::dil {

/*
const llvm::StringMap<Token::Kind> Keywords = {
    {"bool", Token::kw_bool},
    {"char", Token::kw_char},
    {"char16_t", Token::kw_char16_t},
    {"char32_t", Token::kw_char32_t},
    {"const", Token::kw_const},
    {"double", Token::kw_double},
    {"dynamic_cast", Token::kw_dynamic_cast},
    {"false", Token::kw_false},
    {"float", Token::kw_float},
    {"int", Token::kw_int},
    {"long", Token::kw_long},
    {"namespace", Token::kw_namespace},
    {"nullptr", Token::kw_nullptr},
    {"reinterpret_cast", Token::kw_reinterpret_cast},
    {"short", Token::kw_short},
    {"signed", Token::kw_signed},
    {"sizeof", Token::kw_sizeof},
    {"static_cast", Token::kw_static_cast},
    {"this", Token::kw_this},
    {"true", Token::kw_true},
    {"unsigned", Token::kw_unsigned},
    {"void", Token::kw_void},
    {"volatile", Token::kw_volatile},
    {"wchar_t", Token::kw_wchar_t}};
*/

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

static bool isValidIdentifierContinuationCodePoint(uint32_t c) {
  if (c < 0x80)
    return clang::isAsciiIdentifierContinue(c, /*dollar*/ true);

  // N1518: Recommendations for extended identifier characters for C and C++
  // Proposed Annex X.1: Ranges of characters allowed
  return c == 0x00A8 || c == 0x00AA || c == 0x00AD || c == 0x00AF ||
         (c >= 0x00B2 && c <= 0x00B5) || (c >= 0x00B7 && c <= 0x00BA) ||
         (c >= 0x00BC && c <= 0x00BE) || (c >= 0x00C0 && c <= 0x00D6) ||
         (c >= 0x00D8 && c <= 0x00F6) || (c >= 0x00F8 && c <= 0x00FF)

         || (c >= 0x0100 && c <= 0x167F) || (c >= 0x1681 && c <= 0x180D) ||
         (c >= 0x180F && c <= 0x1FFF)

         || (c >= 0x200B && c <= 0x200D) || (c >= 0x202A && c <= 0x202E) ||
         (c >= 0x203F && c <= 0x2040) || c == 0x2054 ||
         (c >= 0x2060 && c <= 0x206F)

         || (c >= 0x2070 && c <= 0x218F) || (c >= 0x2460 && c <= 0x24FF) ||
         (c >= 0x2776 && c <= 0x2793) || (c >= 0x2C00 && c <= 0x2DFF) ||
         (c >= 0x2E80 && c <= 0x2FFF)

         || (c >= 0x3004 && c <= 0x3007) || (c >= 0x3021 && c <= 0x302F) ||
         (c >= 0x3031 && c <= 0x303F)

         || (c >= 0x3040 && c <= 0xD7FF)

         || (c >= 0xF900 && c <= 0xFD3D) || (c >= 0xFD40 && c <= 0xFDCF) ||
         (c >= 0xFDF0 && c <= 0xFE44) || (c >= 0xFE47 && c <= 0xFFF8)

         || (c >= 0x10000 && c <= 0x1FFFD) || (c >= 0x20000 && c <= 0x2FFFD) ||
         (c >= 0x30000 && c <= 0x3FFFD) || (c >= 0x40000 && c <= 0x4FFFD) ||
         (c >= 0x50000 && c <= 0x5FFFD) || (c >= 0x60000 && c <= 0x6FFFD) ||
         (c >= 0x70000 && c <= 0x7FFFD) || (c >= 0x80000 && c <= 0x8FFFD) ||
         (c >= 0x90000 && c <= 0x9FFFD) || (c >= 0xA0000 && c <= 0xAFFFD) ||
         (c >= 0xB0000 && c <= 0xBFFFD) || (c >= 0xC0000 && c <= 0xCFFFD) ||
         (c >= 0xD0000 && c <= 0xDFFFD) || (c >= 0xE0000 && c <= 0xEFFFD);
}

static bool isValidIdentifierStartCodePoint(uint32_t c) {
  if (!isValidIdentifierContinuationCodePoint(c))
    return false;
  if (c < 0x80 && IsDigit(c))
    return false;

  // N1518: Recommendations for extended identifier characters for C and C++
  // Proposed Annex X.2: Ranges of characters disallowed initially
  if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1DC0 && c <= 0x1DFF) ||
      (c >= 0x20D0 && c <= 0x20FF) || (c >= 0xFE20 && c <= 0xFE2F))
    return false;

  return true;
}

static const llvm::sys::UnicodeCharRange UnicodeWhitespaceCharRanges[] = {
    {0x0085, 0x0085}, {0x00A0, 0x00A0}, {0x1680, 0x1680},
    {0x180E, 0x180E}, {0x2000, 0x200A}, {0x2028, 0x2029},
    {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000}};

static bool isUnicodeWhitespace(uint32_t Codepoint) {
  static const llvm::sys::UnicodeCharSet UnicodeWhitespaceChars(
      UnicodeWhitespaceCharRanges);
  return UnicodeWhitespaceChars.contains(Codepoint);
}

static std::tuple<llvm::ConversionResult, llvm::UTF32, uint32_t>
convertUTF8SequenceAndAdvance(llvm::StringRef::iterator &cur_pos,
                              llvm::StringRef::iterator end) {
  llvm::UTF32 CodePoint;
  uint32_t size = llvm::getNumBytesForUTF8(*cur_pos);
  llvm::ConversionResult Status = llvm::convertUTF8Sequence(
      (const llvm::UTF8 **)&cur_pos, (const llvm::UTF8 *)end, &CodePoint,
      llvm::strictConversion);
  return {Status, CodePoint, size};
}

static void SkipUnicodeWhitespaces(llvm::StringRef &remainder) {
  llvm::StringRef::iterator cur_pos = remainder.begin();
  uint32_t length = 0;
  while (true) {
    auto [Status, CodePoint, size] =
        convertUTF8SequenceAndAdvance(cur_pos, remainder.end());
    if (Status != llvm::conversionOK || !isUnicodeWhitespace(CodePoint))
      break;
    length += size;
  }
  remainder = remainder.drop_front(length);
}

static void SkipWhitespaces(llvm::StringRef &remainder) {
  llvm::StringRef::iterator cur_pos;
  do {
    cur_pos = remainder.begin();
    remainder = remainder.ltrim();
    SkipUnicodeWhitespaces(remainder);
  } while (remainder.begin() != cur_pos);
}

static std::optional<llvm::StringRef>
IsWord(llvm::StringRef expr, llvm::StringRef &remainder, uint32_t &utf_length) {
  llvm::StringRef::iterator cur_pos = remainder.begin();
  llvm::StringRef::iterator start = cur_pos;
  auto [Status, CodePoint, size] =
      convertUTF8SequenceAndAdvance(cur_pos, remainder.end());
  if (Status == llvm::conversionOK &&
      isValidIdentifierStartCodePoint(CodePoint)) {
    utf_length = 1;
    unsigned length = size;
    while (true) {
      auto [Status, CodePoint, size] =
          convertUTF8SequenceAndAdvance(cur_pos, remainder.end());
      if (Status != llvm::conversionOK ||
          !isValidIdentifierContinuationCodePoint(CodePoint))
        break;
      utf_length++;
      length += size;
    }

    remainder = remainder.drop_front(length);
    llvm::StringRef utf_token(start, length);
    return utf_token;
  }
  return std::nullopt;
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
  llvm::StringRef::iterator cur_pos = remainder.begin();
  SkipWhitespaces(remainder);
  position += remainder.begin() - cur_pos;

  // Check to see if we've reached the end of our input string.
  if (remainder.empty())
    return Token(Token::eof, "", position);

  cur_pos = remainder.begin();
  std::optional<llvm::StringRef> maybe_number = IsNumber(expr, remainder);
  if (maybe_number) {
    std::string number = (*maybe_number).str();
    auto token = Token(Token::numeric_constant, number, position);
    position += number.size();
    return token;
  } else {
    uint32_t utf_length = 0;
    std::optional<llvm::StringRef> maybe_word =
        IsWord(expr, remainder, utf_length);
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
      position += utf_length;
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
