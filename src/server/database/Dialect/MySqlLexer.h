/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _MYSQLLEXER_H
#define _MYSQLLEXER_H

#include "Define.h"
#include <istream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

enum class MySqlTokenType : uint8
{
    End,
    Whitespace,
    Comment,                // -- ..., # ..., /* ... */
    VersionComment,         // /*!NNNNN ... */
    Identifier,             // bare word, keywords included
    QuotedIdentifier,       // `ident`
    String,                 // '...' or "..." (text keeps quotes and escapes as written)
    Number,                 // 12, 1.5, 1e3
    HexNumber,              // 0x1F, X'1F'
    BitString,              // b'01', 0b01
    Variable,               // @name, @`name`
    SystemVariable,         // @@name, @@session.name
    Parameter,              // ?
    Operator,               // = <> != <= >= := || && << >> + - * / % & | ^ ~ ! < >
    Punctuation,            // ( ) , . ;
    Error                   // unterminated string/comment/identifier
};

struct MySqlToken
{
    MySqlTokenType type = MySqlTokenType::End;
    std::string_view text;  // slice of the lexer input, exactly as written
    std::size_t offset = 0;
    uint32 line = 1;

    // Case-insensitive keyword match, only for Identifier tokens.
    [[nodiscard]] bool Is(std::string_view keyword) const;
    [[nodiscard]] bool IsPunct(char c) const { return type == MySqlTokenType::Punctuation && text.size() == 1 && text[0] == c; }
    [[nodiscard]] bool IsOperator(std::string_view op) const { return type == MySqlTokenType::Operator && text == op; }
    [[nodiscard]] bool IsTrivia() const { return type == MySqlTokenType::Whitespace || type == MySqlTokenType::Comment; }
};

class MySqlLexer
{
public:
    explicit MySqlLexer(std::string_view sql, uint32 firstLine = 1);

    MySqlToken Next();                  // every token, trivia included
    MySqlToken NextSignificant();       // skips Whitespace and Comment (VersionComment is returned)
    MySqlToken PeekSignificant();

    [[nodiscard]] bool AtEnd() const { return _pos >= _sql.size(); }
    [[nodiscard]] std::size_t Position() const { return _pos; }
    [[nodiscard]] uint32 Line() const { return _line; }

private:
    std::string_view _sql;
    std::size_t _pos = 0;
    uint32 _line = 1;
};

std::vector<MySqlToken> MySqlTokenize(std::string_view sql, bool keepTrivia = false);

// '...'/"..." literal (quotes included) -> raw bytes with MySQL escapes resolved (\0 \' \" \b \n \r \t \Z \\, doubled quotes).
// \% and \_ keep the backslash, as MySQL does.
std::string MySqlDecodeString(std::string_view literal);
// `a``b` -> a`b; bare identifiers are returned as is.
std::string MySqlUnquoteIdentifier(std::string_view identifier);
// 0x1F / X'1F' -> bytes
std::string MySqlDecodeHex(std::string_view literal);
// /*!40101 SET x */ -> "SET x"
std::string_view MySqlVersionCommentBody(std::string_view comment);

struct MySqlStatementText
{
    std::string sql;                    // without the terminating ';'
    uint32 line = 1;                    // line of the first significant character
    uint32 index = 0;                   // 1-based statement number in the script
};

// Splits a script into statements on ';' outside quotes/comments, reading the stream incrementally.
// Empty statements (only whitespace/comments) are skipped. DELIMITER is not interpreted.
class MySqlStatementReader
{
public:
    explicit MySqlStatementReader(std::istream& in);
    ~MySqlStatementReader();

    bool Next(MySqlStatementText& out);

    [[nodiscard]] bool HasError() const;
    [[nodiscard]] std::string const& GetError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

#endif
