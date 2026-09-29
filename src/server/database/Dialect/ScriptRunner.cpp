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

#include "ScriptRunner.h"
#include "MySqlLexer.h"
#include "SqlDialect.h"
#include <charconv>
#include <cmath>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace
{
    std::string Lower(std::string_view text)
    {
        std::string result(text);
        for (char& c : result)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return result;
    }

    std::string QuoteIdentifier(std::string_view name)
    {
        std::string result = "`";
        for (char c : name)
        {
            result += c;
            if (c == '`')
                result += '`';
        }
        return result + "`";
    }

    bool IsName(MySqlToken const& token)
    {
        return token.type == MySqlTokenType::Identifier || token.type == MySqlTokenType::QuotedIdentifier;
    }

    std::string NameText(MySqlToken const& token)
    {
        return MySqlUnquoteIdentifier(token.text);
    }

    std::string VariableName(std::string_view text)
    {
        std::string_view name = text.substr(1);
        if (!name.empty() && (name.front() == '`' || name.front() == '\'' || name.front() == '"'))
            return Lower(name.front() == '`' ? MySqlUnquoteIdentifier(name) : MySqlDecodeString(name));
        return Lower(name);
    }

    std::string LiteralText(ScriptValue const& value)
    {
        return std::visit([](auto const& v) -> std::string
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>)
                return "NULL";
            else if constexpr (std::is_same_v<T, int64>)
                return std::to_string(v);
            else if constexpr (std::is_same_v<T, double>)
            {
                std::string text = fmt::format("{}", v);
                if (text.find_first_of(".eEn") == std::string::npos)
                    text += ".0";
                return text;
            }
            else if constexpr (std::is_same_v<T, std::string>)
                return "'" + MySqlEscape(v) + "'";
            else
            {
                static char const digits[] = "0123456789ABCDEF";
                std::string text = "X'";
                for (uint8 b : v)
                {
                    text += digits[b >> 4];
                    text += digits[b & 0xF];
                }
                return text + "'";
            }
        }, value);
    }

    std::string Excerpt(std::string_view sql, std::size_t limit)
    {
        std::string result;
        for (char c : sql.substr(0, limit))
            result += (c == '\n' || c == '\r' || c == '\t') ? ' ' : c;
        if (sql.size() > limit)
            result += " ...";
        return result;
    }

    // Token helpers over the significant tokens of one statement.
    struct Statement
    {
        std::string_view sql;
        std::vector<MySqlToken> tokens;

        explicit Statement(std::string_view text) : sql(text)
        {
            for (MySqlToken const& token : MySqlTokenize(text))
                if (token.type != MySqlTokenType::VersionComment && !token.IsPunct(';'))
                    tokens.push_back(token);
        }

        std::size_t Size() const { return tokens.size(); }

        bool Is(std::size_t i, std::string_view keyword) const { return i < tokens.size() && tokens[i].Is(keyword); }
        bool IsPunct(std::size_t i, char c) const { return i < tokens.size() && tokens[i].IsPunct(c); }

        std::string_view Slice(std::size_t from, std::size_t to) const
        {
            if (from >= to || from >= tokens.size())
                return {};
            MySqlToken const& last = tokens[std::min(to, tokens.size()) - 1];
            return sql.substr(tokens[from].offset, last.offset + last.text.size() - tokens[from].offset);
        }

        std::string_view From(std::size_t from) const { return Slice(from, tokens.size()); }

        // First token at nesting depth 0 in [from, to) matching one of the keywords.
        std::size_t FindTopLevel(std::size_t from, std::initializer_list<std::string_view> keywords, std::size_t to = std::string::npos) const
        {
            int depth = 0;
            to = std::min(to, tokens.size());
            for (std::size_t i = from; i < to; ++i)
            {
                MySqlToken const& token = tokens[i];
                if (token.IsPunct('('))
                    ++depth;
                else if (token.IsPunct(')'))
                    --depth;
                else if (depth == 0)
                    for (std::string_view keyword : keywords)
                        if (token.Is(keyword))
                            return i;
            }
            return std::string::npos;
        }

        // Table name at i ([db.]name); advances i.
        bool ParseName(std::size_t& i, std::string& name) const
        {
            if (i >= tokens.size() || !IsName(tokens[i]))
                return false;
            name = NameText(tokens[i++]);
            while (IsPunct(i, '.') && i + 1 < tokens.size() && IsName(tokens[i + 1]))
            {
                name = NameText(tokens[i + 1]);
                i += 2;
            }
            return true;
        }
    };

    bool IsJoinKeyword(MySqlToken const& token)
    {
        return token.Is("JOIN") || token.Is("INNER") || token.Is("LEFT") || token.Is("RIGHT") || token.Is("CROSS") ||
            token.Is("STRAIGHT_JOIN") || token.Is("NATURAL") || token.Is("OUTER");
    }

    bool IsClauseKeyword(MySqlToken const& token)
    {
        return token.Is("WHERE") || token.Is("SET") || token.Is("ON") || token.Is("USING") || token.Is("ORDER") ||
            token.Is("LIMIT") || token.Is("GROUP") || token.Is("HAVING") || IsJoinKeyword(token) || token.IsPunct(',');
    }

    // [db.]table [[AS] alias]
    struct TableRef
    {
        std::string name;
        std::string alias;

        std::string const& Reference() const { return alias.empty() ? name : alias; }
    };

    bool ParseTableRef(Statement const& s, std::size_t& i, TableRef& ref)
    {
        if (!s.ParseName(i, ref.name))
            return false;
        if (s.Is(i, "AS"))
            ++i;
        if (i < s.Size() && IsName(s.tokens[i]) && !IsClauseKeyword(s.tokens[i]))
            ref.alias = NameText(s.tokens[i++]);
        return true;
    }
}

std::string ScriptError::ToString() const
{
    std::string text = source;
    if (line)
        text += ":" + std::to_string(line);
    if (statement)
        text += ": statement " + std::to_string(statement);
    text += ": " + message;
    if (!sql.empty())
        text += "\n    > " + Excerpt(sql, 300);
    return text;
}

struct ScriptRunner::Impl
{
    IScriptTarget& target;
    ScriptRunnerOptions options;
    SqlDialect const& dialect;
    ScriptError error;
    uint64 executed = 0;
    std::string source;
    MySqlStatementText const* current = nullptr;
    std::unordered_map<std::string, ScriptValue> variables;
    std::unordered_set<std::string> truncated;

    Impl(IScriptTarget& target_, ScriptRunnerOptions const& options_) : target(target_), options(options_), dialect(GetDialect(target_.Backend())) { }

    bool Fail(std::string message)
    {
        error = {};
        error.source = source;
        if (current)
        {
            error.statement = current->index;
            error.line = current->line;
            error.sql = current->sql.size() > 4096 ? current->sql.substr(0, 4096) : current->sql;
        }
        error.message = std::move(message);
        return false;
    }

    bool Unsupported(std::string_view what)
    {
        return Fail(std::string(what) + ": no translation, add an override file");
    }

    bool Run(std::istream& in, std::string_view sourceName)
    {
        source = std::string(sourceName);
        error = {};
        variables.clear();
        current = nullptr;

        DbError err;
        if (options.disableForeignKeys && !target.SetForeignKeys(false, err))
            return Fail(err.message);

        if (options.transaction && !target.Begin(err))
        {
            Fail(err.message);
            if (options.disableForeignKeys)
                target.SetForeignKeys(true, err);
            return false;
        }

        bool ok = true;
        MySqlStatementReader reader(in);
        MySqlStatementText statement;
        while (ok && reader.Next(statement))
        {
            current = &statement;
            ok = target.RunsMySqlAsIs() ? ExecuteAsIs(statement.sql) : Execute(statement.sql);
            if (ok)
                ++executed;
        }
        current = nullptr;

        if (ok && reader.HasError())
            ok = Fail(reader.GetError());
        if (ok && options.disableForeignKeys && !target.CheckForeignKeys(err))
            ok = Fail(err.message);
        if (ok && options.transaction && !target.Commit(err))
            ok = Fail(err.message);
        if (!ok && options.transaction)
            target.Rollback();
        if (options.disableForeignKeys)
        {
            DbError restore;
            if (!target.SetForeignKeys(true, restore) && ok)
                ok = Fail(restore.message);
        }
        return ok;
    }

    bool ExecuteAsIs(std::string_view sql)
    {
        DbError err;
        return target.Exec(sql, err) || Fail(err.message);
    }

    bool Execute(std::string_view sql)
    {
        MySqlLexer lexer(sql);
        MySqlToken head[3];
        std::size_t count = 0;
        while (count < 3)
        {
            MySqlToken token = lexer.NextSignificant();
            if (token.type == MySqlTokenType::End || token.IsPunct(';'))
                break;
            if (token.type == MySqlTokenType::Error)
                return Fail("malformed SQL near '" + Excerpt(token.text, 40) + "'");
            if (token.type != MySqlTokenType::VersionComment)
                head[count++] = token;
        }

        if (!count)
            return true;

        MySqlToken const& first = head[0];
        auto second = [&](std::string_view keyword) { return count > 1 && head[1].Is(keyword); };

        if (first.Is("INSERT") || first.Is("REPLACE"))
            return ExecuteInsert(sql);
        if (first.Is("SET"))
            return ExecuteSet(sql);
        if (first.Is("LOCK") || first.Is("UNLOCK") || first.Is("START") || first.Is("BEGIN") || first.Is("COMMIT") || first.Is("ROLLBACK") ||
            first.Is("DEALLOCATE") || first.Is("FLUSH") || first.Is("ANALYZE") || first.Is("OPTIMIZE"))
            return true;
        if (first.Is("DELIMITER") || first.Is("PREPARE") || first.Is("EXECUTE") || first.Is("CALL") || first.Is("USE") || first.Is("HANDLER") || first.Is("LOAD"))
            return Unsupported(Lower(first.text));
        if (first.Is("CREATE"))
        {
            if (second("TABLE") || second("TEMPORARY"))
                return ExecuteCreateTable(sql);
            if (second("INDEX") || second("UNIQUE") || second("FULLTEXT") || second("SPATIAL"))
                return ExecuteIndex(sql);
            return Unsupported("CREATE " + (count > 1 ? Lower(head[1].text) : std::string()));
        }
        if (first.Is("DROP"))
        {
            if (second("TABLE") || second("TEMPORARY"))
                return ExecuteDropTable(sql);
            if (second("INDEX"))
                return ExecuteIndex(sql);
            return Unsupported("DROP " + (count > 1 ? Lower(head[1].text) : std::string()));
        }
        if (first.Is("ALTER"))
        {
            if (second("TABLE") || second("ONLINE") || second("IGNORE"))
                return ExecuteAlterTable(sql);
            return Unsupported("ALTER " + (count > 1 ? Lower(head[1].text) : std::string()));
        }
        if (first.Is("RENAME"))
            return ExecuteRenameTable(sql);
        if (first.Is("DELETE"))
            return ExecuteDelete(sql);
        if (first.Is("UPDATE"))
            return ExecuteUpdate(sql);

        return ExecuteGeneric(sql);
    }

    bool CheckSupported(Statement const& s)
    {
        for (MySqlToken const& token : s.tokens)
        {
            if (token.type == MySqlTokenType::SystemVariable)
                return Fail("system variable " + std::string(token.text) + " is not supported");
            if (IsName(token) && IdentifierEquals(NameText(token), "information_schema"))
                return Unsupported("INFORMATION_SCHEMA");
            if (token.IsOperator(":="))
                return Unsupported("variable assignment inside a statement");
        }
        return true;
    }

    bool SubstituteVariables(std::string_view sql, std::string& out)
    {
        if (sql.find('@') == std::string_view::npos)
        {
            out.assign(sql);
            return true;
        }

        out.clear();
        out.reserve(sql.size());
        MySqlLexer lexer(sql);
        for (MySqlToken token = lexer.Next(); token.type != MySqlTokenType::End; token = lexer.Next())
        {
            if (token.type == MySqlTokenType::Variable)
            {
                auto itr = variables.find(VariableName(token.text));
                out += "(" + (itr != variables.end() ? LiteralText(itr->second) : std::string("NULL")) + ")";
            }
            else if (token.type == MySqlTokenType::SystemVariable)
                return Fail("system variable " + std::string(token.text) + " is not supported");
            else
                out += token.text;
        }
        return true;
    }

    bool ExecuteText(std::string_view mysqlSql)
    {
        std::string sql;
        if (!SubstituteVariables(mysqlSql, sql))
            return false;

        std::string const translated = dialect.Translate(sql, target.Schema());
        DbError err;
        if (target.Exec(translated, err))
            return true;

        std::string message = err.message;
        if (translated != sql)
            message += " [translated: " + Excerpt(translated, 300) + "]";
        return Fail(message);
    }

    bool ExecuteGeneric(std::string_view sql)
    {
        Statement s(sql);
        if (!CheckSupported(s))
            return false;
        return ExecuteText(sql);
    }

    bool ExecuteSet(std::string_view sql)
    {
        Statement s(sql);
        std::size_t i = 1;
        if (s.Is(i, "SESSION") || s.Is(i, "GLOBAL") || s.Is(i, "LOCAL") || s.Is(i, "PERSIST"))
            return true;

        while (i < s.Size())
        {
            MySqlToken const& variable = s.tokens[i];
            std::size_t end = i + 1;
            int depth = 0;
            for (; end < s.Size(); ++end)
            {
                MySqlToken const& token = s.tokens[end];
                if (token.IsPunct('('))
                    ++depth;
                else if (token.IsPunct(')'))
                    --depth;
                else if (depth == 0 && token.IsPunct(','))
                    break;
            }

            if (variable.type == MySqlTokenType::Variable)
            {
                if (i + 2 > end || !(s.tokens[i + 1].IsOperator("=") || s.tokens[i + 1].IsOperator(":=")))
                    return Fail("malformed variable assignment");

                if (!CheckSupported(Statement(s.Slice(i + 2, end))))
                    return false;

                std::string text;
                if (!SubstituteVariables(s.Slice(i + 2, end), text))
                    return false;

                ScriptValue value;
                DbError err;
                if (!target.Scalar(dialect.Translate("SELECT " + text, target.Schema()), value, err))
                    return Fail(err.message);
                variables[VariableName(variable.text)] = std::move(value);
            }

            i = end + 1;
        }
        return true;
    }

    bool Truncate(std::string const& table)
    {
        if (!options.truncateOnFirstInsert || !truncated.insert(Lower(table)).second)
            return true;
        return ExecuteText("DELETE FROM " + QuoteIdentifier(table));
    }

    enum class LiteralKind : uint8
    {
        Null,
        Integer,
        Real,
        String,
        Binary,
        Hex,
        Bits,
        Variable
    };

    // Reads one literal value; false means the value is not a plain literal (use the generic path).
    static bool ReadLiteral(MySqlLexer& lexer, MySqlToken& token, LiteralKind& kind, std::string& text)
    {
        bool negative = false;
        while (token.IsOperator("-") || token.IsOperator("+"))
        {
            negative ^= token.IsOperator("-");
            token = lexer.NextSignificant();
        }

        switch (token.type)
        {
            case MySqlTokenType::Number:
                text = negative ? "-" + std::string(token.text) : std::string(token.text);
                kind = token.text.find_first_of(".eE") == std::string_view::npos ? LiteralKind::Integer : LiteralKind::Real;
                return true;
            case MySqlTokenType::String:
                if (negative)
                    return false;
                text = MySqlDecodeString(token.text);
                kind = LiteralKind::String;
                return true;
            case MySqlTokenType::HexNumber:
                if (negative)
                    return false;
                text = MySqlDecodeHex(token.text);
                kind = LiteralKind::Hex;
                return true;
            case MySqlTokenType::BitString:
                if (negative)
                    return false;
                text = std::string(token.text);
                kind = LiteralKind::Bits;
                return true;
            case MySqlTokenType::Variable:
                if (negative)
                    return false;
                text = VariableName(token.text);
                kind = LiteralKind::Variable;
                return true;
            case MySqlTokenType::Identifier:
                if (negative)
                    return false;
                if (token.Is("NULL"))
                {
                    kind = LiteralKind::Null;
                    return true;
                }
                if (token.Is("TRUE") || token.Is("FALSE"))
                {
                    text = token.Is("TRUE") ? "1" : "0";
                    kind = LiteralKind::Integer;
                    return true;
                }
                if (token.text.size() > 1 && token.text.front() == '_')
                {
                    bool const binary = token.Is("_binary");
                    token = lexer.NextSignificant();
                    if (token.type == MySqlTokenType::String)
                        text = MySqlDecodeString(token.text);
                    else if (token.type == MySqlTokenType::HexNumber)
                        text = MySqlDecodeHex(token.text);
                    else
                        return false;
                    kind = binary ? LiteralKind::Binary : LiteralKind::String;
                    return true;
                }
                return false;
            default:
                return false;
        }
    }

    static ScriptValue IntegerValue(std::string_view text, ColumnAffinity affinity)
    {
        if (affinity == ColumnAffinity::Text || affinity == ColumnAffinity::DateTime)
            return std::string(text);

        int64 value = 0;
        auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (ec == std::errc() && ptr == text.data() + text.size())
            return value;

        uint64 unsignedValue = 0;
        auto [uptr, uec] = std::from_chars(text.data(), text.data() + text.size(), unsignedValue);
        if (uec == std::errc() && uptr == text.data() + text.size())
            return int64(unsignedValue);

        return std::strtod(std::string(text).c_str(), nullptr);
    }

    static ScriptValue RealValue(std::string_view text, ColumnAffinity affinity)
    {
        if (affinity == ColumnAffinity::Text || affinity == ColumnAffinity::DateTime)
            return std::string(text);

        double value = std::strtod(std::string(text).c_str(), nullptr);
        if (affinity == ColumnAffinity::Integer && std::isfinite(value) && std::fabs(value) < 9.2e18)
            return int64(std::llround(value));
        return value;
    }

    static ScriptValue BytesValue(std::string&& bytes, ColumnAffinity affinity, bool numericHex)
    {
        if (affinity == ColumnAffinity::Blob)
            return std::vector<uint8>(bytes.begin(), bytes.end());

        if (numericHex && (affinity == ColumnAffinity::Integer || affinity == ColumnAffinity::Real || affinity == ColumnAffinity::Decimal) && bytes.size() <= 8)
        {
            uint64 value = 0;
            for (char c : bytes)
                value = (value << 8) | uint8(c);
            return int64(value);
        }

        return std::move(bytes);
    }

    static ScriptValue CoerceVariable(ScriptValue const& value, ColumnAffinity affinity)
    {
        if (affinity == ColumnAffinity::Blob)
            if (std::string const* text = std::get_if<std::string>(&value))
                return std::vector<uint8>(text->begin(), text->end());
        return value;
    }

    bool ExecuteInsert(std::string_view sql)
    {
        MySqlLexer lexer(sql);
        auto next = [&lexer]()
        {
            MySqlToken token = lexer.NextSignificant();
            while (token.type == MySqlTokenType::VersionComment)
                token = lexer.NextSignificant();
            return token;
        };

        MySqlToken token = next();
        BulkInsertMode mode = token.Is("REPLACE") ? BulkInsertMode::Replace : BulkInsertMode::Insert;
        token = next();
        while (token.Is("LOW_PRIORITY") || token.Is("DELAYED") || token.Is("HIGH_PRIORITY") || token.Is("IGNORE"))
        {
            if (token.Is("IGNORE") && mode == BulkInsertMode::Insert)
                mode = BulkInsertMode::InsertIgnore;
            token = next();
        }
        if (token.Is("INTO"))
            token = next();

        if (!IsName(token))
            return ExecuteGeneric(sql);
        std::string table = NameText(token);
        token = next();
        while (token.IsPunct('.'))
        {
            token = next();
            if (!IsName(token))
                return ExecuteGeneric(sql);
            table = NameText(token);
            token = next();
        }

        if (!Truncate(table))
            return false;

        std::vector<std::string> columns;
        if (token.IsPunct('('))
        {
            do
            {
                token = next();
                if (!IsName(token))
                    return ExecuteGeneric(sql);
                columns.push_back(NameText(token));
                token = next();
            } while (token.IsPunct(','));
            if (!token.IsPunct(')'))
                return ExecuteGeneric(sql);
            token = next();
        }

        if (!token.Is("VALUES") && !token.Is("VALUE"))
            return ExecuteGeneric(sql);

        TableModel model;
        if (!target.LoadTable(table, model))
            return ExecuteGeneric(sql);

        std::vector<ColumnAffinity> affinities;
        if (columns.empty())
        {
            for (ColumnModel const& column : model.columns)
                affinities.push_back(column.affinity);
        }
        else
        {
            for (std::string const& name : columns)
            {
                ColumnModel const* column = model.FindColumn(name);
                if (!column)
                    return ExecuteGeneric(sql);
                affinities.push_back(column->affinity);
            }
        }

        std::vector<ScriptRow> rows;
        std::string text;
        while (true)
        {
            token = next();
            if (!token.IsPunct('('))
                return ExecuteGeneric(sql);

            ScriptRow row;
            row.reserve(affinities.size());
            do
            {
                token = next();
                bool parenthesized = false;
                if (token.IsPunct('('))
                {
                    parenthesized = true;
                    token = next();
                }

                LiteralKind kind = LiteralKind::Null;
                if (!ReadLiteral(lexer, token, kind, text))
                    return ExecuteGeneric(sql);

                if (parenthesized)
                {
                    token = next();
                    if (!token.IsPunct(')'))
                        return ExecuteGeneric(sql);
                }

                if (row.size() >= affinities.size())
                    return Fail("Column count doesn't match value count at row " + std::to_string(rows.size() + 1));

                ColumnAffinity const affinity = affinities[row.size()];
                switch (kind)
                {
                    case LiteralKind::Null:
                        row.emplace_back(nullptr);
                        break;
                    case LiteralKind::Integer:
                        row.push_back(IntegerValue(text, affinity));
                        break;
                    case LiteralKind::Real:
                        row.push_back(RealValue(text, affinity));
                        break;
                    case LiteralKind::String:
                        row.push_back(BytesValue(std::move(text), affinity, false));
                        break;
                    case LiteralKind::Binary:
                        row.push_back(BytesValue(std::move(text), affinity == ColumnAffinity::Text ? ColumnAffinity::Text : ColumnAffinity::Blob, false));
                        break;
                    case LiteralKind::Hex:
                        row.push_back(BytesValue(std::move(text), affinity, true));
                        break;
                    case LiteralKind::Bits:
                    {
                        std::string_view bits = text;
                        bits = bits.front() == '0' ? bits.substr(2) : bits.substr(2, bits.size() - 3);
                        uint64 value = 0;
                        for (char c : bits)
                            value = (value << 1) | (c == '1' ? 1 : 0);
                        row.emplace_back(int64(value));
                        break;
                    }
                    case LiteralKind::Variable:
                    {
                        auto itr = variables.find(text);
                        row.push_back(itr != variables.end() ? CoerceVariable(itr->second, affinity) : ScriptValue(nullptr));
                        break;
                    }
                }

                token = next();
            } while (token.IsPunct(','));

            if (!token.IsPunct(')'))
                return ExecuteGeneric(sql);
            if (row.size() != affinities.size())
                return Fail("Column count doesn't match value count at row " + std::to_string(rows.size() + 1));
            rows.push_back(std::move(row));

            token = next();
            if (token.IsPunct(','))
                continue;
            if (token.type == MySqlTokenType::End || token.IsPunct(';'))
                break;
            return ExecuteGeneric(sql);
        }

        DbError err;
        if (!target.BulkInsert(table, columns, rows, mode, err))
            return Fail(err.message);
        return true;
    }

    bool ExecuteCreateTable(std::string_view sql)
    {
        Statement s(sql);
        std::size_t i = 1;
        if (s.Is(i, "TEMPORARY"))
            return Unsupported("CREATE TEMPORARY TABLE");
        ++i;
        bool ifNotExists = false;
        if (s.Is(i, "IF"))
        {
            ifNotExists = true;
            i += 3;
        }

        std::string name;
        if (!s.ParseName(i, name))
            return Fail("expected table name");

        std::size_t like = s.IsPunct(i, '(') ? i + 1 : i;
        if (s.Is(like, "LIKE"))
        {
            std::size_t j = like + 1;
            std::string sourceTable;
            if (!s.ParseName(j, sourceTable))
                return Fail("expected table name after LIKE");

            TableModel model;
            if (!target.LoadTable(sourceTable, model))
                return Fail("Table '" + sourceTable + "' doesn't exist");
            model.name = name;
            model.autoIncrement.reset();

            DbError err;
            if (!target.CreateTable(model, ifNotExists, err))
                return Fail(err.message);
            return true;
        }

        if (s.FindTopLevel(i, { "SELECT" }) != std::string::npos || s.Is(i, "AS"))
            return Unsupported("CREATE TABLE ... SELECT");

        TableModel model;
        std::string parseError;
        if (!ParseMySqlCreateTable(sql, model, ifNotExists, parseError))
            return Fail(parseError);

        DbError err;
        if (!target.CreateTable(model, ifNotExists, err))
            return Fail(err.message);
        return true;
    }

    bool ExecuteDropTable(std::string_view sql)
    {
        Statement s(sql);
        std::size_t i = 1;
        if (s.Is(i, "TEMPORARY"))
            ++i;
        ++i;
        bool ifExists = false;
        if (s.Is(i, "IF") && s.Is(i + 1, "EXISTS"))
        {
            ifExists = true;
            i += 2;
        }

        do
        {
            std::string name;
            if (!s.ParseName(i, name))
                return Fail("expected table name");
            DbError err;
            if (!target.DropTable(name, ifExists, err))
                return Fail(err.message);
        } while (s.IsPunct(i++, ','));
        return true;
    }

    bool ApplyAlter(AlterTableModel const& alter)
    {
        DbError err;
        if (!target.AlterTable(alter, err))
            return Fail(err.message);
        return true;
    }

    bool ExecuteAlterTable(std::string_view sql)
    {
        AlterTableModel alter;
        std::string parseError;
        if (!ParseMySqlAlterTable(sql, alter, parseError))
            return Fail(parseError);
        return ApplyAlter(alter);
    }

    bool ExecuteIndex(std::string_view sql)
    {
        AlterTableModel alter;
        std::string parseError;
        if (!ParseMySqlIndexStatement(sql, alter, parseError))
            return Fail(parseError);
        return ApplyAlter(alter);
    }

    bool ExecuteRenameTable(std::string_view sql)
    {
        Statement s(sql);
        std::size_t i = 1;
        if (!s.Is(i++, "TABLE"))
            return Unsupported("RENAME");

        do
        {
            AlterTableModel alter;
            AlterOp op;
            op.kind = AlterOpKind::RenameTable;
            if (!s.ParseName(i, alter.table) || !s.Is(i++, "TO") || !s.ParseName(i, op.newName))
                return Fail("malformed RENAME TABLE");
            alter.ops.push_back(std::move(op));
            if (!ApplyAlter(alter))
                return false;
        } while (s.IsPunct(i++, ','));
        return true;
    }

    // Top-level ORDER BY / LIMIT of a single-table UPDATE or DELETE: dropped for the version tables, an error otherwise.
    bool StripLimit(Statement const& s, std::string const& table, std::size_t from, std::string& sql)
    {
        std::size_t const order = s.FindTopLevel(from, { "ORDER" });
        std::size_t const limit = s.FindTopLevel(from, { "LIMIT" });
        std::size_t const cut = std::min(order, limit);
        if (cut == std::string::npos)
        {
            sql.assign(s.sql);
            return true;
        }

        if (limit != std::string::npos && !Lower(table).starts_with("version"))
            return Unsupported("UPDATE/DELETE with LIMIT");

        sql.assign(s.Slice(0, cut));
        return true;
    }

    static std::string KeyColumns(IndexModel const& pk, std::string const& qualifier)
    {
        std::string text;
        for (std::size_t i = 0; i < pk.columns.size(); ++i)
        {
            if (i)
                text += ", ";
            if (!qualifier.empty())
                text += QuoteIdentifier(qualifier) + ".";
            text += QuoteIdentifier(pk.columns[i]);
        }
        return text;
    }

    bool ExecuteDelete(std::string_view sql)
    {
        Statement s(sql);
        if (!CheckSupported(s))
            return false;

        std::size_t i = 1;
        while (s.Is(i, "LOW_PRIORITY") || s.Is(i, "QUICK") || s.Is(i, "IGNORE"))
            ++i;

        std::vector<std::string> targets;
        std::size_t refsStart = 0;
        if (s.Is(i, "FROM"))
        {
            ++i;
            std::size_t const using_ = s.FindTopLevel(i, { "USING" });
            if (using_ == std::string::npos)
            {
                std::size_t j = i;
                TableRef ref;
                if (!ParseTableRef(s, j, ref))
                    return Fail("expected table name");

                if (j < s.Size() && (IsJoinKeyword(s.tokens[j]) || s.tokens[j].IsPunct(',')))
                    return Unsupported("DELETE with a join");

                std::string text;
                if (!StripLimit(s, ref.name, j, text))
                    return false;
                return ExecuteText(text);
            }

            for (std::size_t j = i; j < using_;)
            {
                std::string name;
                if (!s.ParseName(j, name))
                    return Fail("expected table name");
                if (s.IsPunct(j, '.') && j + 1 < s.Size() && s.tokens[j + 1].IsOperator("*"))
                    j += 2;
                targets.push_back(name);
                if (s.IsPunct(j, ','))
                    ++j;
            }
            refsStart = using_ + 1;
        }
        else
        {
            std::size_t const from = s.FindTopLevel(i, { "FROM" });
            if (from == std::string::npos)
                return Fail("malformed DELETE");
            for (std::size_t j = i; j < from;)
            {
                std::string name;
                if (!s.ParseName(j, name))
                    return Fail("expected table name");
                if (s.IsPunct(j, '.') && j + 1 < s.Size() && s.tokens[j + 1].IsOperator("*"))
                    j += 2;
                targets.push_back(name);
                if (s.IsPunct(j, ','))
                    ++j;
            }
            refsStart = from + 1;
        }

        if (targets.size() != 1)
            return Unsupported("DELETE from several tables");

        if (s.FindTopLevel(refsStart, { "LIMIT", "ORDER" }) != std::string::npos)
            return Unsupported("multi-table DELETE with ORDER BY/LIMIT");

        // Resolve the target alias among the table references.
        TableRef resolved;
        for (std::size_t j = refsStart; j < s.Size(); ++j)
        {
            if (!IsName(s.tokens[j]))
                continue;
            if (j > refsStart && !(IsJoinKeyword(s.tokens[j - 1]) || s.tokens[j - 1].IsPunct(',')))
                continue;
            std::size_t k = j;
            TableRef ref;
            if (!ParseTableRef(s, k, ref))
                continue;
            if (IdentifierEquals(ref.Reference(), targets.front()) || IdentifierEquals(ref.name, targets.front()))
            {
                resolved = ref;
                break;
            }
        }

        if (resolved.name.empty())
            return Fail("unknown table '" + targets.front() + "' in multi-table DELETE");

        TableModel model;
        if (!target.LoadTable(resolved.name, model))
            return Fail("Table '" + resolved.name + "' doesn't exist");
        if (!model.PrimaryKey())
            return Unsupported("multi-table DELETE on a table without a primary key");

        IndexModel const& pk = *model.PrimaryKey();
        std::string key = KeyColumns(pk, {});
        if (pk.columns.size() > 1)
            key = "(" + key + ")";
        return ExecuteText("DELETE FROM " + QuoteIdentifier(resolved.name) + " WHERE " + key + " IN (SELECT " + KeyColumns(pk, resolved.Reference()) +
            " FROM " + std::string(s.From(refsStart)) + ")");
    }

    bool ExecuteUpdate(std::string_view sql)
    {
        Statement s(sql);
        if (!CheckSupported(s))
            return false;

        std::size_t i = 1;
        while (s.Is(i, "LOW_PRIORITY") || s.Is(i, "IGNORE"))
            ++i;

        std::size_t const set = s.FindTopLevel(i, { "SET" });
        if (set == std::string::npos)
            return Fail("malformed UPDATE");

        std::size_t j = i;
        TableRef first;
        if (!ParseTableRef(s, j, first))
            return Fail("expected table name");

        if (j == set)
        {
            std::string text;
            if (!StripLimit(s, first.name, set, text))
                return false;
            return ExecuteText(text);
        }

        // UPDATE a [AS x] [INNER] JOIN b ... ON cond [JOIN ...] SET ... [WHERE ...]
        //   -> UPDATE a AS x SET ... FROM b ... WHERE (cond) AND (...)
        if (s.FindTopLevel(set, { "ORDER", "LIMIT" }) != std::string::npos)
            return Unsupported("multi-table UPDATE with ORDER BY/LIMIT");

        std::size_t k = j;
        if (s.Is(k, "INNER") || s.Is(k, "CROSS"))
            ++k;
        if (!s.Is(k, "JOIN") && !s.IsPunct(k, ','))
            return Unsupported("UPDATE with an outer join");
        ++k;

        std::size_t const joinedStart = k;
        TableRef joined;
        if (!ParseTableRef(s, k, joined))
            return Fail("expected table name");

        std::size_t const joinedEnd = k;
        std::string onCondition;
        std::size_t restStart = k;
        if (s.Is(k, "ON"))
        {
            std::size_t const onEnd = std::min(s.FindTopLevel(k + 1, { "JOIN", "INNER", "LEFT", "RIGHT", "CROSS", "STRAIGHT_JOIN", "NATURAL" }, set), set);
            onCondition = std::string(s.Slice(k + 1, onEnd));
            restStart = onEnd;
        }
        else if (s.Is(k, "USING"))
            return Unsupported("UPDATE ... JOIN ... USING");

        std::size_t const where = s.FindTopLevel(set + 1, { "WHERE" });
        std::size_t const assignmentsEnd = where == std::string::npos ? s.Size() : where;

        std::string assignments;
        std::size_t start = set + 1;
        for (std::size_t a = set + 1; a <= assignmentsEnd; ++a)
        {
            if (a < assignmentsEnd && !s.IsPunct(a, ','))
                continue;
            if (a < assignmentsEnd)
            {
                int depth = 0;
                for (std::size_t d = start; d < a; ++d)
                    depth += s.IsPunct(d, '(') ? 1 : s.IsPunct(d, ')') ? -1 : 0;
                if (depth)
                    continue;
            }

            std::size_t lhs = start;
            if (IsName(s.tokens[lhs]) && s.IsPunct(lhs + 1, '.'))
            {
                std::string const qualifier = NameText(s.tokens[lhs]);
                if (!IdentifierEquals(qualifier, first.Reference()))
                    return Unsupported("multi-table UPDATE of a joined table");
                lhs += 2;
            }

            if (!assignments.empty())
                assignments += ", ";
            assignments += std::string(s.Slice(lhs, a));
            start = a + 1;
        }

        std::string text = "UPDATE " + QuoteIdentifier(first.name);
        if (!first.alias.empty())
            text += " AS " + QuoteIdentifier(first.alias);
        text += " SET " + assignments + " FROM " + std::string(s.Slice(joinedStart, joinedEnd));
        if (restStart < set)
            text += " " + std::string(s.Slice(restStart, set));

        std::string condition;
        if (!onCondition.empty())
            condition = "(" + onCondition + ")";
        if (where != std::string::npos)
            condition += (condition.empty() ? "(" : " AND (") + std::string(s.From(where + 1)) + ")";
        if (!condition.empty())
            text += " WHERE " + condition;

        return ExecuteText(text);
    }
};

ScriptRunner::ScriptRunner(IScriptTarget& target, ScriptRunnerOptions const& options) : _impl(std::make_unique<Impl>(target, options)) { }

ScriptRunner::~ScriptRunner() = default;

bool ScriptRunner::RunFile(std::filesystem::path const& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in)
    {
        _impl->source = file.generic_string();
        _impl->current = nullptr;
        return _impl->Fail("cannot open file");
    }

    char bom[3] = {};
    in.read(bom, 3);
    if (in.gcount() != 3 || bom[0] != '\xEF' || bom[1] != '\xBB' || bom[2] != '\xBF')
    {
        in.clear();
        in.seekg(0);
    }

    return _impl->Run(in, file.generic_string());
}

bool ScriptRunner::RunStream(std::istream& in, std::string_view sourceName)
{
    return _impl->Run(in, sourceName);
}

bool ScriptRunner::RunText(std::string_view sql, std::string_view sourceName)
{
    std::istringstream in{ std::string(sql) };
    return _impl->Run(in, sourceName);
}

ScriptError const& ScriptRunner::GetError() const
{
    return _impl->error;
}

uint64 ScriptRunner::GetExecutedStatements() const
{
    return _impl->executed;
}
