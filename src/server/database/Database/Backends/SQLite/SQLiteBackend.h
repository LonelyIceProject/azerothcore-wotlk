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

#ifndef _SQLITEBACKEND_H
#define _SQLITEBACKEND_H

#include "IDbConnectionBackend.h"
#include <initializer_list>

struct sqlite3;

class SQLiteBackend : public IDbConnectionBackend
{
public:
    explicit SQLiteBackend(DatabaseConnectionInfo const& info);
    ~SQLiteBackend() override;

    SQLiteBackend(SQLiteBackend const&) = delete;
    SQLiteBackend& operator=(SQLiteBackend const&) = delete;

    DbError Open(bool create) override;
    void Close() override;

    std::unique_ptr<IDbStatement> Prepare(std::string_view sql, DbError& err) override;
    bool Execute(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err) override;
    std::unique_ptr<RowSet> Query(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err) override;

    bool Execute(std::string_view sql, DbError& err) override;
    std::unique_ptr<RowSet> Query(std::string_view sql, DbError& err) override;

    bool Begin(DbError& err) override;
    bool Commit(DbError& err) override;
    void Rollback() override;

    bool HasAnyTable() override;
    bool TableExists(std::string_view table) override;
    std::vector<std::string> ListColumns(std::string_view table) override;

    std::unique_ptr<IScriptTarget> CreateScriptTarget() override;

    [[nodiscard]] std::string ServerInfo() const override;
    [[nodiscard]] DatabaseBackend Backend() const override { return DatabaseBackend::SQLite; }

private:
    DbError Configure();
    DbError Attach(std::string const& alias, std::string const& path);
    // Runs every statement in sql; result, if given, receives the rows of the last statement that returns columns.
    bool Run(std::string_view sql, std::unique_ptr<RowSet>* result, DbError& err);
    std::unique_ptr<RowSet> QueryText(std::string_view sql, std::initializer_list<std::string_view> params);
    bool IsOpen(DbError& err) const;

    DatabaseConnectionInfo _info;
    sqlite3* _db;
};

#endif
