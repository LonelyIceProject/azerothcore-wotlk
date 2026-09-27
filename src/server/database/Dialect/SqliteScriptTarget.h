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

#ifndef _SQLITESCRIPTTARGET_H
#define _SQLITESCRIPTTARGET_H

#include "ScriptRunner.h"
#include "SqlDialect.h"
#include <memory>
#include <string>
#include <unordered_map>

struct sqlite3;
struct sqlite3_stmt;

DbError SqliteMakeError(sqlite3* db, int rc);
// Runs every statement of sql, discarding rows.
bool SqliteExec(sqlite3* db, std::string_view sql, DbError& err);

// IScriptTarget over a caller-owned connection.
class SqliteScriptTarget final : public IScriptTarget, public ISchemaLookup
{
public:
    explicit SqliteScriptTarget(sqlite3* db);
    ~SqliteScriptTarget() override;

    SqliteScriptTarget(SqliteScriptTarget const&) = delete;
    SqliteScriptTarget& operator=(SqliteScriptTarget const&) = delete;

    [[nodiscard]] DatabaseBackend Backend() const override { return DatabaseBackend::SQLite; }

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

    [[nodiscard]] ISchemaLookup const* Schema() const override { return this; }
    TableModel const* Find(std::string_view table) const override;

    [[nodiscard]] sqlite3* Handle() const { return _db; }

private:
    void InvalidateSchema();

    sqlite3* _db;
    mutable std::unordered_map<std::string, std::unique_ptr<TableModel>> _tables;
    std::unordered_map<std::string, sqlite3_stmt*> _inserts;
};

#endif
