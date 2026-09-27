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

#include "SqliteScriptTarget.h"
#include "SchemaDdl.h"
#include <sqlite3.h>

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

    bool IsSchemaStatement(std::string_view sql)
    {
        std::size_t i = 0;
        while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\r' || sql[i] == '\n'))
            ++i;
        std::string_view word = sql.substr(i, 6);
        return IdentifierEquals(word, "create") || IdentifierEquals(word.substr(0, 4), "drop") || IdentifierEquals(word.substr(0, 5), "alter");
    }

    bool BindValue(sqlite3_stmt* stmt, int index, ScriptValue const& value)
    {
        int rc = SQLITE_OK;
        std::visit([&](auto const& v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>)
                rc = sqlite3_bind_null(stmt, index);
            else if constexpr (std::is_same_v<T, int64>)
                rc = sqlite3_bind_int64(stmt, index, v);
            else if constexpr (std::is_same_v<T, double>)
                rc = sqlite3_bind_double(stmt, index, v);
            else if constexpr (std::is_same_v<T, std::string>)
                rc = sqlite3_bind_text64(stmt, index, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8);
            else if (v.empty())
                rc = sqlite3_bind_zeroblob(stmt, index, 0);
            else
                rc = sqlite3_bind_blob64(stmt, index, v.data(), v.size(), SQLITE_TRANSIENT);
        }, value);
        return rc == SQLITE_OK;
    }

    ScriptValue ColumnValue(sqlite3_stmt* stmt, int column)
    {
        switch (sqlite3_column_type(stmt, column))
        {
            case SQLITE_INTEGER:
                return int64(sqlite3_column_int64(stmt, column));
            case SQLITE_FLOAT:
                return sqlite3_column_double(stmt, column);
            case SQLITE_TEXT:
                return std::string(reinterpret_cast<char const*>(sqlite3_column_text(stmt, column)), std::size_t(sqlite3_column_bytes(stmt, column)));
            case SQLITE_BLOB:
            {
                uint8 const* data = static_cast<uint8 const*>(sqlite3_column_blob(stmt, column));
                return std::vector<uint8>(data, data + sqlite3_column_bytes(stmt, column));
            }
            default:
                return nullptr;
        }
    }
}

DbError SqliteMakeError(sqlite3* db, int rc)
{
    DbError err;
    err.native = db ? sqlite3_extended_errcode(db) : rc;
    err.message = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);

    switch (rc & 0xFF)
    {
        case SQLITE_CONSTRAINT:
            err.cls = DbErrorClass::Constraint;
            break;
        case SQLITE_BUSY:
        case SQLITE_LOCKED:
            err.cls = DbErrorClass::Retryable;
            break;
        case SQLITE_ERROR:
            err.cls = (err.message.starts_with("no such table") || err.message.starts_with("no such column") || err.message.find("has no column named") != std::string::npos)
                ? DbErrorClass::SchemaMismatch : DbErrorClass::Syntax;
            break;
        case SQLITE_CANTOPEN:
            err.cls = DbErrorClass::DatabaseMissing;
            break;
        default:
            err.cls = DbErrorClass::Other;
            break;
    }
    return err;
}

bool SqliteExec(sqlite3* db, std::string_view sql, DbError& err)
{
    char const* pos = sql.data();
    char const* end = sql.data() + sql.size();
    while (pos < end)
    {
        sqlite3_stmt* stmt = nullptr;
        char const* tail = nullptr;
        int rc = sqlite3_prepare_v2(db, pos, int(end - pos), &stmt, &tail);
        if (rc != SQLITE_OK)
        {
            err = SqliteMakeError(db, rc);
            return false;
        }

        pos = tail;
        if (!stmt)
            continue;

        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW);
        if (rc != SQLITE_DONE)
        {
            err = SqliteMakeError(db, rc);
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);
    }
    return true;
}

SqliteScriptTarget::SqliteScriptTarget(sqlite3* db) : _db(db) { }

SqliteScriptTarget::~SqliteScriptTarget()
{
    InvalidateSchema();
}

void SqliteScriptTarget::InvalidateSchema()
{
    for (auto& [key, stmt] : _inserts)
        sqlite3_finalize(stmt);
    _inserts.clear();
    _tables.clear();
}

bool SqliteScriptTarget::Exec(std::string_view sql, DbError& err)
{
    bool const ok = SqliteExec(_db, sql, err);
    if (IsSchemaStatement(sql))
        InvalidateSchema();
    return ok;
}

bool SqliteScriptTarget::Scalar(std::string_view sql, ScriptValue& value, DbError& err)
{
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(_db, sql.data(), int(sql.size()), &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        err = SqliteMakeError(_db, rc);
        return false;
    }

    value = nullptr;
    if (!stmt)
        return true;

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW)
    {
        value = ColumnValue(stmt, 0);
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW);
    }

    if (rc != SQLITE_DONE)
    {
        err = SqliteMakeError(_db, rc);
        sqlite3_finalize(stmt);
        return false;
    }

    sqlite3_finalize(stmt);
    return true;
}

bool SqliteScriptTarget::BulkInsert(std::string_view table, std::vector<std::string> const& columns, std::span<ScriptRow const> rows, BulkInsertMode mode, DbError& err)
{
    std::vector<std::string> names = columns;
    if (names.empty())
    {
        TableModel const* model = Find(table);
        if (!model)
        {
            err = { DbErrorClass::SchemaMismatch, 0, "no such table: " + std::string(table) };
            return false;
        }
        names = model->ColumnNames();
    }

    std::string key = std::to_string(int(mode)) + ":" + Lower(table);
    for (std::string const& name : names)
        key += "," + name;

    sqlite3_stmt*& stmt = _inserts[key];
    if (!stmt)
    {
        std::string sql = mode == BulkInsertMode::InsertIgnore ? "INSERT OR IGNORE INTO " : mode == BulkInsertMode::Replace ? "REPLACE INTO " : "INSERT INTO ";
        sql += SqliteQuoteIdentifier(table) + " (";
        for (std::size_t i = 0; i < names.size(); ++i)
            sql += (i ? ", " : "") + SqliteQuoteIdentifier(names[i]);
        sql += ") VALUES (";
        for (std::size_t i = 0; i < names.size(); ++i)
            sql += i ? ", ?" : "?";
        sql += ")";

        int rc = sqlite3_prepare_v3(_db, sql.c_str(), int(sql.size()), SQLITE_PREPARE_PERSISTENT, &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            err = SqliteMakeError(_db, rc);
            _inserts.erase(key);
            return false;
        }
    }

    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        ScriptRow const& row = rows[r];
        if (row.size() != names.size())
        {
            err = { DbErrorClass::Syntax, 0, "Column count doesn't match value count at row " + std::to_string(r + 1) };
            return false;
        }

        for (std::size_t i = 0; i < row.size(); ++i)
        {
            if (!BindValue(stmt, int(i + 1), row[i]))
            {
                err = SqliteMakeError(_db, sqlite3_errcode(_db));
                sqlite3_reset(stmt);
                sqlite3_clear_bindings(stmt);
                return false;
            }
        }

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE)
        {
            err = SqliteMakeError(_db, rc);
            if (rows.size() > 1)
                err.message += " (row " + std::to_string(r + 1) + ")";
            sqlite3_reset(stmt);
            sqlite3_clear_bindings(stmt);
            return false;
        }

        sqlite3_reset(stmt);
    }

    sqlite3_clear_bindings(stmt);
    return true;
}

TableModel const* SqliteScriptTarget::Find(std::string_view table) const
{
    std::string key = Lower(table);
    auto itr = _tables.find(key);
    if (itr != _tables.end())
        return itr->second.get();

    auto model = std::make_unique<TableModel>();
    std::string error;
    if (!SqliteLoadTable(_db, table, *model, error))
        return nullptr;

    return (_tables[key] = std::move(model)).get();
}

bool SqliteScriptTarget::LoadTable(std::string_view table, TableModel& model)
{
    TableModel const* found = Find(table);
    if (!found)
        return false;
    model = *found;
    return true;
}

bool SqliteScriptTarget::CreateTable(TableModel const& table, bool ifNotExists, DbError& err)
{
    if (Find(table.name))
    {
        if (ifNotExists)
            return true;
        err = { DbErrorClass::Syntax, 0, "Table '" + table.name + "' already exists" };
        return false;
    }

    InvalidateSchema();
    for (std::string const& sql : SqliteCreateTableScript(table, false))
        if (!SqliteExec(_db, sql, err))
            return false;

    return true;
}

bool SqliteScriptTarget::AlterTable(AlterTableModel const& alter, DbError& err)
{
    InvalidateSchema();
    bool const ok = SqliteAlterTable(_db, alter, err);
    InvalidateSchema();
    return ok;
}

bool SqliteScriptTarget::DropTable(std::string_view table, bool ifExists, DbError& err)
{
    InvalidateSchema();
    return SqliteExec(_db, std::string(ifExists ? "DROP TABLE IF EXISTS " : "DROP TABLE ") + SqliteQuoteIdentifier(table), err);
}

bool SqliteScriptTarget::Begin(DbError& err)
{
    return SqliteExec(_db, "BEGIN IMMEDIATE", err);
}

bool SqliteScriptTarget::Commit(DbError& err)
{
    return SqliteExec(_db, "COMMIT", err);
}

void SqliteScriptTarget::Rollback()
{
    if (sqlite3_get_autocommit(_db))
        return;

    DbError err;
    SqliteExec(_db, "ROLLBACK", err);
    InvalidateSchema();
}

bool SqliteScriptTarget::SetForeignKeys(bool enabled, DbError& err)
{
    return SqliteExec(_db, enabled ? "PRAGMA foreign_keys = ON" : "PRAGMA foreign_keys = OFF", err);
}

bool SqliteScriptTarget::CheckForeignKeys(DbError& err)
{
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(_db, "PRAGMA foreign_key_check", -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
    {
        err = SqliteMakeError(_db, rc);
        return false;
    }

    std::string violations;
    uint32 count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    {
        if (++count <= 10)
        {
            if (!violations.empty())
                violations += "; ";
            violations += reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
            violations += " rowid " + std::to_string(sqlite3_column_int64(stmt, 1)) + " -> ";
            violations += reinterpret_cast<char const*>(sqlite3_column_text(stmt, 2));
        }
    }

    if (rc != SQLITE_DONE)
    {
        err = SqliteMakeError(_db, rc);
        sqlite3_finalize(stmt);
        return false;
    }
    sqlite3_finalize(stmt);

    if (!count)
        return true;

    err = { DbErrorClass::Constraint, SQLITE_CONSTRAINT_FOREIGNKEY, "FOREIGN KEY constraint failed: " + std::to_string(count) + " violation(s): " + violations };
    return false;
}
