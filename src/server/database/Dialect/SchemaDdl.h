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

#ifndef _SCHEMADDL_H
#define _SCHEMADDL_H

#include "DatabaseBackend.h"
#include "SchemaModel.h"
#include "StatementRewriter.h"
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

enum class DdlSyntax : uint8
{
    MySql,                      // `ident`, '...'/"..." strings with backslash escapes
    Sqlite                      // "ident", '...' strings
};

ColumnAffinity MySqlTypeAffinity(std::string_view type);

bool ParseCreateTable(std::string_view sql, TableModel& table, bool& ifNotExists, std::string& error, DdlSyntax syntax);
bool ParseIndexStatement(std::string_view sql, AlterTableModel& alter, std::string& error, DdlSyntax syntax);

// MySQL name of an unnamed key: first column, then _2, _3, ...
std::string AutoIndexName(TableModel const& table, IndexModel const& index);

std::string SqliteColumnType(ColumnModel const& column);
std::string SqliteColumnDefinition(ColumnModel const& column, bool rowidAlias);
bool SqliteIsRowidAlias(TableModel const& table, ColumnModel const& column);
std::string SqliteIndexName(std::string_view table, std::string_view index);
std::string SqliteCreateTable(TableModel const& table, std::string_view name, bool ifNotExists);
std::string SqliteCreateIndex(std::string_view table, IndexModel const& index, bool ifNotExists);
std::string SqliteOnUpdateTriggerName(std::string_view table, std::string_view column);
std::string SqliteCreateOnUpdateTrigger(std::string_view table, std::string_view column);
// CREATE TABLE, indexes, triggers and sqlite_sequence for a new table.
std::vector<std::string> SqliteCreateTableScript(TableModel const& table, bool ifNotExists);

// Reads a table created by SqliteCreateTableScript back from sqlite_schema. false if the table does not exist.
bool SqliteLoadTable(sqlite3* db, std::string_view table, TableModel& model, std::string& error);

// ALTER TABLE on SQLite: native statements where possible, otherwise a table rebuild.
// A rebuild requires foreign keys to be disabled on the connection.
bool SqliteAlterTable(sqlite3* db, AlterTableModel const& alter, DbError& err);

#endif
