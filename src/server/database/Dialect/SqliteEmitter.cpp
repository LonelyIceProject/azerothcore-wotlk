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
#include <sqlite3.h>

namespace
{
    std::string ToUpper(std::string_view text)
    {
        std::string result(text);
        for (char& c : result)
            if (c >= 'a' && c <= 'z')
                c = char(c - 'a' + 'A');
        return result;
    }

    std::string_view TypeBase(std::string_view type)
    {
        return type.substr(0, type.find_first_of(" ("));
    }

    bool IsNumericText(std::string_view text)
    {
        std::size_t i = 0;
        if (i < text.size() && (text[i] == '-' || text[i] == '+'))
            ++i;
        bool digits = false;
        bool dot = false;
        for (; i < text.size(); ++i)
        {
            char c = text[i];
            if (c >= '0' && c <= '9')
                digits = true;
            else if (c == '.' && !dot)
                dot = true;
            else
                return false;
        }
        return digits;
    }

    std::string HexToInteger(std::string const& bytes)
    {
        uint64 value = 0;
        for (char c : bytes)
            value = (value << 8) | uint8(c);
        return std::to_string(value);
    }

    std::string HexLiteral(std::string const& bytes)
    {
        static char const digits[] = "0123456789ABCDEF";
        std::string result = "X'";
        for (char c : bytes)
        {
            result += digits[uint8(c) >> 4];
            result += digits[uint8(c) & 0xF];
        }
        return result + "'";
    }

    std::string BitsToInteger(std::string_view literal)
    {
        std::string_view bits = literal;
        if (bits.size() >= 2 && (bits[0] == 'b' || bits[0] == 'B') && bits[1] == '\'')
            bits = bits.substr(2, bits.size() - 3);
        else if (bits.size() >= 2 && bits[0] == '0' && bits[1] == 'b')
            bits = bits.substr(2);
        uint64 value = 0;
        for (char c : bits)
            value = (value << 1) | (c == '1' ? 1 : 0);
        return std::to_string(value);
    }

    std::string MySqlExpressionToSqlite(std::string_view expression)
    {
        std::string result;
        MySqlLexer lexer(expression);
        for (MySqlToken token = lexer.Next(); token.type != MySqlTokenType::End; token = lexer.Next())
        {
            switch (token.type)
            {
                case MySqlTokenType::QuotedIdentifier:
                    result += SqliteQuoteIdentifier(MySqlUnquoteIdentifier(token.text));
                    break;
                case MySqlTokenType::String:
                    result += SqliteQuoteString(MySqlDecodeString(token.text));
                    break;
                case MySqlTokenType::Comment:
                    result += ' ';
                    break;
                default:
                    result += token.text;
                    break;
            }
        }
        return result;
    }

    std::string SqliteDefault(ColumnModel const& column)
    {
        std::string const& value = *column.defaultValue;
        bool const numeric = column.affinity == ColumnAffinity::Integer || column.affinity == ColumnAffinity::Real || column.affinity == ColumnAffinity::Decimal;

        if (value.empty())
            return "NULL";
        if (value == "CURRENT_TIMESTAMP")
            return value;
        if (value.front() == '(')
            return MySqlExpressionToSqlite(value);

        MySqlToken token = MySqlLexer(value).NextSignificant();
        switch (token.type)
        {
            case MySqlTokenType::String:
            {
                std::string text = MySqlDecodeString(token.text);
                if (numeric && IsNumericText(text))
                    return text.front() == '+' ? text.substr(1) : text;
                return SqliteQuoteString(text);
            }
            case MySqlTokenType::HexNumber:
                return numeric ? HexToInteger(MySqlDecodeHex(token.text)) : HexLiteral(MySqlDecodeHex(token.text));
            case MySqlTokenType::BitString:
                return BitsToInteger(token.text);
            default:
                break;
        }

        if (IsNumericText(value))
            return numeric ? value : SqliteQuoteString(value);

        return value;
    }
}

std::string SqliteColumnType(ColumnModel const& column)
{
    std::string_view const base = TypeBase(column.type);
    switch (column.affinity)
    {
        case ColumnAffinity::Integer:
            if (base == "bit" || base == "bool" || base == "boolean" || base == "year" || base == "serial")
                return "INTEGER";
            return ToUpper(base) + (column.isUnsigned ? " UNSIGNED" : "");
        case ColumnAffinity::Real:
        case ColumnAffinity::Decimal:
        {
            std::string_view type = column.type;
            if (std::size_t pos = type.find(" unsigned"); pos != std::string_view::npos)
                type = type.substr(0, pos);
            return ToUpper(type);
        }
        case ColumnAffinity::Blob:
            if (base == "binary" || base == "varbinary")
                return "BLOB";
            return ToUpper(base);
        case ColumnAffinity::DateTime:
            return "TEXT";
        case ColumnAffinity::Text:
        default:
            if (base == "enum" || base == "set" || base == "json")
                return "TEXT";
            return ToUpper(column.type);
    }
}

bool SqliteIsRowidAlias(TableModel const& table, ColumnModel const& column)
{
    if (!column.autoIncrement || column.affinity != ColumnAffinity::Integer)
        return false;

    IndexModel const* pk = table.PrimaryKey();
    return pk && pk->columns.size() == 1 && IdentifierEquals(pk->columns.front(), column.name);
}

std::string SqliteColumnDefinition(ColumnModel const& column, bool rowidAlias)
{
    std::string def = SqliteQuoteIdentifier(column.name);
    if (rowidAlias)
        return def + " INTEGER PRIMARY KEY AUTOINCREMENT";

    def += " " + SqliteColumnType(column);
    if (column.notNull)
        def += " NOT NULL";

    bool const isEnum = TypeBase(column.type) == "enum" && !column.enumValues.empty();
    if (column.defaultValue)
        def += " DEFAULT " + SqliteDefault(column);
    else if (isEnum && column.notNull)
        def += " DEFAULT " + SqliteQuoteString(column.enumValues.front());

    if (column.caseInsensitive && column.affinity == ColumnAffinity::Text)
        def += " COLLATE NOCASE";

    if (isEnum)
    {
        def += " CHECK (" + SqliteQuoteIdentifier(column.name) + " IN (";
        for (std::size_t i = 0; i < column.enumValues.size(); ++i)
        {
            if (i)
                def += ", ";
            def += SqliteQuoteString(column.enumValues[i]);
        }
        def += "))";
    }

    return def;
}

std::string SqliteIndexName(std::string_view table, std::string_view index)
{
    return std::string(table) + "__" + std::string(index);
}

std::string SqliteCreateTable(TableModel const& table, std::string_view name, bool ifNotExists)
{
    std::string sql = "CREATE TABLE ";
    if (ifNotExists)
        sql += "IF NOT EXISTS ";
    sql += SqliteQuoteIdentifier(name) + " (";

    bool first = true;
    auto separator = [&]()
    {
        sql += first ? "\n  " : ",\n  ";
        first = false;
    };

    bool rowidAlias = false;
    for (ColumnModel const& column : table.columns)
    {
        bool const alias = SqliteIsRowidAlias(table, column);
        rowidAlias |= alias;
        separator();
        sql += SqliteColumnDefinition(column, alias);
    }

    if (IndexModel const* pk = table.PrimaryKey(); pk && !rowidAlias)
    {
        separator();
        sql += "PRIMARY KEY (";
        for (std::size_t i = 0; i < pk->columns.size(); ++i)
            sql += (i ? ", " : "") + SqliteQuoteIdentifier(pk->columns[i]);
        sql += ")";
    }

    for (ForeignKeyModel const& fk : table.foreignKeys)
    {
        separator();
        if (!fk.name.empty())
            sql += "CONSTRAINT " + SqliteQuoteIdentifier(fk.name) + " ";
        sql += "FOREIGN KEY (";
        for (std::size_t i = 0; i < fk.columns.size(); ++i)
            sql += (i ? ", " : "") + SqliteQuoteIdentifier(fk.columns[i]);
        sql += ") REFERENCES " + SqliteQuoteIdentifier(fk.refTable);
        if (!fk.refColumns.empty())
        {
            sql += " (";
            for (std::size_t i = 0; i < fk.refColumns.size(); ++i)
                sql += (i ? ", " : "") + SqliteQuoteIdentifier(fk.refColumns[i]);
            sql += ")";
        }
        if (!fk.onDelete.empty())
            sql += " ON DELETE " + fk.onDelete;
        if (!fk.onUpdate.empty())
            sql += " ON UPDATE " + fk.onUpdate;
    }

    for (std::string const& check : table.checks)
    {
        separator();
        sql += "CHECK " + MySqlExpressionToSqlite(check);
    }

    return sql + "\n)";
}

std::string SqliteCreateIndex(std::string_view table, IndexModel const& index, bool ifNotExists)
{
    std::string sql = index.kind == IndexKind::Unique ? "CREATE UNIQUE INDEX " : "CREATE INDEX ";
    if (ifNotExists)
        sql += "IF NOT EXISTS ";
    sql += SqliteQuoteIdentifier(SqliteIndexName(table, index.name)) + " ON " + SqliteQuoteIdentifier(table) + " (";
    for (std::size_t i = 0; i < index.columns.size(); ++i)
        sql += (i ? ", " : "") + SqliteQuoteIdentifier(index.columns[i]);
    return sql + ")";
}

std::string SqliteOnUpdateTriggerName(std::string_view table, std::string_view column)
{
    return std::string(table) + "__" + std::string(column) + "__on_update";
}

std::string SqliteCreateOnUpdateTrigger(std::string_view table, std::string_view column)
{
    std::string const t = SqliteQuoteIdentifier(table);
    std::string const c = SqliteQuoteIdentifier(column);
    return "CREATE TRIGGER " + SqliteQuoteIdentifier(SqliteOnUpdateTriggerName(table, column)) + " AFTER UPDATE ON " + t +
        " FOR EACH ROW WHEN NEW." + c + " IS OLD." + c + " BEGIN UPDATE " + t + " SET " + c + " = CURRENT_TIMESTAMP WHERE rowid = NEW.rowid; END";
}

std::vector<std::string> SqliteCreateTableScript(TableModel const& table, bool ifNotExists)
{
    std::vector<std::string> script;
    script.push_back(SqliteCreateTable(table, table.name, ifNotExists));

    for (IndexModel const& index : table.indexes)
        if (index.kind != IndexKind::Primary)
            script.push_back(SqliteCreateIndex(table.name, index, ifNotExists));

    bool rowidAlias = false;
    for (ColumnModel const& column : table.columns)
    {
        if (column.onUpdateCurrentTimestamp)
            script.push_back(SqliteCreateOnUpdateTrigger(table.name, column.name));
        rowidAlias |= SqliteIsRowidAlias(table, column);
    }

    if (rowidAlias && table.autoIncrement && *table.autoIncrement > 1)
    {
        std::string const name = SqliteQuoteString(table.name);
        script.push_back("DELETE FROM sqlite_sequence WHERE name = " + name);
        script.push_back("INSERT INTO sqlite_sequence (name, seq) VALUES (" + name + ", " + std::to_string(*table.autoIncrement - 1) + ")");
    }

    return script;
}

bool SqliteLoadTable(sqlite3* db, std::string_view table, TableModel& model, std::string& error)
{
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT type, name, sql FROM sqlite_schema WHERE tbl_name = ?1 COLLATE NOCASE AND sql IS NOT NULL "
        "ORDER BY type = 'table' DESC, rowid", -1, &stmt, nullptr) != SQLITE_OK)
    {
        error = sqlite3_errmsg(db);
        return false;
    }

    sqlite3_bind_text(stmt, 1, table.data(), int(table.size()), SQLITE_TRANSIENT);

    bool found = false;
    bool ok = true;
    int rc;
    while (ok && (rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        std::string_view const type = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
        std::string_view const name = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
        std::string_view const sql = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 2));

        if (type == "table")
        {
            bool ifNotExists = false;
            if (!ParseCreateTable(sql, model, ifNotExists, error, DdlSyntax::Sqlite))
            {
                error = "cannot read schema of table '" + std::string(name) + "': " + error;
                ok = false;
                break;
            }
            model.name = std::string(name);
            found = true;
        }
        else if (!found)
            break;
        else if (type == "index")
        {
            AlterTableModel alter;
            if (!ParseIndexStatement(sql, alter, error, DdlSyntax::Sqlite) || alter.ops.empty())
            {
                error = "cannot read index '" + std::string(name) + "': " + error;
                ok = false;
                break;
            }

            IndexModel index = std::move(alter.ops.front().index);
            std::string const prefix = model.name + "__";
            if (index.name.size() > prefix.size() && IdentifierEquals(std::string_view(index.name).substr(0, prefix.size()), prefix))
                index.name = index.name.substr(prefix.size());
            model.indexes.push_back(std::move(index));
        }
        else if (type == "trigger")
        {
            std::string const prefix = model.name + "__";
            std::string_view const suffix = "__on_update";
            if (name.size() > prefix.size() + suffix.size() && IdentifierEquals(name.substr(0, prefix.size()), prefix) &&
                name.substr(name.size() - suffix.size()) == suffix)
                if (ColumnModel* column = model.FindColumn(name.substr(prefix.size(), name.size() - prefix.size() - suffix.size())))
                    column->onUpdateCurrentTimestamp = true;
        }
    }

    sqlite3_finalize(stmt);
    return ok && found;
}
