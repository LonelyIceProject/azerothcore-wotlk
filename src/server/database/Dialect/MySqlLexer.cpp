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

#include "MySqlLexer.h"
#include <algorithm>
#include <array>

namespace
{
    bool IsSpace(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    }

    bool IsDigit(char c)
    {
        return c >= '0' && c <= '9';
    }

    bool IsHexDigit(char c)
    {
        return IsDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    bool IsIdentChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) || c == '_' || c == '$' || static_cast<unsigned char>(c) >= 0x80;
    }

    char ToLower(char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    int HexValue(char c)
    {
        if (IsDigit(c))
            return c - '0';
        return ToLower(c) - 'a' + 10;
    }

    // Offset just past a quoted run starting at 'pos' (the opening quote), or npos if unterminated.
    std::size_t SkipQuoted(std::string_view sql, std::size_t pos, bool backslashEscapes)
    {
        char const quote = sql[pos];
        std::size_t i = pos + 1;
        while (i < sql.size())
        {
            char c = sql[i];
            if (backslashEscapes && c == '\\')
            {
                i += 2;
                continue;
            }

            if (c == quote)
            {
                if (i + 1 < sql.size() && sql[i + 1] == quote)
                {
                    i += 2;
                    continue;
                }
                return i + 1;
            }
            ++i;
        }
        return std::string_view::npos;
    }

    bool IsLineCommentStart(std::string_view sql, std::size_t pos)
    {
        if (sql[pos] == '#')
            return true;
        return sql[pos] == '-' && pos + 1 < sql.size() && sql[pos + 1] == '-' &&
            (pos + 2 >= sql.size() || IsSpace(sql[pos + 2]) || static_cast<unsigned char>(sql[pos + 2]) < 0x20);
    }

    constexpr std::array<std::string_view, 11> MultiCharOperators =
    {
        "<=>", "->>", ":=", "<=", ">=", "<>", "!=", "||", "&&", "<<", ">>"
    };
}

bool MySqlToken::Is(std::string_view keyword) const
{
    if (type != MySqlTokenType::Identifier || text.size() != keyword.size())
        return false;

    for (std::size_t i = 0; i < text.size(); ++i)
        if (ToLower(text[i]) != ToLower(keyword[i]))
            return false;

    return true;
}

MySqlLexer::MySqlLexer(std::string_view sql, uint32 firstLine) : _sql(sql), _line(firstLine) { }

MySqlToken MySqlLexer::Next()
{
    MySqlToken token;
    token.offset = _pos;
    token.line = _line;

    if (_pos >= _sql.size())
    {
        token.type = MySqlTokenType::End;
        return token;
    }

    std::size_t const start = _pos;
    std::size_t end = start + 1;
    char const c = _sql[start];
    char const next = start + 1 < _sql.size() ? _sql[start + 1] : '\0';

    auto finishError = [&]()
    {
        token.type = MySqlTokenType::Error;
        end = _sql.size();
    };

    if (IsSpace(c))
    {
        token.type = MySqlTokenType::Whitespace;
        while (end < _sql.size() && IsSpace(_sql[end]))
            ++end;
    }
    else if (IsLineCommentStart(_sql, start))
    {
        token.type = MySqlTokenType::Comment;
        end = _sql.find('\n', start);
        if (end == std::string_view::npos)
            end = _sql.size();
    }
    else if (c == '/' && next == '*')
    {
        token.type = (start + 2 < _sql.size() && _sql[start + 2] == '!') ? MySqlTokenType::VersionComment : MySqlTokenType::Comment;
        end = _sql.find("*/", start + 2);
        if (end == std::string_view::npos)
            finishError();
        else
            end += 2;
    }
    else if (c == '\'' || c == '"')
    {
        token.type = MySqlTokenType::String;
        end = SkipQuoted(_sql, start, true);
        if (end == std::string_view::npos)
            finishError();
    }
    else if (c == '`')
    {
        token.type = MySqlTokenType::QuotedIdentifier;
        end = SkipQuoted(_sql, start, false);
        if (end == std::string_view::npos)
            finishError();
    }
    else if ((c == 'x' || c == 'X' || c == 'b' || c == 'B') && next == '\'')
    {
        token.type = (c == 'x' || c == 'X') ? MySqlTokenType::HexNumber : MySqlTokenType::BitString;
        end = _sql.find('\'', start + 2);
        if (end == std::string_view::npos)
            finishError();
        else
            ++end;
    }
    else if (c == '0' && (next == 'x' || next == 'b') && start + 2 < _sql.size() &&
        (next == 'x' ? IsHexDigit(_sql[start + 2]) : (_sql[start + 2] == '0' || _sql[start + 2] == '1')))
    {
        token.type = next == 'x' ? MySqlTokenType::HexNumber : MySqlTokenType::BitString;
        end = start + 2;
        while (end < _sql.size() && (next == 'x' ? IsHexDigit(_sql[end]) : (_sql[end] == '0' || _sql[end] == '1')))
            ++end;
        if (end < _sql.size() && IsIdentChar(_sql[end]))
        {
            token.type = MySqlTokenType::Identifier;
            while (end < _sql.size() && IsIdentChar(_sql[end]))
                ++end;
        }
    }
    else if (IsDigit(c) || (c == '.' && IsDigit(next) && (start == 0 || !(IsIdentChar(_sql[start - 1]) || _sql[start - 1] == '`' || _sql[start - 1] == ')'))))
    {
        token.type = MySqlTokenType::Number;
        end = start;
        while (end < _sql.size() && IsDigit(_sql[end]))
            ++end;
        if (end < _sql.size() && _sql[end] == '.')
        {
            ++end;
            while (end < _sql.size() && IsDigit(_sql[end]))
                ++end;
        }
        if (end < _sql.size() && (_sql[end] == 'e' || _sql[end] == 'E'))
        {
            std::size_t exp = end + 1;
            if (exp < _sql.size() && (_sql[exp] == '+' || _sql[exp] == '-'))
                ++exp;
            if (exp < _sql.size() && IsDigit(_sql[exp]))
            {
                end = exp;
                while (end < _sql.size() && IsDigit(_sql[end]))
                    ++end;
            }
        }
        if (end < _sql.size() && IsIdentChar(_sql[end]) && _sql.substr(start, end - start).find('.') == std::string_view::npos)
        {
            token.type = MySqlTokenType::Identifier;
            while (end < _sql.size() && IsIdentChar(_sql[end]))
                ++end;
        }
    }
    else if (IsIdentChar(c))
    {
        token.type = MySqlTokenType::Identifier;
        while (end < _sql.size() && IsIdentChar(_sql[end]))
            ++end;
    }
    else if (c == '@')
    {
        if (next == '@')
        {
            token.type = MySqlTokenType::SystemVariable;
            end = start + 2;
            while (end < _sql.size() && (IsIdentChar(_sql[end]) || (_sql[end] == '.' && end + 1 < _sql.size() && IsIdentChar(_sql[end + 1]))))
                ++end;
        }
        else if (next == '`' || next == '\'' || next == '"')
        {
            token.type = MySqlTokenType::Variable;
            end = SkipQuoted(_sql, start + 1, next != '`');
            if (end == std::string_view::npos)
                finishError();
        }
        else
        {
            token.type = MySqlTokenType::Variable;
            while (end < _sql.size() && (IsIdentChar(_sql[end]) || _sql[end] == '.'))
                ++end;
        }
    }
    else if (c == '?')
        token.type = MySqlTokenType::Parameter;
    else if (c == '(' || c == ')' || c == ',' || c == '.' || c == ';')
        token.type = MySqlTokenType::Punctuation;
    else
    {
        token.type = MySqlTokenType::Operator;
        std::string_view rest = _sql.substr(start);
        auto itr = std::find_if(MultiCharOperators.begin(), MultiCharOperators.end(), [rest](std::string_view op) { return rest.starts_with(op); });
        if (itr != MultiCharOperators.end())
            end = start + itr->size();
        else if (c == '-' && next == '>')
            end = start + 2;
        else if (std::string_view("=+-*/%&|^~!<>:").find(c) == std::string_view::npos)
            token.type = MySqlTokenType::Error;
    }

    token.text = _sql.substr(start, end - start);
    _line += static_cast<uint32>(std::count(token.text.begin(), token.text.end(), '\n'));
    _pos = end;
    return token;
}

MySqlToken MySqlLexer::NextSignificant()
{
    MySqlToken token = Next();
    while (token.IsTrivia())
        token = Next();
    return token;
}

MySqlToken MySqlLexer::PeekSignificant()
{
    std::size_t const pos = _pos;
    uint32 const line = _line;
    MySqlToken token = NextSignificant();
    _pos = pos;
    _line = line;
    return token;
}

std::vector<MySqlToken> MySqlTokenize(std::string_view sql, bool keepTrivia)
{
    std::vector<MySqlToken> tokens;
    MySqlLexer lexer(sql);
    for (MySqlToken token = keepTrivia ? lexer.Next() : lexer.NextSignificant(); token.type != MySqlTokenType::End;
        token = keepTrivia ? lexer.Next() : lexer.NextSignificant())
        tokens.push_back(token);
    return tokens;
}

std::string MySqlDecodeString(std::string_view literal)
{
    std::string result;
    if (literal.size() < 2)
        return result;

    char const quote = literal.front();
    std::string_view body = literal.substr(1, literal.size() - 2);
    result.reserve(body.size());

    for (std::size_t i = 0; i < body.size(); ++i)
    {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size())
        {
            char e = body[++i];
            switch (e)
            {
                case '0': result += '\0'; break;
                case 'b': result += '\b'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'Z': result += '\x1a'; break;
                case '%':
                case '_':
                    result += '\\';
                    result += e;
                    break;
                default: result += e; break;
            }
        }
        else if (c == quote && i + 1 < body.size() && body[i + 1] == quote)
        {
            result += c;
            ++i;
        }
        else
            result += c;
    }

    return result;
}

std::string MySqlUnquoteIdentifier(std::string_view identifier)
{
    if (identifier.size() < 2 || identifier.front() != '`' || identifier.back() != '`')
        return std::string(identifier);

    std::string result;
    std::string_view body = identifier.substr(1, identifier.size() - 2);
    result.reserve(body.size());
    for (std::size_t i = 0; i < body.size(); ++i)
    {
        result += body[i];
        if (body[i] == '`' && i + 1 < body.size() && body[i + 1] == '`')
            ++i;
    }
    return result;
}

std::string MySqlDecodeHex(std::string_view literal)
{
    std::string_view digits;
    if (literal.size() >= 2 && literal[0] == '0' && (literal[1] == 'x' || literal[1] == 'X'))
        digits = literal.substr(2);
    else if (literal.size() >= 3 && (literal[0] == 'x' || literal[0] == 'X') && literal[1] == '\'' && literal.back() == '\'')
        digits = literal.substr(2, literal.size() - 3);
    else
        return {};

    std::string result;
    result.reserve((digits.size() + 1) / 2);
    std::size_t i = 0;
    if (digits.size() % 2)
    {
        result += static_cast<char>(HexValue(digits[0]));
        i = 1;
    }
    for (; i + 1 < digits.size(); i += 2)
        result += static_cast<char>((HexValue(digits[i]) << 4) | HexValue(digits[i + 1]));
    return result;
}

std::string_view MySqlVersionCommentBody(std::string_view comment)
{
    if (!comment.starts_with("/*!") || !comment.ends_with("*/") || comment.size() < 5)
        return {};

    std::string_view body = comment.substr(3, comment.size() - 5);
    std::size_t i = 0;
    while (i < body.size() && IsDigit(body[i]))
        ++i;
    body.remove_prefix(i);
    while (!body.empty() && IsSpace(body.front()))
        body.remove_prefix(1);
    while (!body.empty() && IsSpace(body.back()))
        body.remove_suffix(1);
    return body;
}

struct MySqlStatementReader::Impl
{
    enum class State : uint8
    {
        Normal,
        Quoted,
        LineComment,
        BlockComment
    };

    std::istream& in;
    std::string buffer;
    std::size_t start = 0;
    std::size_t pos = 0;
    uint32 line = 1;
    uint32 index = 0;
    State state = State::Normal;
    char quote = 0;
    std::size_t stateStart = 0;
    bool eof = false;
    std::string error;

    explicit Impl(std::istream& stream) : in(stream) { }

    bool Fill()
    {
        if (eof)
            return false;

        if (start > 0)
        {
            buffer.erase(0, start);
            pos -= start;
            stateStart -= std::min(stateStart, start);
            start = 0;
        }

        std::size_t const old = buffer.size();
        buffer.resize(old + 256 * 1024);
        in.read(buffer.data() + old, static_cast<std::streamsize>(buffer.size() - old));
        std::size_t const got = static_cast<std::size_t>(in.gcount());
        buffer.resize(old + got);
        if (got == 0 || !in)
            eof = true;
        return got > 0;
    }

    bool Available(std::size_t count)
    {
        while (buffer.size() - pos < count)
            if (!Fill() && buffer.size() - pos < count)
                return false;
        return true;
    }

    bool Emit(std::string_view text, uint32 firstLine, MySqlStatementText& out)
    {
        MySqlLexer lexer(text, firstLine);
        MySqlToken first = lexer.NextSignificant();
        if (first.type == MySqlTokenType::End)
            return false;

        std::string_view sql = text.substr(first.offset);
        while (!sql.empty() && IsSpace(sql.back()))
            sql.remove_suffix(1);

        out.sql.assign(sql);
        out.line = first.line;
        out.index = ++index;
        return true;
    }

    bool Next(MySqlStatementText& out)
    {
        while (true)
        {
            if (!Available(1))
            {
                if (state == State::Quoted || state == State::BlockComment)
                {
                    if (error.empty())
                        error = "unterminated " + std::string(state == State::Quoted ? "quoted text" : "comment") + " starting at line " + std::to_string(LineAt(stateStart));
                    return false;
                }

                if (start >= buffer.size())
                    return false;

                std::string_view text(buffer.data() + start, buffer.size() - start);
                uint32 const firstLine = line;
                line += static_cast<uint32>(std::count(text.begin(), text.end(), '\n'));
                bool const emitted = Emit(text, firstLine, out);
                start = pos = buffer.size();
                state = State::Normal;
                if (emitted)
                    return true;
                continue;
            }

            char const c = buffer[pos];
            switch (state)
            {
                case State::Normal:
                    if (c == ';')
                    {
                        std::string_view text(buffer.data() + start, pos - start);
                        uint32 const firstLine = line;
                        line += static_cast<uint32>(std::count(text.begin(), text.end(), '\n'));
                        start = ++pos;
                        if (Emit(text, firstLine, out))
                            return true;
                        continue;
                    }
                    if (c == '\'' || c == '"' || c == '`')
                    {
                        state = State::Quoted;
                        quote = c;
                        stateStart = pos;
                    }
                    else if (c == '#')
                        state = State::LineComment;
                    else if (c == '-' || c == '/')
                    {
                        Available(3);
                        std::string_view rest(buffer.data() + pos, buffer.size() - pos);
                        if (c == '/' && rest.size() >= 2 && rest[1] == '*')
                        {
                            state = State::BlockComment;
                            stateStart = pos;
                            pos += 2;
                            continue;
                        }
                        if (c == '-' && IsLineCommentStart(rest, 0))
                            state = State::LineComment;
                    }
                    ++pos;
                    break;
                case State::Quoted:
                    if (c == '\\' && quote != '`')
                    {
                        Available(2);
                        pos = std::min(pos + 2, buffer.size());
                        continue;
                    }
                    if (c == quote)
                    {
                        Available(2);
                        if (pos + 1 < buffer.size() && buffer[pos + 1] == quote)
                        {
                            pos += 2;
                            continue;
                        }
                        state = State::Normal;
                    }
                    ++pos;
                    break;
                case State::LineComment:
                    if (c == '\n')
                        state = State::Normal;
                    ++pos;
                    break;
                case State::BlockComment:
                    if (c == '*')
                    {
                        Available(2);
                        if (pos + 1 < buffer.size() && buffer[pos + 1] == '/')
                        {
                            state = State::Normal;
                            pos += 2;
                            continue;
                        }
                    }
                    ++pos;
                    break;
            }
        }
    }

    uint32 LineAt(std::size_t offset) const
    {
        return line + static_cast<uint32>(std::count(buffer.begin() + start, buffer.begin() + offset, '\n'));
    }
};

MySqlStatementReader::MySqlStatementReader(std::istream& in) : _impl(std::make_unique<Impl>(in)) { }

MySqlStatementReader::~MySqlStatementReader() = default;

bool MySqlStatementReader::Next(MySqlStatementText& out)
{
    return _impl->Next(out);
}

bool MySqlStatementReader::HasError() const
{
    return !_impl->error.empty();
}

std::string const& MySqlStatementReader::GetError() const
{
    return _impl->error;
}
