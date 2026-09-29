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

#ifndef _SQLITEEXTENSIONS_H
#define _SQLITEEXTENSIONS_H

#include "Define.h"

struct sqlite3;
struct sqlite3_api_routines;

// Entry point of an SQLite extension (functions, collations, virtual table modules). SQLite lives inside the
// database library, so an extension compiled elsewhere is built against sqlite3ext.h and reaches SQLite
// through api (SQLITE_EXTENSION_INIT2). Returns SQLITE_OK or an error code with *errorMessage set.
using SQLiteExtensionInit = int (*)(sqlite3* db, char** errorMessage, sqlite3_api_routines const* api);

// Runs init on every SQLite connection opened afterwards.
AC_DATABASE_API bool AddSQLiteExtension(SQLiteExtensionInit init);
AC_DATABASE_API void RemoveSQLiteExtension(SQLiteExtensionInit init);

#endif
