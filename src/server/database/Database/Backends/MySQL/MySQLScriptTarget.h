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

#ifndef _MYSQLSCRIPTTARGET_H
#define _MYSQLSCRIPTTARGET_H

#include "ScriptRunner.h"

class IDbConnectionBackend;

// Applies sql files over a MySQL connection instead of the mysql command line client: the files are MySQL
// already, so every statement is sent as written. The schema model operations are not used in that mode.
class MySQLScriptTarget final : public IScriptTarget
{
public:
    explicit MySQLScriptTarget(IDbConnectionBackend& connection) : _connection(connection) { }

    [[nodiscard]] DatabaseBackend Backend() const override { return DatabaseBackend::MySQL; }
    [[nodiscard]] bool RunsMySqlAsIs() const override { return true; }

    bool Exec(std::string_view sql, DbError& err) override;
    bool Scalar(std::string_view sql, ScriptValue& value, DbError& err) override;
    bool BulkInsert(std::string_view table, std::vector<std::string> const& columns, std::span<ScriptRow const> rows, BulkInsertMode mode, DbError& err) override;

    bool LoadTable(std::string_view table, TableModel& model) override;
    bool CreateTable(TableModel const& table, bool ifNotExists, DbError& err) override;
    bool AlterTable(AlterTableModel const& alter, DbError& err) override;
    bool DropTable(std::string_view table, bool ifExists, DbError& err) override;

    bool Begin(DbError& err) override;
    bool Commit(DbError& err) override;
    void Rollback() override;
    bool SetForeignKeys(bool enabled, DbError& err) override;
    bool CheckForeignKeys(DbError& err) override;

private:
    IDbConnectionBackend& _connection;
};

#endif
