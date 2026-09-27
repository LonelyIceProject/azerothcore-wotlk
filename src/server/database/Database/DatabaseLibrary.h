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

#ifndef _DATABASELIBRARY_H
#define _DATABASELIBRARY_H

#include "Define.h"
#include <string>

// Process-wide init/teardown of every compiled-in client library.
namespace DatabaseLibrary
{
    AC_DATABASE_API void Init();
    AC_DATABASE_API void End();
    // e.g. "MySQL 8.0.36, SQLite 3.50.4"
    AC_DATABASE_API std::string Version();
}

#endif
