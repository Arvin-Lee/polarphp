// This source file is part of the polarphp.org open source project
//
// Copyright (c) 2017 - 2019 polarphp software foundation
// Copyright (c) 2017 - 2019 zzu_softboy <zzu_softboy@163.com>
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://polarphp.org/LICENSE.txt for license information
// See https://polarphp.org/CONTRIBUTORS.txt for the list of polarphp project authors
//
// Created by polarboy on 2019/06/26.

#ifndef POLARPHP_PARSER_INTERNAL_YY_LEXER_EXTRAS_H
#define POLARPHP_PARSER_INTERNAL_YY_LEXER_EXTRAS_H

#include <cstddef>

namespace polar::parser {
class Lexer;
}

namespace polar::parser::internal {

size_t count_str_newline(const unsigned char *str, size_t length);
void handle_newlines(Lexer &lexer, const unsigned char *str, size_t length);
void handle_newline(Lexer &lexer, unsigned char c);

} // polar::parser::internal

#endif // POLARPHP_PARSER_INTERNAL_YY_LEXER_EXTRAS_H
