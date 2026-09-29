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

#include "MySQLScriptTarget.h"
#include "IDbConnectionBackend.h"
#include "RowSet.h"

namespace
{
    bool NotUsed(DbError& err)
    {
        err = { DbErrorClass::Other, 0, "the MySQL script target runs statements as written" };
        return false;
    }
}

bool MySQLScriptTarget::Exec(std::string_view sql, DbError& err)
{
    // The updater asks for fresh statistics after populating; MySQL keeps its own.
    if (sql == "ANALYZE")
        return true;

    return _connection.Execute(sql, err);
}

bool MySQLScriptTarget::Scalar(std::string_view sql, ScriptValue& value, DbError& err)
{
    std::unique_ptr<RowSet> rows = _connection.Query(sql, err);
    if (!rows)
        return false;

    value = nullptr;
    if (!rows->GetRowCount() || rows->GetColumns().empty())
        return true;

    FieldValue const& field = rows->Get(0, 0);
    switch (field.Type)
    {
        case FieldValueType::Int:  value = field.Int; break;
        case FieldValueType::Real: value = field.Real; break;
        case FieldValueType::Text: value = std::string(field.AsBytes()); break;
        case FieldValueType::Blob:
        {
            std::string_view const bytes = field.AsBytes();
            value = std::vector<uint8>(bytes.begin(), bytes.end());
            break;
        }
        default: break;
    }
    return true;
}

bool MySQLScriptTarget::BulkInsert(std::string_view /*table*/, std::vector<std::string> const& /*columns*/, std::span<ScriptRow const> /*rows*/,
    BulkInsertMode /*mode*/, DbError& err)
{
    return NotUsed(err);
}

bool MySQLScriptTarget::LoadTable(std::string_view /*table*/, TableModel& /*model*/)
{
    return false;
}

bool MySQLScriptTarget::CreateTable(TableModel const& /*table*/, bool /*ifNotExists*/, DbError& err)
{
    return NotUsed(err);
}

bool MySQLScriptTarget::AlterTable(AlterTableModel const& /*alter*/, DbError& err)
{
    return NotUsed(err);
}

bool MySQLScriptTarget::DropTable(std::string_view /*table*/, bool /*ifExists*/, DbError& err)
{
    return NotUsed(err);
}

bool MySQLScriptTarget::Begin(DbError& err)
{
    return _connection.Begin(err);
}

bool MySQLScriptTarget::Commit(DbError& err)
{
    return _connection.Commit(err);
}

void MySQLScriptTarget::Rollback()
{
    _connection.Rollback();
}

bool MySQLScriptTarget::SetForeignKeys(bool enabled, DbError& err)
{
    return _connection.Execute(enabled ? "SET FOREIGN_KEY_CHECKS = 1" : "SET FOREIGN_KEY_CHECKS = 0", err);
}

// Rows written with the checks off are not checked again, as with the mysql client.
bool MySQLScriptTarget::CheckForeignKeys(DbError& /*err*/)
{
    return true;
}
