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
#include "SchemaDdl.h"
#include "SqlDialect.h"
#include <algorithm>
#include <charconv>

namespace
{
    std::string ToLower(std::string_view text)
    {
        std::string result(text);
        for (char& c : result)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return result;
    }

    bool EndsWith(std::string_view text, std::string_view suffix)
    {
        return text.size() >= suffix.size() && IdentifierEquals(text.substr(text.size() - suffix.size()), suffix);
    }

    std::string SqliteDecodeString(std::string_view literal)
    {
        std::string result;
        if (literal.size() < 2)
            return result;

        char const quote = literal.front();
        std::string_view body = literal.substr(1, literal.size() - 2);
        result.reserve(body.size());
        for (std::size_t i = 0; i < body.size(); ++i)
        {
            result += body[i];
            if (body[i] == quote && i + 1 < body.size() && body[i + 1] == quote)
                ++i;
        }
        return result;
    }

    std::string MySqlQuoteString(std::string_view text)
    {
        return "'" + MySqlEscape(text) + "'";
    }

    struct ColumnExtras
    {
        std::optional<IndexKind> key;
        std::vector<std::string> checks;
        bool explicitCollation = false;
        std::optional<ForeignKeyModel> reference;
    };

    class DdlParser
    {
    public:
        DdlParser(std::string_view sql, DdlSyntax syntax) : _sql(sql), _syntax(syntax)
        {
            for (MySqlToken const& token : MySqlTokenize(sql))
                if (token.type != MySqlTokenType::VersionComment)
                    _tokens.push_back(token);

            _end.offset = sql.size();
        }

        std::string const& Error() const { return _error; }

        bool ParseCreateTable(TableModel& table, bool& ifNotExists)
        {
            ifNotExists = false;
            if (!Expect("CREATE"))
                return false;
            Accept("TEMPORARY");
            if (!Expect("TABLE"))
                return false;
            if (Accept("IF"))
            {
                if (!Expect("NOT") || !Expect("EXISTS"))
                    return false;
                ifNotExists = true;
            }

            if (!ParseName(table.name))
                return false;

            if (!Peek().IsPunct('('))
                return Fail("CREATE TABLE without a column list is not supported");
            Take();

            std::vector<bool> explicitCollation;
            while (true)
            {
                if (!ParseCreateDefinition(table, explicitCollation))
                    return false;
                if (AcceptPunct(','))
                    continue;
                if (AcceptPunct(')'))
                    break;
                return Fail("expected ',' or ')'");
            }

            bool tableCaseInsensitive = _syntax == DdlSyntax::MySql;
            std::optional<bool> collationCi;
            while (!AtEnd())
            {
                if (Accept("AUTO_INCREMENT"))
                {
                    AcceptOp("=");
                    uint64 value = 0;
                    if (!ParseUInt(value))
                        return false;
                    table.autoIncrement = value;
                }
                else if (Accept("COLLATE"))
                {
                    AcceptOp("=");
                    collationCi = EndsWith(Take().text, "_ci");
                }
                else if (Peek().Is("CHARSET") || (Peek().Is("CHARACTER") && Peek(1).Is("SET")))
                {
                    if (Take().Is("CHARACTER"))
                        Take();
                    AcceptOp("=");
                    if (Take().Is("binary"))
                        tableCaseInsensitive = false;
                }
                else
                    Take();
            }

            if (collationCi)
                tableCaseInsensitive = *collationCi;

            for (std::size_t i = 0; i < table.columns.size(); ++i)
            {
                ColumnModel& column = table.columns[i];
                if (!explicitCollation[i])
                    column.caseInsensitive = tableCaseInsensitive && column.affinity == ColumnAffinity::Text;
            }

            if (IndexModel const* pk = table.PrimaryKey())
                for (std::string const& name : pk->columns)
                    if (ColumnModel* column = table.FindColumn(name))
                        column->notNull = true;

            return true;
        }

        bool ParseAlterTable(AlterTableModel& alter)
        {
            if (!Expect("ALTER"))
                return false;
            Accept("ONLINE");
            Accept("IGNORE");
            if (!Expect("TABLE") || !ParseName(alter.table))
                return false;

            do
            {
                if (AtEnd())
                    break;
                if (!ParseAlterSpec(alter))
                    return false;
            } while (AcceptPunct(','));

            if (!AtEnd())
                return Fail("unexpected text after ALTER TABLE specification");

            return true;
        }

        bool ParseIndexStatement(AlterTableModel& alter)
        {
            AlterOp op;
            if (Accept("CREATE"))
            {
                op.kind = AlterOpKind::AddIndex;
                if (Accept("UNIQUE"))
                    op.index.kind = IndexKind::Unique;
                else if (Accept("FULLTEXT") || Accept("SPATIAL"))
                    op.index.kind = IndexKind::FullText;

                if (!Expect("INDEX"))
                    return false;
                if (Accept("IF"))
                {
                    if (!Expect("NOT") || !Expect("EXISTS"))
                        return false;
                    op.ifNotExists = true;
                }

                if (!ParseName(op.index.name))
                    return false;
                SkipIndexOptions();
                if (!Expect("ON") || !ParseName(alter.table) || !ParseKeyParts(op.index.columns))
                    return false;
                SkipIndexOptions();
            }
            else if (Accept("DROP"))
            {
                op.kind = AlterOpKind::DropIndex;
                if (!Expect("INDEX"))
                    return false;
                if (Accept("IF"))
                {
                    if (!Expect("EXISTS"))
                        return false;
                    op.ifExists = true;
                }
                if (!ParseName(op.name) || !Expect("ON") || !ParseName(alter.table))
                    return false;
            }
            else
                return Fail("expected CREATE INDEX or DROP INDEX");

            while (!AtEnd())
            {
                if (Accept("ALGORITHM") || Accept("LOCK"))
                {
                    AcceptOp("=");
                    Take();
                }
                else
                    return Fail("unexpected text after index statement");
            }

            alter.ops.push_back(std::move(op));
            return true;
        }

    private:
        MySqlToken const& Peek(std::size_t ahead = 0) const
        {
            return _pos + ahead < _tokens.size() ? _tokens[_pos + ahead] : _end;
        }

        MySqlToken const& Take()
        {
            MySqlToken const& token = Peek();
            if (_pos < _tokens.size())
                ++_pos;
            return token;
        }

        bool AtEnd() const
        {
            return _pos >= _tokens.size() || (_pos + 1 == _tokens.size() && _tokens[_pos].IsPunct(';'));
        }

        bool Accept(std::string_view keyword)
        {
            if (!Peek().Is(keyword))
                return false;
            ++_pos;
            return true;
        }

        bool AcceptPunct(char c)
        {
            if (!Peek().IsPunct(c))
                return false;
            ++_pos;
            return true;
        }

        bool AcceptOp(std::string_view op)
        {
            if (!Peek().IsOperator(op))
                return false;
            ++_pos;
            return true;
        }

        bool Expect(std::string_view keyword)
        {
            if (Accept(keyword))
                return true;
            return Fail("expected " + std::string(keyword));
        }

        bool ExpectPunct(char c)
        {
            if (AcceptPunct(c))
                return true;
            return Fail(std::string("expected '") + c + "'");
        }

        bool Fail(std::string const& message)
        {
            if (_error.empty())
            {
                MySqlToken const& token = Peek();
                if (token.type == MySqlTokenType::End)
                    _error = message + " at end of statement";
                else
                    _error = message + " near '" + std::string(_sql.substr(token.offset, std::min<std::size_t>(40, _sql.size() - token.offset))) + "'";
            }
            return false;
        }

        bool IsIdentifier(MySqlToken const& token) const
        {
            switch (token.type)
            {
                case MySqlTokenType::Identifier:
                case MySqlTokenType::QuotedIdentifier:
                    return true;
                case MySqlTokenType::String:
                    return _syntax == DdlSyntax::Sqlite && token.text.front() == '"';
                default:
                    return false;
            }
        }

        std::string IdentifierText(MySqlToken const& token) const
        {
            if (token.type == MySqlTokenType::QuotedIdentifier)
                return MySqlUnquoteIdentifier(token.text);
            if (token.type == MySqlTokenType::String)
                return SqliteDecodeString(token.text);
            return std::string(token.text);
        }

        bool ParseIdentifier(std::string& name)
        {
            if (!IsIdentifier(Peek()))
                return Fail("expected identifier");
            name = IdentifierText(Take());
            return true;
        }

        bool ParseName(std::string& name)
        {
            if (!ParseIdentifier(name))
                return false;
            while (Peek().IsPunct('.') && IsIdentifier(Peek(1)))
            {
                Take();
                name = IdentifierText(Take());
            }
            return true;
        }

        bool ParseUInt(uint64& value)
        {
            MySqlToken const& token = Peek();
            if (token.type != MySqlTokenType::Number)
                return Fail("expected number");
            auto [ptr, ec] = std::from_chars(token.text.data(), token.text.data() + token.text.size(), value);
            if (ec != std::errc() || ptr != token.text.data() + token.text.size())
                return Fail("expected integer");
            Take();
            return true;
        }

        // Source text between tokens [from, to), single spaces where the source had whitespace;
        // identifiers and strings rewritten into MySQL quoting.
        std::string Expression(std::size_t from, std::size_t to) const
        {
            std::string result;
            for (std::size_t i = from; i < to; ++i)
            {
                MySqlToken const& token = _tokens[i];
                if (i > from && token.offset > _tokens[i - 1].offset + _tokens[i - 1].text.size())
                    result += ' ';

                if (_syntax == DdlSyntax::Sqlite && token.type == MySqlTokenType::String)
                {
                    if (token.text.front() == '"')
                    {
                        std::string name = SqliteDecodeString(token.text);
                        std::string quoted;
                        for (char c : name)
                        {
                            quoted += c;
                            if (c == '`')
                                quoted += '`';
                        }
                        result += "`" + quoted + "`";
                    }
                    else
                        result += MySqlQuoteString(SqliteDecodeString(token.text));
                }
                else
                    result += token.text;
            }
            return result;
        }

        // Balanced parenthesised group starting at '('; returns the token range inside it.
        bool SkipGroup(std::size_t& innerFrom, std::size_t& innerTo)
        {
            if (!ExpectPunct('('))
                return false;
            innerFrom = _pos;
            int depth = 1;
            while (_pos < _tokens.size())
            {
                MySqlToken const& token = _tokens[_pos];
                if (token.IsPunct('('))
                    ++depth;
                else if (token.IsPunct(')') && --depth == 0)
                {
                    innerTo = _pos;
                    ++_pos;
                    return true;
                }
                ++_pos;
            }
            return Fail("unbalanced parentheses");
        }

        bool ParseKeyParts(std::vector<std::string>& columns)
        {
            if (!ExpectPunct('('))
                return false;
            do
            {
                if (Peek().IsPunct('('))
                    return Fail("functional key parts are not supported");
                std::string name;
                if (!ParseIdentifier(name))
                    return false;
                if (Peek().IsPunct('('))
                {
                    std::size_t from = 0, to = 0;
                    if (!SkipGroup(from, to))
                        return false;
                }
                if (!Accept("ASC"))
                    Accept("DESC");
                columns.push_back(std::move(name));
            } while (AcceptPunct(','));
            return ExpectPunct(')');
        }

        void SkipIndexOptions()
        {
            while (true)
            {
                if (Accept("USING"))
                    Take();
                else if (Accept("KEY_BLOCK_SIZE"))
                {
                    AcceptOp("=");
                    Take();
                }
                else if (Accept("COMMENT"))
                    Take();
                else if (Accept("VISIBLE") || Accept("INVISIBLE"))
                    continue;
                else if (Peek().Is("WITH") && Peek(1).Is("PARSER"))
                {
                    Take();
                    Take();
                    Take();
                }
                else
                    break;
            }
        }

        bool ParseIndexDefinition(IndexModel& index, bool allowName)
        {
            if (allowName && !Peek().IsPunct('(') && !Peek().Is("USING"))
            {
                if (!ParseIdentifier(index.name))
                    return false;
            }
            SkipIndexOptions();
            if (!ParseKeyParts(index.columns))
                return false;
            SkipIndexOptions();
            return true;
        }

        bool ParseReferenceAction(std::string& action)
        {
            if (Accept("CASCADE"))
                action = "CASCADE";
            else if (Accept("RESTRICT"))
                action = "RESTRICT";
            else if (Accept("SET"))
            {
                if (Accept("NULL"))
                    action = "SET NULL";
                else if (Accept("DEFAULT"))
                    action = "SET DEFAULT";
                else
                    return Fail("expected NULL or DEFAULT");
            }
            else if (Accept("NO"))
            {
                if (!Expect("ACTION"))
                    return false;
                action = "NO ACTION";
            }
            else
                return Fail("expected referential action");
            return true;
        }

        bool ParseReferences(ForeignKeyModel& fk)
        {
            if (!Expect("REFERENCES") || !ParseName(fk.refTable))
                return false;
            if (Peek().IsPunct('(') && !ParseKeyParts(fk.refColumns))
                return false;
            while (true)
            {
                if (Accept("MATCH"))
                    Take();
                else if (Peek().Is("ON") && Peek(1).Is("DELETE"))
                {
                    Take();
                    Take();
                    if (!ParseReferenceAction(fk.onDelete))
                        return false;
                }
                else if (Peek().Is("ON") && Peek(1).Is("UPDATE"))
                {
                    Take();
                    Take();
                    if (!ParseReferenceAction(fk.onUpdate))
                        return false;
                }
                else
                    break;
            }
            return true;
        }

        bool ParseForeignKey(ForeignKeyModel& fk)
        {
            if (!Expect("FOREIGN") || !Expect("KEY"))
                return false;
            if (IsIdentifier(Peek()) && !Peek().IsPunct('('))
            {
                std::string name;
                if (!ParseIdentifier(name))
                    return false;
                if (fk.name.empty())
                    fk.name = std::move(name);
            }
            if (!ParseKeyParts(fk.columns))
                return false;
            return ParseReferences(fk);
        }

        bool ParseCheck(std::string& expression)
        {
            if (!Expect("CHECK"))
                return false;
            std::size_t from = 0, to = 0;
            if (!SkipGroup(from, to))
                return false;
            expression = "(" + Expression(from, to) + ")";
            if (Peek().Is("NOT") && Peek(1).Is("ENFORCED"))
            {
                Take();
                Take();
            }
            else
                Accept("ENFORCED");
            return true;
        }

        bool ParseDataType(ColumnModel& column)
        {
            if (Peek().type != MySqlTokenType::Identifier)
                return Fail("expected data type");

            std::string base = ToLower(Take().text);
            if (base == "double")
                Accept("precision");
            else if (base == "national")
            {
                if (Peek().type != MySqlTokenType::Identifier)
                    return Fail("expected data type");
                base = ToLower(Take().text);
            }
            if ((base == "character" || base == "char") && Accept("varying"))
                base = "varchar";
            else if (base == "character")
                base = "char";
            else if (base == "long")
            {
                if (Accept("varbinary"))
                    base = "mediumblob";
                else
                {
                    Accept("varchar");
                    base = "mediumtext";
                }
            }
            else if (base == "bool" || base == "boolean")
                base = "tinyint";

            static constexpr std::string_view IntegerTypes[] = { "tinyint", "smallint", "mediumint", "int", "integer", "bigint", "middleint", "int1", "int2", "int3", "int4", "int8" };
            bool const isInteger = std::find(std::begin(IntegerTypes), std::end(IntegerTypes), base) != std::end(IntegerTypes);
            bool const isEnum = base == "enum" || base == "set";

            std::string args;
            if (Peek().IsPunct('('))
            {
                std::size_t from = 0, to = 0;
                if (!SkipGroup(from, to))
                    return false;

                if (isEnum)
                {
                    for (std::size_t i = from; i < to; ++i)
                    {
                        MySqlToken const& token = _tokens[i];
                        if (token.type == MySqlTokenType::String)
                            column.enumValues.push_back(_syntax == DdlSyntax::Sqlite ? SqliteDecodeString(token.text) : MySqlDecodeString(token.text));
                        else if (!token.IsPunct(','))
                            return Fail("expected enum value");
                    }
                }
                else
                {
                    for (std::size_t i = from; i < to; ++i)
                        if (!_tokens[i].IsTrivia())
                            args += _tokens[i].text;
                }
            }

            while (true)
            {
                if (Accept("UNSIGNED") || Accept("ZEROFILL"))
                    column.isUnsigned = true;
                else if (!Accept("SIGNED"))
                    break;
            }

            std::string type = base;
            if (isEnum)
            {
                type += "(";
                for (std::size_t i = 0; i < column.enumValues.size(); ++i)
                {
                    if (i)
                        type += ",";
                    type += MySqlQuoteString(column.enumValues[i]);
                }
                type += ")";
            }
            else if (!args.empty() && !isInteger)
                type += "(" + args + ")";

            column.affinity = MySqlTypeAffinity(type);
            if (column.affinity != ColumnAffinity::Integer && column.affinity != ColumnAffinity::Real && column.affinity != ColumnAffinity::Decimal)
                column.isUnsigned = false;
            if (column.isUnsigned)
                type += " unsigned";

            column.type = std::move(type);
            return true;
        }

        bool ParseDefault(std::optional<std::string>& value)
        {
            MySqlToken const& token = Peek();
            if (token.IsPunct('('))
            {
                std::size_t from = 0, to = 0;
                if (!SkipGroup(from, to))
                    return false;
                value = "(" + Expression(from, to) + ")";
                return true;
            }

            if (token.IsOperator("-") || token.IsOperator("+"))
            {
                std::string sign(Take().text);
                if (Peek().type != MySqlTokenType::Number)
                    return Fail("expected number");
                value = (sign == "-" ? sign : std::string()) + std::string(Take().text);
                return true;
            }

            if (token.type == MySqlTokenType::Identifier && token.text.front() == '_' && Peek(1).type == MySqlTokenType::String)
                Take();

            MySqlToken const& literal = Peek();
            switch (literal.type)
            {
                case MySqlTokenType::String:
                    if (_syntax == DdlSyntax::Sqlite)
                    {
                        if (literal.text.front() == '"')
                            return Fail("expected default value");
                        value = MySqlQuoteString(SqliteDecodeString(literal.text));
                    }
                    else
                        value = std::string(literal.text);
                    Take();
                    return true;
                case MySqlTokenType::Number:
                case MySqlTokenType::HexNumber:
                case MySqlTokenType::BitString:
                    value = std::string(Take().text);
                    return true;
                case MySqlTokenType::Identifier:
                    break;
                default:
                    return Fail("expected default value");
            }

            if (Accept("NULL"))
                value = "NULL";
            else if (Accept("TRUE"))
                value = "1";
            else if (Accept("FALSE"))
                value = "0";
            else if (Accept("CURRENT_TIMESTAMP") || Accept("NOW") || Accept("LOCALTIMESTAMP") || Accept("LOCALTIME"))
            {
                if (Peek().IsPunct('('))
                {
                    std::size_t from = 0, to = 0;
                    if (!SkipGroup(from, to))
                        return false;
                }
                value = "CURRENT_TIMESTAMP";
            }
            else
                return Fail("unsupported default value");

            return true;
        }

        bool ParseColumnDefinition(ColumnModel& column, ColumnExtras& extras)
        {
            if (!ParseIdentifier(column.name) || !ParseDataType(column))
                return false;

            while (!AtEnd() && !Peek().IsPunct(',') && !Peek().IsPunct(')') && !Peek().Is("FIRST") && !Peek().Is("AFTER"))
            {
                if (Peek().Is("NOT") && Peek(1).Is("NULL"))
                {
                    Take();
                    Take();
                    column.notNull = true;
                }
                else if (Accept("NULL"))
                    column.notNull = false;
                else if (Accept("DEFAULT"))
                {
                    if (!ParseDefault(column.defaultValue))
                        return false;
                    if (column.defaultValue == "NULL")
                        column.defaultValue.reset();
                }
                else if (Accept("AUTO_INCREMENT") || Accept("AUTOINCREMENT"))
                    column.autoIncrement = true;
                else if (Accept("UNIQUE"))
                {
                    if (!Accept("KEY"))
                        Accept("INDEX");
                    if (!extras.key)
                        extras.key = IndexKind::Unique;
                }
                else if (Accept("PRIMARY"))
                {
                    if (!Expect("KEY"))
                        return false;
                    if (!Accept("ASC"))
                        Accept("DESC");
                    extras.key = IndexKind::Primary;
                }
                else if (Accept("KEY"))
                    extras.key = IndexKind::Primary;
                else if (Accept("COMMENT"))
                {
                    if (Peek().type != MySqlTokenType::String)
                        return Fail("expected comment text");
                    Take();
                }
                else if (Accept("COLLATE"))
                {
                    if (!IsIdentifier(Peek()) && Peek().type != MySqlTokenType::String)
                        return Fail("expected collation");
                    std::string name = IdentifierText(Take());
                    extras.explicitCollation = true;
                    column.caseInsensitive = EndsWith(name, "_ci") || IdentifierEquals(name, "nocase");
                }
                else if (Peek().Is("CHARSET") || (Peek().Is("CHARACTER") && Peek(1).Is("SET")))
                {
                    if (Take().Is("CHARACTER"))
                        Take();
                    if (Take().Is("binary"))
                    {
                        extras.explicitCollation = true;
                        column.caseInsensitive = false;
                    }
                }
                else if (Accept("BINARY"))
                {
                    extras.explicitCollation = true;
                    column.caseInsensitive = false;
                }
                else if (Peek().Is("ON") && Peek(1).Is("UPDATE"))
                {
                    Take();
                    Take();
                    if (!Accept("CURRENT_TIMESTAMP") && !Accept("NOW") && !Accept("LOCALTIMESTAMP") && !Accept("LOCALTIME"))
                        return Fail("unsupported ON UPDATE value");
                    if (Peek().IsPunct('('))
                    {
                        std::size_t from = 0, to = 0;
                        if (!SkipGroup(from, to))
                            return false;
                    }
                    column.onUpdateCurrentTimestamp = true;
                }
                else if (Accept("COLUMN_FORMAT") || Accept("STORAGE") || Accept("SRID"))
                    Take();
                else if (Accept("VISIBLE") || Accept("INVISIBLE"))
                    continue;
                else if (Peek().Is("CONSTRAINT") || Peek().Is("CHECK"))
                {
                    if (Accept("CONSTRAINT") && !Peek().Is("CHECK"))
                        Take();
                    std::size_t const checkStart = _pos;
                    std::string expression;
                    if (!ParseCheck(expression))
                        return false;
                    if (!ParseEnumCheck(column, checkStart))
                        extras.checks.push_back(std::move(expression));
                }
                else if (Peek().Is("REFERENCES"))
                {
                    ForeignKeyModel fk;
                    fk.columns.push_back(column.name);
                    if (!ParseReferences(fk))
                        return false;
                    if (_syntax == DdlSyntax::Sqlite)
                        extras.reference = std::move(fk);
                }
                else if (Peek().Is("GENERATED") || Peek().Is("AS"))
                    return Fail("generated columns are not supported");
                else
                    return Fail("unexpected column attribute");
            }

            return true;
        }

        // SQLite form of an enum: CHECK ("col" IN ('a', 'b'))
        bool ParseEnumCheck(ColumnModel& column, std::size_t checkStart)
        {
            if (_syntax != DdlSyntax::Sqlite)
                return false;

            std::size_t i = checkStart + 2;
            if (i + 3 >= _pos || !IsIdentifier(_tokens[i]) || !IdentifierEquals(IdentifierText(_tokens[i]), column.name) ||
                !_tokens[i + 1].Is("IN") || !_tokens[i + 2].IsPunct('('))
                return false;

            std::vector<std::string> values;
            for (i += 3; i < _pos && !_tokens[i].IsPunct(')'); ++i)
            {
                MySqlToken const& token = _tokens[i];
                if (token.type == MySqlTokenType::String && token.text.front() == '\'')
                    values.push_back(SqliteDecodeString(token.text));
                else if (!token.IsPunct(','))
                    return false;
            }

            column.enumValues = std::move(values);
            column.type = "enum(";
            for (std::size_t v = 0; v < column.enumValues.size(); ++v)
            {
                if (v)
                    column.type += ",";
                column.type += MySqlQuoteString(column.enumValues[v]);
            }
            column.type += ")";
            column.affinity = ColumnAffinity::Text;
            return true;
        }

        void AddColumnKey(TableModel& table, ColumnModel const& column, ColumnExtras const& extras)
        {
            if (!extras.key)
                return;

            if (*extras.key == IndexKind::Primary)
            {
                IndexModel pk;
                pk.name = "PRIMARY";
                pk.kind = IndexKind::Primary;
                pk.columns.push_back(column.name);
                table.indexes.push_back(std::move(pk));
                return;
            }

            IndexModel index;
            index.kind = *extras.key;
            index.columns.push_back(column.name);
            index.name = AutoIndexName(table, index);
            table.indexes.push_back(std::move(index));
        }

        bool ParseCreateDefinition(TableModel& table, std::vector<bool>& explicitCollation)
        {
            std::string constraintName;
            bool hasConstraint = false;
            if (Accept("CONSTRAINT"))
            {
                hasConstraint = true;
                if (!Peek().Is("PRIMARY") && !Peek().Is("UNIQUE") && !Peek().Is("FOREIGN") && !Peek().Is("CHECK"))
                    if (!ParseIdentifier(constraintName))
                        return false;
            }

            if (Accept("PRIMARY"))
            {
                IndexModel index;
                index.kind = IndexKind::Primary;
                if (!Expect("KEY") || !ParseIndexDefinition(index, false))
                    return false;
                index.name = "PRIMARY";
                table.indexes.push_back(std::move(index));
                return true;
            }

            if (Accept("UNIQUE"))
            {
                IndexModel index;
                index.kind = IndexKind::Unique;
                if (!Accept("INDEX"))
                    Accept("KEY");
                if (!ParseIndexDefinition(index, true))
                    return false;
                if (index.name.empty())
                    index.name = constraintName;
                if (index.name.empty())
                    index.name = AutoIndexName(table, index);
                table.indexes.push_back(std::move(index));
                return true;
            }

            if (Peek().Is("FOREIGN"))
            {
                ForeignKeyModel fk;
                fk.name = constraintName;
                if (!ParseForeignKey(fk))
                    return false;
                table.foreignKeys.push_back(std::move(fk));
                return true;
            }

            if (Peek().Is("CHECK"))
            {
                std::string expression;
                if (!ParseCheck(expression))
                    return false;
                table.checks.push_back(std::move(expression));
                return true;
            }

            if (hasConstraint)
                return Fail("expected constraint");

            if (Peek().Is("INDEX") || Peek().Is("KEY") || Peek().Is("FULLTEXT") || Peek().Is("SPATIAL"))
            {
                bool const fullText = Peek().Is("FULLTEXT") || Peek().Is("SPATIAL");
                Take();
                IndexModel index;
                if (fullText)
                {
                    index.kind = IndexKind::FullText;
                    if (!Accept("INDEX"))
                        Accept("KEY");
                }
                if (!ParseIndexDefinition(index, true))
                    return false;
                if (index.name.empty())
                    index.name = AutoIndexName(table, index);
                table.indexes.push_back(std::move(index));
                return true;
            }

            ColumnModel column;
            ColumnExtras extras;
            if (!ParseColumnDefinition(column, extras))
                return false;

            if (table.FindColumn(column.name))
                return Fail("duplicate column name '" + column.name + "'");

            AddColumnKey(table, column, extras);
            for (std::string& check : extras.checks)
                table.checks.push_back(std::move(check));
            if (extras.reference)
                table.foreignKeys.push_back(std::move(*extras.reference));

            explicitCollation.push_back(extras.explicitCollation);
            table.columns.push_back(std::move(column));
            return true;
        }

        bool ParseColumnPosition(AlterOp& op)
        {
            if (Accept("FIRST"))
                op.position = ColumnPosition::First;
            else if (Accept("AFTER"))
            {
                op.position = ColumnPosition::After;
                if (!ParseIdentifier(op.after))
                    return false;
            }
            return true;
        }

        bool ParseAlterColumn(AlterOp& op, ColumnExtras& extras)
        {
            if (!ParseColumnDefinition(op.column, extras))
                return false;
            if (!extras.explicitCollation)
                op.column.caseInsensitive = op.column.affinity == ColumnAffinity::Text;
            if (extras.key)
            {
                op.index.kind = *extras.key;
                op.index.columns.push_back(op.column.name);
                if (*extras.key == IndexKind::Primary)
                {
                    op.index.name = "PRIMARY";
                    op.column.notNull = true;
                }
            }
            return ParseColumnPosition(op);
        }

        void SkipTableOption()
        {
            int depth = 0;
            while (!AtEnd())
            {
                MySqlToken const& token = Peek();
                if (depth == 0 && token.IsPunct(','))
                    break;
                if (token.IsPunct('('))
                    ++depth;
                else if (token.IsPunct(')'))
                    --depth;
                Take();
            }
        }

        bool ParseAlterSpec(AlterTableModel& alter)
        {
            AlterOp op;

            if (Accept("ADD"))
            {
                std::string constraintName;
                bool hasConstraint = false;
                if (Accept("CONSTRAINT"))
                {
                    hasConstraint = true;
                    if (!Peek().Is("PRIMARY") && !Peek().Is("UNIQUE") && !Peek().Is("FOREIGN") && !Peek().Is("CHECK"))
                        if (!ParseIdentifier(constraintName))
                            return false;
                }

                if (Accept("PRIMARY"))
                {
                    op.kind = AlterOpKind::AddIndex;
                    op.index.kind = IndexKind::Primary;
                    if (!Expect("KEY") || !ParseIndexDefinition(op.index, false))
                        return false;
                    op.index.name = "PRIMARY";
                }
                else if (Accept("UNIQUE"))
                {
                    op.kind = AlterOpKind::AddIndex;
                    op.index.kind = IndexKind::Unique;
                    if (!Accept("INDEX"))
                        Accept("KEY");
                    if (!ParseIfNotExists(op) || !ParseIndexDefinition(op.index, true))
                        return false;
                    if (op.index.name.empty())
                        op.index.name = constraintName;
                }
                else if (Peek().Is("FOREIGN"))
                {
                    op.kind = AlterOpKind::AddForeignKey;
                    op.foreignKey.name = constraintName;
                    if (!ParseForeignKey(op.foreignKey))
                        return false;
                }
                else if (Peek().Is("CHECK"))
                {
                    std::string expression;
                    if (!ParseCheck(expression))
                        return false;
                    op.kind = AlterOpKind::TableOption;
                }
                else if (hasConstraint)
                    return Fail("expected constraint");
                else if (Peek().Is("INDEX") || Peek().Is("KEY") || Peek().Is("FULLTEXT") || Peek().Is("SPATIAL"))
                {
                    op.kind = AlterOpKind::AddIndex;
                    if (Peek().Is("FULLTEXT") || Peek().Is("SPATIAL"))
                    {
                        Take();
                        op.index.kind = IndexKind::FullText;
                        if (!Accept("INDEX"))
                            Accept("KEY");
                    }
                    else
                        Take();
                    if (!ParseIfNotExists(op) || !ParseIndexDefinition(op.index, true))
                        return false;
                }
                else
                {
                    Accept("COLUMN");
                    bool ifNotExists = false;
                    if (Peek().Is("IF") && Peek(1).Is("NOT"))
                    {
                        Take();
                        Take();
                        if (!Expect("EXISTS"))
                            return false;
                        ifNotExists = true;
                    }

                    if (AcceptPunct('('))
                    {
                        do
                        {
                            AlterOp column;
                            column.kind = AlterOpKind::AddColumn;
                            column.ifNotExists = ifNotExists;
                            ColumnExtras extras;
                            if (!ParseAlterColumn(column, extras))
                                return false;
                            alter.ops.push_back(std::move(column));
                        } while (AcceptPunct(','));
                        return ExpectPunct(')');
                    }

                    op.kind = AlterOpKind::AddColumn;
                    op.ifNotExists = ifNotExists;
                    ColumnExtras extras;
                    if (!ParseAlterColumn(op, extras))
                        return false;
                }

                alter.ops.push_back(std::move(op));
                return true;
            }

            if (Accept("DROP"))
            {
                if (Accept("PRIMARY"))
                {
                    if (!Expect("KEY"))
                        return false;
                    op.kind = AlterOpKind::DropPrimaryKey;
                }
                else if (Accept("INDEX") || Accept("KEY"))
                {
                    op.kind = AlterOpKind::DropIndex;
                    if (!ParseIfExists(op) || !ParseIdentifier(op.name))
                        return false;
                }
                else if (Accept("FOREIGN"))
                {
                    if (!Expect("KEY"))
                        return false;
                    op.kind = AlterOpKind::DropForeignKey;
                    if (!ParseIfExists(op) || !ParseIdentifier(op.name))
                        return false;
                }
                else if (Accept("CONSTRAINT"))
                {
                    op.kind = AlterOpKind::DropForeignKey;
                    op.ifExists = true;
                    if (!ParseIfExists(op) || !ParseIdentifier(op.name))
                        return false;
                }
                else if (Accept("CHECK"))
                {
                    op.kind = AlterOpKind::TableOption;
                    std::string name;
                    if (!ParseIdentifier(name))
                        return false;
                }
                else
                {
                    Accept("COLUMN");
                    op.kind = AlterOpKind::DropColumn;
                    if (!ParseIfExists(op) || !ParseIdentifier(op.name))
                        return false;
                    Accept("RESTRICT");
                    Accept("CASCADE");
                }

                alter.ops.push_back(std::move(op));
                return true;
            }

            if (Accept("MODIFY"))
            {
                Accept("COLUMN");
                op.kind = AlterOpKind::ModifyColumn;
                ColumnExtras extras;
                if (!ParseAlterColumn(op, extras))
                    return false;
                op.name = op.column.name;
                alter.ops.push_back(std::move(op));
                return true;
            }

            if (Accept("CHANGE"))
            {
                Accept("COLUMN");
                op.kind = AlterOpKind::ChangeColumn;
                ColumnExtras extras;
                if (!ParseIdentifier(op.name) || !ParseAlterColumn(op, extras))
                    return false;
                alter.ops.push_back(std::move(op));
                return true;
            }

            if (Accept("RENAME"))
            {
                if (Accept("COLUMN"))
                {
                    op.kind = AlterOpKind::RenameColumn;
                    if (!ParseIdentifier(op.name) || !Expect("TO") || !ParseIdentifier(op.newName))
                        return false;
                }
                else if (Peek().Is("INDEX") || Peek().Is("KEY"))
                    return Fail("RENAME INDEX is not supported");
                else
                {
                    if (!Accept("TO"))
                        Accept("AS");
                    op.kind = AlterOpKind::RenameTable;
                    if (!ParseName(op.newName))
                        return false;
                }

                alter.ops.push_back(std::move(op));
                return true;
            }

            if (Peek().Is("ALTER") && !Peek(1).Is("INDEX") && !Peek(1).Is("CHECK") && !Peek(1).Is("CONSTRAINT"))
            {
                Take();
                Accept("COLUMN");
                op.kind = AlterOpKind::AlterColumnDefault;
                if (!ParseIdentifier(op.name))
                    return false;
                if (Accept("SET"))
                {
                    if (Accept("VISIBLE") || Accept("INVISIBLE"))
                        op.kind = AlterOpKind::TableOption;
                    else if (!Expect("DEFAULT") || !ParseDefault(op.defaultValue))
                        return false;
                    if (op.defaultValue == "NULL")
                        op.defaultValue.reset();
                }
                else if (!Expect("DROP") || !Expect("DEFAULT"))
                    return false;

                alter.ops.push_back(std::move(op));
                return true;
            }

            if (Accept("AUTO_INCREMENT"))
            {
                AcceptOp("=");
                op.kind = AlterOpKind::SetAutoIncrement;
                if (!ParseUInt(op.autoIncrement))
                    return false;
                alter.ops.push_back(std::move(op));
                return true;
            }

            static constexpr std::string_view Options[] = { "ENGINE", "DEFAULT", "CHARSET", "CHARACTER", "COLLATE", "COMMENT", "ROW_FORMAT",
                "CONVERT", "ENABLE", "DISABLE", "FORCE", "ALGORITHM", "LOCK", "ORDER", "KEY_BLOCK_SIZE", "PACK_KEYS", "CHECKSUM",
                "DELAY_KEY_WRITE", "STATS_PERSISTENT", "STATS_AUTO_RECALC", "STATS_SAMPLE_PAGES", "MAX_ROWS", "MIN_ROWS", "AVG_ROW_LENGTH",
                "ALTER", "WITH", "WITHOUT", "VALIDATION" };
            for (std::string_view option : Options)
            {
                if (Peek().Is(option))
                {
                    SkipTableOption();
                    alter.ops.push_back(op);
                    return true;
                }
            }

            return Fail("unsupported ALTER TABLE specification");
        }

        bool ParseIfNotExists(AlterOp& op)
        {
            if (Peek().Is("IF") && Peek(1).Is("NOT"))
            {
                Take();
                Take();
                if (!Expect("EXISTS"))
                    return false;
                op.ifNotExists = true;
            }
            return true;
        }

        bool ParseIfExists(AlterOp& op)
        {
            if (Peek().Is("IF") && Peek(1).Is("EXISTS"))
            {
                Take();
                Take();
                op.ifExists = true;
            }
            return true;
        }

        std::string_view _sql;
        DdlSyntax _syntax;
        std::vector<MySqlToken> _tokens;
        std::size_t _pos = 0;
        MySqlToken _end;
        std::string _error;
    };
}

ColumnAffinity MySqlTypeAffinity(std::string_view type)
{
    std::size_t const end = type.find_first_of(" (");
    std::string const base = ToLower(type.substr(0, end));

    static constexpr std::string_view Integer[] = { "tinyint", "smallint", "mediumint", "int", "integer", "bigint", "middleint",
        "int1", "int2", "int3", "int4", "int8", "bit", "bool", "boolean", "year", "serial" };
    static constexpr std::string_view Real[] = { "float", "double", "real", "float4", "float8" };
    static constexpr std::string_view Decimal[] = { "decimal", "numeric", "dec", "fixed" };
    static constexpr std::string_view Blob[] = { "binary", "varbinary", "blob", "tinyblob", "mediumblob", "longblob" };
    static constexpr std::string_view DateTime[] = { "date", "time", "datetime", "timestamp" };

    auto in = [&base](auto const& list) { return std::find(std::begin(list), std::end(list), base) != std::end(list); };
    if (in(Integer))
        return ColumnAffinity::Integer;
    if (in(Real))
        return ColumnAffinity::Real;
    if (in(Decimal))
        return ColumnAffinity::Decimal;
    if (in(Blob))
        return ColumnAffinity::Blob;
    if (in(DateTime))
        return ColumnAffinity::DateTime;
    return ColumnAffinity::Text;
}

std::string AutoIndexName(TableModel const& table, IndexModel const& index)
{
    std::string const base = index.columns.empty() ? std::string("index") : index.columns.front();
    if (!table.FindIndex(base) && !IdentifierEquals(base, "PRIMARY"))
        return base;

    for (uint32 i = 2;; ++i)
    {
        std::string name = base + "_" + std::to_string(i);
        if (!table.FindIndex(name))
            return name;
    }
}

bool ParseCreateTable(std::string_view sql, TableModel& table, bool& ifNotExists, std::string& error, DdlSyntax syntax)
{
    DdlParser parser(sql, syntax);
    table = {};
    if (parser.ParseCreateTable(table, ifNotExists))
        return true;
    error = parser.Error();
    return false;
}

bool ParseIndexStatement(std::string_view sql, AlterTableModel& alter, std::string& error, DdlSyntax syntax)
{
    DdlParser parser(sql, syntax);
    alter = {};
    if (parser.ParseIndexStatement(alter))
        return true;
    error = parser.Error();
    return false;
}

bool ParseMySqlCreateTable(std::string_view sql, TableModel& table, bool& ifNotExists, std::string& error)
{
    return ParseCreateTable(sql, table, ifNotExists, error, DdlSyntax::MySql);
}

bool ParseMySqlAlterTable(std::string_view sql, AlterTableModel& alter, std::string& error)
{
    DdlParser parser(sql, DdlSyntax::MySql);
    alter = {};
    if (parser.ParseAlterTable(alter))
        return true;
    error = parser.Error();
    return false;
}

bool ParseMySqlIndexStatement(std::string_view sql, AlterTableModel& alter, std::string& error)
{
    return ParseIndexStatement(sql, alter, error, DdlSyntax::MySql);
}
