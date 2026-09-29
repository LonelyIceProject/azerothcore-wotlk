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

#ifndef _DATABASEBACKEND_H
#define _DATABASEBACKEND_H

#include "Define.h"
#include <string>
#include <string_view>

enum class DatabaseBackend : uint8
{
    MySQL,
    SQLite,
    PostgreSQL
};

// Also the directory name under data/sql/overrides/
constexpr std::string_view DatabaseBackendName(DatabaseBackend backend)
{
    switch (backend)
    {
        case DatabaseBackend::MySQL:      return "mysql";
        case DatabaseBackend::SQLite:     return "sqlite";
        case DatabaseBackend::PostgreSQL: return "pgsql";
    }
    return "unknown";
}

enum class DbErrorClass : uint8
{
    None,
    ConnectionLost,
    DatabaseMissing,
    Retryable,
    Constraint,
    SchemaMismatch,
    Syntax,
    Other
};

struct DbError
{
    DbErrorClass cls = DbErrorClass::None;
    int32 native = 0;
    std::string message;

    [[nodiscard]] bool IsError() const { return cls != DbErrorClass::None; }
};

struct DbBackendCaps
{
    uint8 maxAsyncWorkers = 0;      // 0 = unlimited
    bool needsKeepAlive = false;
    bool supportsReconnect = false;
    bool externalScripts = false;   // sql files are applied by an external client (mysql CLI) instead of the connection
};

#endif
