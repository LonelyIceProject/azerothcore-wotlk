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

#include "SQLiteStatement.h"
#include "StringFormat.h"
#include <sqlite3.h>
#include <cctype>

namespace
{
    DatabaseFieldTypes DeclTypeToFieldType(std::string_view declType)
    {
        std::string type(declType);
        for (char& c : type)
            c = char(std::toupper(uint8(c)));

        std::string_view view(type);
        if (view.starts_with("TINYINT") || view.starts_with("BOOL"))
            return DatabaseFieldTypes::Int8;
        if (view.starts_with("SMALLINT") || view.starts_with("YEAR"))
            return DatabaseFieldTypes::Int16;
        if (view.starts_with("BIGINT") || view.starts_with("BIT"))
            return DatabaseFieldTypes::Int64;
        if (view.starts_with("FLOAT"))
            return DatabaseFieldTypes::Float;
        if (view.starts_with("DOUBLE") || view.starts_with("REAL"))
            return DatabaseFieldTypes::Double;
        if (view.starts_with("DECIMAL") || view.starts_with("NUMERIC"))
            return DatabaseFieldTypes::Decimal;
        if (view.find("INT") != std::string_view::npos)
            return DatabaseFieldTypes::Int32;

        return DatabaseFieldTypes::Binary;
    }

    void DescribeColumn(sqlite3_stmt* stmt, int index, QueryResultFieldMetadata& meta)
    {
        char const* declType = sqlite3_column_decltype(stmt, index);
        char const* table = sqlite3_column_table_name(stmt, index);
        char const* origin = sqlite3_column_origin_name(stmt, index);
        char const* name = sqlite3_column_name(stmt, index);

        meta.TableName = table ? table : "";
        meta.TableAlias = meta.TableName;
        meta.Alias = name ? name : "";
        meta.Name = origin ? origin : meta.Alias;
        meta.TypeName = declType ? declType : "";
        meta.Index = uint32(index);
        meta.Type = declType ? DeclTypeToFieldType(declType) : DatabaseFieldTypes::Null;
    }

    // Expression columns have no declared type, take it from the first non-null value.
    void ResolveUntypedColumns(RowSet& result)
    {
        uint64 const rowCount = result.GetRowCount();
        for (uint32 column = 0; column < result.GetFieldCount(); ++column)
        {
            QueryResultFieldMetadata& meta = result.GetColumn(column);
            if (!meta.TypeName.empty())
                continue;

            meta.TypeName = "NULL";
            for (uint64 row = 0; row < rowCount; ++row)
            {
                FieldValueType const type = result.Get(row, column).Type;
                if (type == FieldValueType::Null)
                    continue;

                switch (type)
                {
                    case FieldValueType::Int:
                        meta.Type = DatabaseFieldTypes::Int64;
                        meta.TypeName = "INTEGER";
                        break;
                    case FieldValueType::Real:
                        meta.Type = DatabaseFieldTypes::Double;
                        meta.TypeName = "REAL";
                        break;
                    case FieldValueType::Text:
                        meta.Type = DatabaseFieldTypes::Binary;
                        meta.TypeName = "TEXT";
                        break;
                    default:
                        meta.Type = DatabaseFieldTypes::Binary;
                        meta.TypeName = "BLOB";
                        break;
                }
                break;
            }
        }
    }

    void ReadValue(RowSet& result, sqlite3_stmt* stmt, int index, FieldValue& cell)
    {
        switch (sqlite3_column_type(stmt, index))
        {
            case SQLITE_INTEGER:
                cell.SetInt(sqlite3_column_int64(stmt, index));
                break;
            case SQLITE_FLOAT:
                cell.SetReal(sqlite3_column_double(stmt, index));
                break;
            case SQLITE_TEXT:
            {
                char const* text = reinterpret_cast<char const*>(sqlite3_column_text(stmt, index));
                int const size = sqlite3_column_bytes(stmt, index);
                result.SetText(cell, text ? std::string_view(text, std::size_t(size)) : std::string_view());
                break;
            }
            case SQLITE_BLOB:
            {
                void const* data = sqlite3_column_blob(stmt, index);
                int const size = sqlite3_column_bytes(stmt, index);
                result.SetBlob(cell, data, data ? std::size_t(size) : 0);
                break;
            }
            default:
                break;
        }
    }

    template <typename T>
    int BindValue(sqlite3_stmt* stmt, int index, T const& value)
    {
        if constexpr (std::is_same_v<T, std::nullptr_t>)
            return sqlite3_bind_null(stmt, index);
        else if constexpr (std::is_same_v<T, std::string>)
            return sqlite3_bind_text(stmt, index, value.data(), int(value.size()), SQLITE_TRANSIENT);
        else if constexpr (std::is_same_v<T, std::vector<uint8>>)
        {
            if (value.empty())
                return sqlite3_bind_zeroblob(stmt, index, 0);

            return sqlite3_bind_blob(stmt, index, value.data(), int(value.size()), SQLITE_TRANSIENT);
        }
        else if constexpr (std::is_floating_point_v<T>)
            return sqlite3_bind_double(stmt, index, double(value));
        else
            return sqlite3_bind_int64(stmt, index, static_cast<sqlite3_int64>(value));
    }
}

SQLiteStatement::SQLiteStatement(sqlite3_stmt* stmt) :
    _stmt(stmt),
    _parameterCount(uint32(sqlite3_bind_parameter_count(stmt)))
{
}

SQLiteStatement::~SQLiteStatement()
{
    sqlite3_finalize(_stmt);
}

DbErrorClass SQLite::ClassifyError(int code, std::string_view message)
{
    switch (code & 0xFF)
    {
        case SQLITE_OK:
        case SQLITE_ROW:
        case SQLITE_DONE:
            return DbErrorClass::None;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
        case SQLITE_SCHEMA:
            return DbErrorClass::Retryable;
        case SQLITE_CONSTRAINT:
        case SQLITE_MISMATCH:
        case SQLITE_TOOBIG:
            return DbErrorClass::Constraint;
        case SQLITE_ERROR:
            if (message.find("no such table") != std::string_view::npos
                || message.find("no such column") != std::string_view::npos
                || message.find("has no column named") != std::string_view::npos)
                return DbErrorClass::SchemaMismatch;
            return DbErrorClass::Syntax;
        case SQLITE_RANGE:
            return DbErrorClass::Syntax;
        default:
            return DbErrorClass::Other;
    }
}

DbError SQLite::MakeError(sqlite3* db, int code)
{
    DbError err;
    err.native = code;
    err.message = db ? sqlite3_errmsg(db) : sqlite3_errstr(code);
    err.cls = ClassifyError(code, err.message);
    if (err.cls == DbErrorClass::None)
        err.cls = DbErrorClass::Other;

    return err;
}

bool SQLite::BindParameters(sqlite3_stmt* stmt, std::span<PreparedStatementData const> params, DbError& err)
{
    int const count = sqlite3_bind_parameter_count(stmt);
    if (params.size() != std::size_t(count))
    {
        err = { DbErrorClass::Syntax, SQLITE_RANGE, Acore::StringFormat("Statement expects {} parameters, {} given", count, params.size()) };
        return false;
    }

    for (int i = 0; i < count; ++i)
    {
        int const rc = std::visit([stmt, i](auto const& value) { return BindValue(stmt, i + 1, value); }, params[i].data);
        if (rc != SQLITE_OK)
        {
            err = MakeError(sqlite3_db_handle(stmt), rc);
            return false;
        }
    }

    return true;
}

bool SQLite::Run(sqlite3_stmt* stmt, DbError& err)
{
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) { }

    if (rc != SQLITE_DONE)
    {
        err = MakeError(sqlite3_db_handle(stmt), rc);
        return false;
    }

    return true;
}

std::unique_ptr<RowSet> SQLite::Fetch(sqlite3_stmt* stmt, DbError& err)
{
    int const columnCount = sqlite3_column_count(stmt);

    std::vector<QueryResultFieldMetadata> columns(static_cast<std::size_t>(columnCount));
    for (int i = 0; i < columnCount; ++i)
        DescribeColumn(stmt, i, columns[i]);

    std::unique_ptr<RowSet> result = std::make_unique<RowSet>(std::move(columns));

    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        FieldValue* row = result->AppendRow();
        for (int i = 0; i < columnCount; ++i)
            ReadValue(*result, stmt, i, row[i]);
    }

    if (rc != SQLITE_DONE)
    {
        err = MakeError(sqlite3_db_handle(stmt), rc);
        return nullptr;
    }

    ResolveUntypedColumns(*result);
    return result;
}

std::string SQLite::QuoteIdentifier(std::string_view identifier)
{
    std::string quoted;
    quoted.reserve(identifier.size() + 2);
    quoted += '"';
    for (char c : identifier)
    {
        if (c == '"')
            quoted += '"';
        quoted += c;
    }
    quoted += '"';
    return quoted;
}
