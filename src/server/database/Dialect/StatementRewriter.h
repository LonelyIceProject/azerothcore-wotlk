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

#ifndef _STATEMENTREWRITER_H
#define _STATEMENTREWRITER_H

#include <string>
#include <string_view>

// One MySQL statement (no trailing ';') -> SQLite. Returns an empty string for statements
// that have no effect on SQLite (SET NAMES, LOCK TABLES, COMMIT, version comments, ...).
std::string RewriteMySqlForSqlite(std::string_view sql);
// false means RewriteMySqlForSqlite() returns the input unchanged.
bool MySqlNeedsSqliteRewrite(std::string_view sql);

std::string SqliteQuoteIdentifier(std::string_view name);
// Raw bytes -> SQLite string literal.
std::string SqliteQuoteString(std::string_view value);

#endif
