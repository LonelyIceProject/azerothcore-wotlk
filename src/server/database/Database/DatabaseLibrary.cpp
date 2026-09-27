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

#include "DatabaseLibrary.h"
#include "StringFormat.h"

#ifdef ACORE_WITH_MYSQL
#include "MySQLBackend.h"
#endif

#ifdef ACORE_WITH_SQLITE
#include <sqlite3.h>
#endif

void DatabaseLibrary::Init()
{
#ifdef ACORE_WITH_MYSQL
    MySQLLibrary::Init();
#endif
#ifdef ACORE_WITH_SQLITE
    sqlite3_initialize();
#endif
}

void DatabaseLibrary::End()
{
#ifdef ACORE_WITH_MYSQL
    MySQLLibrary::End();
#endif
}

std::string DatabaseLibrary::Version()
{
    std::string version;

#ifdef ACORE_WITH_MYSQL
    version = MySQLLibrary::Version();
#endif
#ifdef ACORE_WITH_SQLITE
    if (!version.empty())
        version += ", ";
    version += Acore::StringFormat("SQLite {}", sqlite3_libversion());
#endif

    return version;
}
