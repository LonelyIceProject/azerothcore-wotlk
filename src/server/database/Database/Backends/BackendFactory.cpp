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

#include "BackendRegistry.h"
#include "IDbConnectionBackend.h"
#include "Log.h"
#include <array>
#include <mutex>
#include <optional>

#ifdef ACORE_WITH_MYSQL
#include "MySQLBackend.h"
#endif

#ifdef ACORE_WITH_SQLITE
#include <sqlite3.h>
#endif

namespace
{
    constexpr std::size_t BackendCount = std::size_t(DatabaseBackend::PostgreSQL) + 1;

    struct Registry
    {
        std::mutex lock;
        std::array<std::optional<DbBackendDriver>, BackendCount> drivers;
        bool initialized = false;

        Registry()
        {
#ifdef ACORE_WITH_MYSQL
            DbBackendDriver mysql;
            mysql.create = &CreateMySQLBackend;
            mysql.caps = { 0, true, true, true };
            mysql.init = &MySQLLibrary::Init;
            mysql.end = &MySQLLibrary::End;
            mysql.version = &MySQLLibrary::Version;
            drivers[std::size_t(DatabaseBackend::MySQL)] = mysql;
#endif
#ifdef ACORE_WITH_SQLITE
            DbBackendDriver sqlite;
            sqlite.create = &CreateSQLiteBackend;
            sqlite.caps = { 1, false, false, false };
            sqlite.init = [] { sqlite3_initialize(); };
            sqlite.version = [] { return std::string("SQLite ") + sqlite3_libversion(); };
            drivers[std::size_t(DatabaseBackend::SQLite)] = sqlite;
#endif
        }
    };

    Registry& GetRegistry()
    {
        static Registry registry;
        return registry;
    }

    std::optional<DbBackendDriver> Find(DatabaseBackend backend)
    {
        Registry& registry = GetRegistry();
        std::lock_guard guard(registry.lock);
        return registry.drivers[std::size_t(backend)];
    }
}

void RegisterBackendDriver(DatabaseBackend backend, DbBackendDriver const& driver)
{
    Registry& registry = GetRegistry();
    bool initNow = false;
    {
        std::lock_guard guard(registry.lock);
        registry.drivers[std::size_t(backend)] = driver;
        initNow = registry.initialized;
    }

    if (initNow && driver.init)
        driver.init();

    LOG_INFO("sql.driver", "Database backend '{}' registered", DatabaseBackendName(backend));
}

void UnregisterBackendDriver(DatabaseBackend backend)
{
    std::optional<DbBackendDriver> driver;
    Registry& registry = GetRegistry();
    {
        std::lock_guard guard(registry.lock);
        driver.swap(registry.drivers[std::size_t(backend)]);
        if (!registry.initialized)
            driver.reset();
    }

    if (driver && driver->end)
        driver->end();
}

bool IsBackendAvailable(DatabaseBackend backend)
{
    std::optional<DbBackendDriver> driver = Find(backend);
    return driver && driver->create;
}

std::unique_ptr<IDbConnectionBackend> CreateBackend(DatabaseConnectionInfo const& info)
{
    if (std::optional<DbBackendDriver> driver = Find(info.backend); driver && driver->create)
        return driver->create(info);

    LOG_ERROR("sql.driver", "Database backend '{}' is not available in this build (database '{}')",
        DatabaseBackendName(info.backend), info.database);
    return nullptr;
}

DbBackendCaps GetBackendCaps(DatabaseBackend backend)
{
    if (std::optional<DbBackendDriver> driver = Find(backend))
        return driver->caps;

    return { 0, true, true, false };
}

void BackendRegistry::InitAll()
{
    std::array<std::optional<DbBackendDriver>, BackendCount> drivers;
    Registry& registry = GetRegistry();
    {
        std::lock_guard guard(registry.lock);
        if (registry.initialized)
            return;

        registry.initialized = true;
        drivers = registry.drivers;
    }

    for (std::optional<DbBackendDriver> const& driver : drivers)
        if (driver && driver->init)
            driver->init();
}

void BackendRegistry::EndAll()
{
    std::array<std::optional<DbBackendDriver>, BackendCount> drivers;
    Registry& registry = GetRegistry();
    {
        std::lock_guard guard(registry.lock);
        if (!registry.initialized)
            return;

        registry.initialized = false;
        drivers = registry.drivers;
    }

    for (std::optional<DbBackendDriver> const& driver : drivers)
        if (driver && driver->end)
            driver->end();
}

std::string BackendRegistry::Versions()
{
    std::array<std::optional<DbBackendDriver>, BackendCount> drivers;
    {
        Registry& registry = GetRegistry();
        std::lock_guard guard(registry.lock);
        drivers = registry.drivers;
    }

    std::string versions;
    for (std::optional<DbBackendDriver> const& driver : drivers)
    {
        if (!driver || !driver->version)
            continue;

        if (!versions.empty())
            versions += ", ";
        versions += driver->version();
    }

    return versions;
}
