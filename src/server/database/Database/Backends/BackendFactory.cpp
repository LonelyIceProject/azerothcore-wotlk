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

#include "IDbConnectionBackend.h"
#include "Log.h"

std::unique_ptr<IDbConnectionBackend> CreateBackend(DatabaseConnectionInfo const& info)
{
    switch (info.backend)
    {
#ifdef ACORE_WITH_MYSQL
        case DatabaseBackend::MySQL:
            return CreateMySQLBackend(info);
#endif
#ifdef ACORE_WITH_SQLITE
        case DatabaseBackend::SQLite:
            return CreateSQLiteBackend(info);
#endif
        default:
            break;
    }

    LOG_ERROR("sql.driver", "Database backend '{}' is not available in this build (database '{}')",
        DatabaseBackendName(info.backend), info.database);
    return nullptr;
}

DbBackendCaps GetBackendCaps(DatabaseBackend backend)
{
    switch (backend)
    {
        case DatabaseBackend::SQLite:
            return { 1, false, false };
        case DatabaseBackend::MySQL:
        case DatabaseBackend::PostgreSQL:
        default:
            return { 0, true, true };
    }
}
