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

#ifndef _MYSQLBACKEND_H
#define _MYSQLBACKEND_H

#include "IDbConnectionBackend.h"
#include "MySQLHacks.h"

class MySQLBackend final : public IDbConnectionBackend
{
public:
    explicit MySQLBackend(DatabaseConnectionInfo const& info);
    ~MySQLBackend() override;

    DbError Open(bool create) override;
    void Close() override;
    bool Reconnect(DbError& err) override;
    void Ping() override;

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

    [[nodiscard]] std::string ServerInfo() const override;
    [[nodiscard]] DatabaseBackend Backend() const override { return DatabaseBackend::MySQL; }

private:
    bool ExecuteStatement(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err);
    DbError LastError() const;

    DatabaseConnectionInfo _info;
    MYSQL* _mysql;
    bool _connectedBefore;
};

DbError MakeMySQLError(uint32 errNo, char const* message);

namespace MySQLLibrary
{
    void Init();
    void End();
    std::string Version();
}

#endif
