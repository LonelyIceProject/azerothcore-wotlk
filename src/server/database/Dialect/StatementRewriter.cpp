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

#include "StatementRewriter.h"
#include "MySqlLexer.h"
#include <sqlite3.h>
#include <algorithm>
#include <array>
#include <optional>
#include <vector>

namespace
{
    constexpr std::size_t npos = std::string_view::npos;

    char ToLower(char c)
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    bool EqualsNoCase(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return ToLower(x) == ToLower(y); });
    }

    template <std::size_t N>
    bool InList(std::string_view word, std::array<std::string_view, N> const& list)
    {
        return std::any_of(list.begin(), list.end(), [word](std::string_view w) { return EqualsNoCase(word, w); });
    }

    bool IsSqliteKeyword(std::string_view word)
    {
        return sqlite3_keyword_check(word.data(), static_cast<int>(word.size())) != 0;
    }

    // Words that SQLite reserves (no fallback to identifier) but MySQL accepts as plain identifiers.
    constexpr std::array<std::string_view, 7> SqliteOnlyReserved =
    {
        "AUTOINCREMENT", "DEFERRABLE", "NOTHING", "TRANSACTION", "RETURNING", "NOTNULL", "FILTER"
    };

    constexpr std::array<std::string_view, 9> DroppedModifiers =
    {
        "LOW_PRIORITY", "HIGH_PRIORITY", "DELAYED", "SQL_NO_CACHE", "SQL_CACHE", "SQL_CALC_FOUND_ROWS",
        "SQL_SMALL_RESULT", "SQL_BIG_RESULT", "SQL_BUFFER_RESULT"
    };

    constexpr std::array<std::string_view, 8> CharsetIntroducers =
    {
        "_utf8", "_utf8mb4", "_utf8mb3", "_latin1", "_ascii", "_binary", "_ucs2", "_utf16"
    };

    constexpr std::array<std::string_view, 5> CurrentTimeWords =
    {
        "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME", "LOCALTIME", "LOCALTIMESTAMP"
    };

    constexpr std::array<std::string_view, 12> StatementTriggers =
    {
        "SET", "LOCK", "UNLOCK", "FLUSH", "START", "BEGIN", "COMMIT", "TRUNCATE", "DESC", "DESCRIBE", "EXPLAIN", "SHOW"
    };

    constexpr std::array<std::string_view, 17> WordTriggers =
    {
        "IGNORE", "DUPLICATE", "FORCE", "USE", "STRAIGHT_JOIN", "QUICK", "DUAL", "DIV", "MOD", "SEPARATOR",
        "CAST", "ISNULL", "COLLATE", "BINARY", "FOR", "SHARE", "N"
    };

    bool StringNeedsRewrite(std::string_view literal)
    {
        return literal.front() == '"' || literal.find('\\') != npos;
    }

    std::string BitStringToDecimal(std::string_view literal)
    {
        std::string_view digits = literal;
        if (digits.starts_with("0b"))
            digits.remove_prefix(2);
        else if (digits.size() >= 3)
            digits = digits.substr(2, digits.size() - 3);

        uint64 value = 0;
        for (char c : digits)
            value = (value << 1) | (c == '1' ? 1 : 0);
        return std::to_string(value);
    }

    struct Token
    {
        MySqlTokenType type = MySqlTokenType::End;
        std::string_view src;
        std::optional<std::string> out;

        [[nodiscard]] std::string_view Text() const { return out ? std::string_view(*out) : src; }
    };

    class Rewriter
    {
    public:
        explicit Rewriter(std::string_view sql)
        {
            MySqlLexer lexer(sql);
            int32 depth = 0;
            for (MySqlToken token = lexer.Next(); token.type != MySqlTokenType::End; token = lexer.Next())
            {
                Token& added = _tokens.emplace_back();
                added.type = token.type;
                added.src = token.text;
                if (token.type == MySqlTokenType::Comment || token.type == MySqlTokenType::VersionComment)
                {
                    added.type = MySqlTokenType::Whitespace;
                    added.out = " ";
                }

                if (added.type == MySqlTokenType::Whitespace)
                    continue;

                if (token.IsPunct(')'))
                    --depth;
                _sig.push_back(_tokens.size() - 1);
                _depth.push_back(depth);
                if (token.IsPunct('('))
                    ++depth;
            }
        }

        std::string Run()
        {
            if (_sig.empty())
                return {};

            if (std::optional<std::string> special = RewriteStatement())
                return *special;

            RewriteTokens();
            RewriteLikeEscapes();
            RewriteOnDuplicateKey();
            return Join();
        }

    private:
        std::vector<Token> _tokens;
        std::vector<std::size_t> _sig;
        std::vector<int32> _depth;

        std::size_t Count() const { return _sig.size(); }
        Token& Sig(std::size_t i) { return _tokens[_sig[i]]; }
        Token const& Sig(std::size_t i) const { return _tokens[_sig[i]]; }

        bool Is(std::size_t i, std::string_view keyword) const
        {
            return i < Count() && Sig(i).type == MySqlTokenType::Identifier && EqualsNoCase(Sig(i).src, keyword);
        }

        bool IsPunct(std::size_t i, char c) const
        {
            return i < Count() && Sig(i).type == MySqlTokenType::Punctuation && Sig(i).src.size() == 1 && Sig(i).src[0] == c;
        }

        bool IsName(std::size_t i) const
        {
            return i < Count() && (Sig(i).type == MySqlTokenType::Identifier || Sig(i).type == MySqlTokenType::QuotedIdentifier);
        }

        void Set(std::size_t i, std::string text)
        {
            Sig(i).out = std::move(text);
        }

        // Replaces an operator-like token, keeping it separated from its neighbours.
        void SetSpaced(std::size_t i, std::string_view text)
        {
            std::size_t const raw = _sig[i];
            std::string result;
            if (raw > 0 && _tokens[raw - 1].type != MySqlTokenType::Whitespace)
                result += ' ';
            result += text;
            if (raw + 1 < _tokens.size() && _tokens[raw + 1].type != MySqlTokenType::Whitespace)
                result += ' ';
            Set(i, std::move(result));
        }

        void Erase(std::size_t first, std::size_t last)
        {
            for (std::size_t raw = _sig[first]; raw <= _sig[last]; ++raw)
                _tokens[raw].out = std::string();
        }

        std::size_t MatchingParen(std::size_t open) const
        {
            for (std::size_t i = open + 1; i < Count(); ++i)
                if (IsPunct(i, ')') && _depth[i] == _depth[open])
                    return i;
            return npos;
        }

        std::string Join() const
        {
            std::string result;
            for (Token const& token : _tokens)
            {
                std::string_view text = token.Text();
                if (token.type == MySqlTokenType::Whitespace && (result.empty() || result.back() == ' ' || result.back() == '\n' || result.back() == '\t' || result.back() == '\r'))
                    continue;
                result += text;
            }
            while (!result.empty() && (result.back() == ' ' || result.back() == '\n' || result.back() == '\t' || result.back() == '\r'))
                result.pop_back();
            return result;
        }

        // Reads [schema.]table starting at i; returns the index after it.
        std::size_t ReadTableName(std::size_t i, std::string& schema, std::string& table) const
        {
            if (!IsName(i))
                return npos;
            table = MySqlUnquoteIdentifier(Sig(i).src);
            if (IsPunct(i + 1, '.') && IsName(i + 2))
            {
                schema = std::move(table);
                table = MySqlUnquoteIdentifier(Sig(i + 2).src);
                return i + 3;
            }
            return i + 1;
        }

        static std::string TableColumnsQuery(std::string const& schema, std::string const& table)
        {
            std::string result = "SELECT name FROM pragma_table_info(" + SqliteQuoteString(table);
            if (!schema.empty())
                result += ", " + SqliteQuoteString(schema);
            return result + ") ORDER BY cid";
        }

        std::optional<std::string> RewriteStatement()
        {
            if (Is(0, "SET"))
            {
                if (Count() > 1 && Sig(1).type != MySqlTokenType::Variable)
                    return std::string();
                return std::nullopt;
            }

            if (Is(0, "LOCK") || Is(0, "UNLOCK") || Is(0, "FLUSH"))
                return std::string();

            if ((Is(0, "START") && Is(1, "TRANSACTION")) || ((Is(0, "BEGIN") || Is(0, "COMMIT")) && (Count() == 1 || (Count() == 2 && Is(1, "WORK")))))
                return std::string();

            if (Is(0, "TRUNCATE"))
            {
                Set(0, "DELETE FROM");
                if (Is(1, "TABLE"))
                    Erase(1, 1);
                return std::nullopt;
            }

            std::string schema, table;
            if ((Is(0, "DESC") || Is(0, "DESCRIBE") || Is(0, "EXPLAIN")) && IsName(1) &&
                !Is(1, "SELECT") && !Is(1, "INSERT") && !Is(1, "UPDATE") && !Is(1, "DELETE") && !Is(1, "REPLACE") &&
                !Is(1, "WITH") && !Is(1, "FORMAT") && !Is(1, "ANALYZE") && !Is(1, "EXTENDED") && !Is(1, "TABLE"))
            {
                if (ReadTableName(1, schema, table) != npos)
                    return TableColumnsQuery(schema, table);
            }

            if (Is(0, "SHOW"))
            {
                std::size_t i = Is(1, "FULL") ? 2 : 1;
                if ((Is(i, "COLUMNS") || Is(i, "FIELDS")) && (Is(i + 1, "FROM") || Is(i + 1, "IN")))
                {
                    std::size_t next = ReadTableName(i + 2, schema, table);
                    if (next != npos)
                    {
                        if ((Is(next, "FROM") || Is(next, "IN")) && IsName(next + 1))
                            schema = MySqlUnquoteIdentifier(Sig(next + 1).src);
                        return TableColumnsQuery(schema, table);
                    }
                }
                else if (Is(i, "TABLES"))
                {
                    ++i;
                    if ((Is(i, "FROM") || Is(i, "IN")) && IsName(i + 1))
                    {
                        schema = MySqlUnquoteIdentifier(Sig(i + 1).src);
                        i += 2;
                    }

                    std::string result = "SELECT name FROM " + (schema.empty() ? std::string() : SqliteQuoteIdentifier(schema) + ".") +
                        "sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\'";
                    if (Is(i, "LIKE") && i + 1 < Count() && Sig(i + 1).type == MySqlTokenType::String)
                    {
                        std::string pattern = MySqlDecodeString(Sig(i + 1).src);
                        result += " AND name LIKE " + SqliteQuoteString(pattern);
                        if (pattern.find('\\') != npos)
                            result += " ESCAPE '\\'";
                    }
                    return result + " ORDER BY name";
                }
            }

            return std::nullopt;
        }

        std::size_t PreviousStatementWord(std::size_t i) const
        {
            while (i > 0)
            {
                --i;
                if (!InList(Sig(i).src, DroppedModifiers) && !Is(i, "QUICK"))
                    return i;
            }
            return npos;
        }

        bool IsOperandEnd(std::size_t i) const
        {
            if (i >= Count())
                return false;
            switch (Sig(i).type)
            {
                case MySqlTokenType::QuotedIdentifier:
                case MySqlTokenType::String:
                case MySqlTokenType::Number:
                case MySqlTokenType::HexNumber:
                case MySqlTokenType::BitString:
                case MySqlTokenType::Variable:
                case MySqlTokenType::Parameter:
                    return true;
                case MySqlTokenType::Punctuation:
                    return IsPunct(i, ')');
                case MySqlTokenType::Identifier:
                    return !IsSqliteKeyword(Sig(i).src) || Is(i, "NULL") || Is(i, "TRUE") || Is(i, "FALSE");
                default:
                    return false;
            }
        }

        void RewriteIndexHint(std::size_t i)
        {
            std::size_t j = i + 2;
            if (Is(j, "FOR"))
            {
                j += Is(j + 1, "JOIN") ? 2 : 3;
            }
            if (IsPunct(j, '('))
            {
                std::size_t close = MatchingParen(j);
                if (close != npos)
                    Erase(i, close);
            }
        }

        void RewriteTokens()
        {
            std::vector<std::string_view> parenOwners;
            for (std::size_t i = 0; i < Count(); ++i)
            {
                Token& token = Sig(i);
                if (token.type == MySqlTokenType::Punctuation)
                {
                    if (IsPunct(i, '('))
                        parenOwners.push_back(i > 0 && Sig(i - 1).type == MySqlTokenType::Identifier ? Sig(i - 1).src : std::string_view());
                    else if (IsPunct(i, ')') && !parenOwners.empty())
                        parenOwners.pop_back();
                    continue;
                }

                if (token.out)
                    continue;

                switch (token.type)
                {
                    case MySqlTokenType::QuotedIdentifier:
                        Set(i, SqliteQuoteIdentifier(MySqlUnquoteIdentifier(token.src)));
                        break;
                    case MySqlTokenType::String:
                        if (StringNeedsRewrite(token.src))
                            Set(i, SqliteQuoteString(MySqlDecodeString(token.src)));
                        break;
                    case MySqlTokenType::BitString:
                        Set(i, BitStringToDecimal(token.src));
                        break;
                    case MySqlTokenType::Operator:
                        if (token.src == "/")
                            SetSpaced(i, "* 1.0 /");
                        else if (token.src == "||")
                            SetSpaced(i, "OR");
                        else if (token.src == "&&")
                            SetSpaced(i, "AND");
                        else if (token.src == "<=>")
                            SetSpaced(i, "IS");
                        else if (token.src == "!")
                            SetSpaced(i, "NOT");
                        break;
                    case MySqlTokenType::Identifier:
                        RewriteIdentifier(i, parenOwners.empty() ? std::string_view() : parenOwners.back());
                        break;
                    default:
                        break;
                }
            }
        }

        void RewriteIdentifier(std::size_t i, std::string_view parenOwner)
        {
            std::string_view const word = Sig(i).src;
            if (i > 0 && IsPunct(i - 1, '.'))
            {
                if (IsSqliteKeyword(word))
                    Set(i, SqliteQuoteIdentifier(word));
                return;
            }

            if (i + 1 < Count() && Sig(i + 1).type == MySqlTokenType::String)
            {
                if (InList(word, CharsetIntroducers))
                {
                    if (EqualsNoCase(word, "_binary"))
                    {
                        Set(i, "CAST(");
                        std::string literal(Sig(i + 1).src);
                        Set(i + 1, (StringNeedsRewrite(literal) ? SqliteQuoteString(MySqlDecodeString(literal)) : literal) + " AS BLOB)");
                    }
                    else
                        Set(i, std::string());
                    return;
                }
                if (EqualsNoCase(word, "N") && _sig[i] + 1 == _sig[i + 1])
                {
                    Set(i, std::string());
                    return;
                }
            }

            if (Is(i, "IGNORE"))
            {
                if (Is(i + 1, "INDEX") || Is(i + 1, "KEY"))
                {
                    RewriteIndexHint(i);
                    return;
                }
                std::size_t prev = PreviousStatementWord(i);
                if (Is(prev, "INSERT") || Is(prev, "UPDATE"))
                    Set(i, "OR IGNORE");
                else if (Is(prev, "DELETE"))
                    Erase(i, i);
                return;
            }

            if ((Is(i, "USE") || Is(i, "FORCE")) && (Is(i + 1, "INDEX") || Is(i + 1, "KEY")))
            {
                RewriteIndexHint(i);
                return;
            }

            if (InList(word, DroppedModifiers) || (Is(i, "QUICK") && Is(PreviousStatementWord(i), "DELETE")))
            {
                Erase(i, i);
                return;
            }

            if (Is(i, "STRAIGHT_JOIN"))
            {
                if (Is(PreviousStatementWord(i), "SELECT") || Is(PreviousStatementWord(i), "DISTINCT"))
                    Erase(i, i);
                else
                    Set(i, "JOIN");
                return;
            }

            if (Is(i, "FOR") && Is(i + 1, "UPDATE") && _depth[i] == 0)
            {
                Erase(i, i + 1);
                return;
            }

            if (Is(i, "LOCK") && Is(i + 1, "IN") && Is(i + 2, "SHARE") && Is(i + 3, "MODE"))
            {
                Erase(i, i + 3);
                return;
            }

            if (Is(i, "FROM") && Is(i + 1, "DUAL"))
            {
                Erase(i, i + 1);
                return;
            }

            if ((Is(i, "DIV") || Is(i, "MOD")) && IsOperandEnd(i - 1) && i + 1 < Count() && !IsPunct(i + 1, ')') && !IsPunct(i + 1, ','))
            {
                SetSpaced(i, Is(i, "DIV") ? "/" : "%");
                return;
            }

            if (Is(i, "SEPARATOR") && EqualsNoCase(parenOwner, "GROUP_CONCAT"))
            {
                if (_sig[i] > 0 && _tokens[_sig[i] - 1].type == MySqlTokenType::Whitespace)
                    _tokens[_sig[i] - 1].out = std::string();
                if (i + 1 < Count() && Sig(i + 1).type == MySqlTokenType::String && MySqlDecodeString(Sig(i + 1).src) == ",")
                    Erase(i, i + 1);
                else
                    Set(i, ",");
                return;
            }

            if (Is(i, "AS") && (EqualsNoCase(parenOwner, "CAST") || EqualsNoCase(parenOwner, "CONVERT")))
            {
                RewriteCastType(i + 1);
                return;
            }

            if (Is(i, "ISNULL") && IsPunct(i + 1, '('))
            {
                std::size_t close = MatchingParen(i + 1);
                if (close != npos)
                {
                    Set(i, "(");
                    Set(close, ") IS NULL)");
                }
                return;
            }

            if (InList(word, CurrentTimeWords))
            {
                if (Is(i, "LOCALTIME") || Is(i, "LOCALTIMESTAMP"))
                    Set(i, "CURRENT_TIMESTAMP");
                if (IsPunct(i + 1, '(') && IsPunct(i + 2, ')'))
                    Erase(i + 1, i + 2);
                return;
            }

            if (Is(i, "COLLATE") && IsName(i + 1))
            {
                std::string collation = MySqlUnquoteIdentifier(Sig(i + 1).src);
                std::transform(collation.begin(), collation.end(), collation.begin(), ToLower);
                Set(i + 1, collation.ends_with("_ci") ? "NOCASE" : "BINARY");
                return;
            }

            if (Is(i, "BINARY") && !Is(i - 1, "AS") && !IsPunct(i + 1, '(') && i + 1 < Count())
            {
                Erase(i, i);
                return;
            }

            if (i > 0 && InList(word, SqliteOnlyReserved) && !IsPunct(i + 1, '('))
                Set(i, SqliteQuoteIdentifier(word));
        }

        void RewriteCastType(std::size_t i)
        {
            if (i >= Count())
                return;

            std::size_t last = i;
            std::string type;
            if (Is(i, "UNSIGNED") || Is(i, "SIGNED"))
            {
                type = "INTEGER";
                if (Is(i + 1, "INTEGER") || Is(i + 1, "INT"))
                    last = i + 1;
            }
            else if (Is(i, "CHAR") || Is(i, "NCHAR") || Is(i, "BINARY") || Is(i, "DECIMAL") || Is(i, "DATETIME") || Is(i, "DATE") || Is(i, "TIME"))
            {
                type = Is(i, "BINARY") ? "BLOB" : Is(i, "DECIMAL") ? "NUMERIC" : "TEXT";
                if (IsPunct(i + 1, '('))
                {
                    std::size_t close = MatchingParen(i + 1);
                    if (close != npos)
                        last = close;
                }
                if (Is(last + 1, "CHARACTER") && Is(last + 2, "SET") && IsName(last + 3))
                    last += 3;
                else if (Is(last + 1, "CHARSET") && IsName(last + 2))
                    last += 2;
            }
            else
                return;

            Erase(i, last);
            Set(i, std::move(type));
        }

        void RewriteLikeEscapes()
        {
            for (std::size_t i = 0; i + 1 < Count(); ++i)
            {
                if (!Is(i, "LIKE") || Sig(i + 1).type != MySqlTokenType::String || Is(i + 2, "ESCAPE"))
                    continue;

                if (MySqlDecodeString(Sig(i + 1).src).find('\\') == npos)
                    continue;

                std::string literal(Sig(i + 1).Text());
                Set(i + 1, literal + " ESCAPE '\\'");
            }
        }

        void RewriteOnDuplicateKey()
        {
            std::size_t on = npos;
            for (std::size_t i = 0; i + 3 < Count(); ++i)
            {
                if (_depth[i] == 0 && Is(i, "ON") && Is(i + 1, "DUPLICATE") && Is(i + 2, "KEY") && Is(i + 3, "UPDATE"))
                {
                    on = i;
                    break;
                }
            }
            if (on == npos)
                return;

            bool noop = true;
            std::size_t first = on + 4;
            for (std::size_t i = first; i <= Count(); ++i)
            {
                if (i < Count() && !(IsPunct(i, ',') && _depth[i] == _depth[on]))
                    continue;

                if (i - first != 3 || !IsName(first) || Sig(first + 1).src != "=" || !IsName(first + 2) ||
                    !EqualsNoCase(MySqlUnquoteIdentifier(Sig(first).src), MySqlUnquoteIdentifier(Sig(first + 2).src)))
                    noop = false;
                first = i + 1;
            }

            Set(on + 1, "CONFLICT");
            Set(on + 2, "DO");
            if (noop)
            {
                Set(on + 3, "NOTHING");
                if (on + 4 < Count())
                    Erase(on + 4, Count() - 1);
            }
            else
            {
                Set(on + 3, "UPDATE SET");
                for (std::size_t i = on + 4; i + 3 < Count(); ++i)
                {
                    if (!Is(i, "VALUES") || !IsPunct(i + 1, '(') || !IsName(i + 2) || !IsPunct(i + 3, ')'))
                        continue;

                    std::string column = MySqlUnquoteIdentifier(Sig(i + 2).src);
                    Erase(i, i + 3);
                    Set(i, "excluded." + (IsSqliteKeyword(column) || Sig(i + 2).type == MySqlTokenType::QuotedIdentifier ? SqliteQuoteIdentifier(column) : column));
                }
            }

            if (!Is(0, "INSERT"))
                return;

            std::size_t select = npos;
            for (std::size_t i = 1; i < on; ++i)
            {
                if (_depth[i] == 0 && Is(i, "SELECT"))
                {
                    select = i;
                    break;
                }
            }
            if (select == npos)
                return;

            std::size_t insertAt = on;
            for (std::size_t i = select + 1; i < on; ++i)
            {
                if (_depth[i] != 0)
                    continue;
                if (Is(i, "WHERE"))
                    return;
                if (Is(i, "GROUP") || Is(i, "HAVING") || Is(i, "ORDER") || Is(i, "LIMIT") || Is(i, "WINDOW"))
                {
                    insertAt = i;
                    break;
                }
            }
            Set(insertAt, "WHERE true " + std::string(Sig(insertAt).Text()));
        }
    };
}

std::string RewriteMySqlForSqlite(std::string_view sql)
{
    return Rewriter(sql).Run();
}

bool MySqlNeedsSqliteRewrite(std::string_view sql)
{
    MySqlLexer lexer(sql);
    bool first = true;
    bool afterDot = false;
    for (MySqlToken token = lexer.Next(); token.type != MySqlTokenType::End; token = lexer.Next())
    {
        switch (token.type)
        {
            case MySqlTokenType::Whitespace:
                continue;
            case MySqlTokenType::Comment:
            case MySqlTokenType::VersionComment:
            case MySqlTokenType::QuotedIdentifier:
            case MySqlTokenType::BitString:
                return true;
            case MySqlTokenType::String:
                if (StringNeedsRewrite(token.text))
                    return true;
                break;
            case MySqlTokenType::Operator:
                if (token.text == "/" || token.text == "||" || token.text == "&&" || token.text == "<=>" || token.text == "!")
                    return true;
                break;
            case MySqlTokenType::Identifier:
                if (first && InList(token.text, StatementTriggers))
                    return true;
                if (afterDot ? IsSqliteKeyword(token.text) :
                    (token.text.front() == '_' || InList(token.text, WordTriggers) || InList(token.text, DroppedModifiers) ||
                     InList(token.text, CurrentTimeWords) || InList(token.text, SqliteOnlyReserved)))
                    return true;
                break;
            default:
                break;
        }

        first = false;
        afterDot = token.IsPunct('.');
    }
    return first;
}

std::string SqliteQuoteIdentifier(std::string_view name)
{
    std::string result;
    result.reserve(name.size() + 2);
    result += '"';
    for (char c : name)
    {
        if (c == '"')
            result += '"';
        result += c;
    }
    result += '"';
    return result;
}

std::string SqliteQuoteString(std::string_view value)
{
    if (value.find('\0') != npos)
    {
        static constexpr char Digits[] = "0123456789ABCDEF";
        std::string result = "CAST(X'";
        for (unsigned char c : value)
        {
            result += Digits[c >> 4];
            result += Digits[c & 0xF];
        }
        return result + "' AS TEXT)";
    }

    std::string result;
    result.reserve(value.size() + 2);
    result += '\'';
    for (char c : value)
    {
        if (c == '\'')
            result += '\'';
        result += c;
    }
    result += '\'';
    return result;
}
