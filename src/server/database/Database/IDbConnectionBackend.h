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

#ifndef _IDBCONNECTIONBACKEND_H
#define _IDBCONNECTIONBACKEND_H

#include "DatabaseBackend.h"
#include "DatabaseConnectionInfo.h"
#include "PreparedStatement.h"
#include "RowSet.h"
#include "ScriptRunner.h"
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

class IDbStatement
{
public:
    virtual ~IDbStatement() = default;
    [[nodiscard]] virtual uint32 GetParameterCount() const = 0;
};

// One physical connection. SQL passed in is already in the backend's own dialect
// (DatabaseConnection translates through SqlDialect before calling in).
// Query() returns an empty RowSet when no rows matched and nullptr only on error.
class IDbConnectionBackend
{
public:
    virtual ~IDbConnectionBackend() = default;

    virtual DbError Open(bool create) = 0;
    virtual void Close() = 0;
    virtual bool Reconnect(DbError& err) { err = {}; return false; }
    virtual void Ping() { }

    virtual std::unique_ptr<IDbStatement> Prepare(std::string_view sql, DbError& err) = 0;
    virtual bool Execute(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err) = 0;
    virtual std::unique_ptr<RowSet> Query(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err) = 0;

    virtual bool Execute(std::string_view sql, DbError& err) = 0;
    virtual std::unique_ptr<RowSet> Query(std::string_view sql, DbError& err) = 0;

    virtual bool Begin(DbError& err) = 0;
    virtual bool Commit(DbError& err) = 0;
    virtual void Rollback() = 0;

    virtual bool HasAnyTable() = 0;
    virtual bool TableExists(std::string_view table) = 0;
    virtual std::vector<std::string> ListColumns(std::string_view table) = 0;

    // Target for ScriptRunner on this connection; nullptr when the backend applies files externally (MySQL CLI).
    virtual std::unique_ptr<IScriptTarget> CreateScriptTarget() { return nullptr; }

    [[nodiscard]] virtual std::string ServerInfo() const = 0;
    [[nodiscard]] virtual DatabaseBackend Backend() const = 0;
};

AC_DATABASE_API std::unique_ptr<IDbConnectionBackend> CreateBackend(DatabaseConnectionInfo const& info);
AC_DATABASE_API DbBackendCaps GetBackendCaps(DatabaseBackend backend);

std::unique_ptr<IDbConnectionBackend> CreateMySQLBackend(DatabaseConnectionInfo const& info);
std::unique_ptr<IDbConnectionBackend> CreateSQLiteBackend(DatabaseConnectionInfo const& info);

#endif
