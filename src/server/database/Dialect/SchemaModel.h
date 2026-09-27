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

#ifndef _SCHEMAMODEL_H
#define _SCHEMAMODEL_H

#include "Define.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Backend-neutral schema IR. Identifier lookups are ASCII case-insensitive, as in MySQL.

enum class ColumnAffinity : uint8
{
    Integer,
    Real,
    Decimal,
    Text,
    Blob,
    DateTime
};

struct ColumnModel
{
    std::string name;
    std::string type;                           // MySQL type as written, lowercased: "int unsigned", "varchar(255)", "enum('a','b')"
    ColumnAffinity affinity = ColumnAffinity::Text;
    bool isUnsigned = false;
    bool notNull = false;
    bool autoIncrement = false;
    std::optional<std::string> defaultValue;    // MySQL-dialect expression/literal text; nullopt = no DEFAULT
    bool onUpdateCurrentTimestamp = false;
    bool caseInsensitive = false;               // *_ci collation
    std::vector<std::string> enumValues;        // unquoted, for enum(...) and set(...)
};

enum class IndexKind : uint8
{
    Primary,
    Unique,
    Index,
    FullText
};

struct IndexModel
{
    std::string name;                           // MySQL name ("PRIMARY" for the primary key)
    IndexKind kind = IndexKind::Index;
    std::vector<std::string> columns;           // prefix lengths dropped
};

struct ForeignKeyModel
{
    std::string name;
    std::vector<std::string> columns;
    std::string refTable;
    std::vector<std::string> refColumns;
    std::string onDelete;                       // "CASCADE", "SET NULL", ... empty = default
    std::string onUpdate;
};

struct TableModel
{
    std::string name;
    std::vector<ColumnModel> columns;           // MySQL column order
    std::vector<IndexModel> indexes;            // includes the primary key
    std::vector<ForeignKeyModel> foreignKeys;
    std::vector<std::string> checks;            // CHECK expressions, MySQL dialect
    std::optional<uint64> autoIncrement;        // table option AUTO_INCREMENT=N

    [[nodiscard]] ColumnModel const* FindColumn(std::string_view column) const;
    ColumnModel* FindColumn(std::string_view column);
    [[nodiscard]] int32 ColumnIndex(std::string_view column) const;       // -1 if absent
    [[nodiscard]] IndexModel const* FindIndex(std::string_view index) const;
    [[nodiscard]] IndexModel const* PrimaryKey() const;
    [[nodiscard]] std::vector<std::string> ColumnNames() const;
};

enum class AlterOpKind : uint8
{
    AddColumn,
    DropColumn,
    ModifyColumn,                               // MODIFY [COLUMN] c def
    ChangeColumn,                               // CHANGE [COLUMN] old new def
    RenameColumn,                               // RENAME COLUMN old TO new
    AlterColumnDefault,                         // ALTER [COLUMN] c SET DEFAULT x | DROP DEFAULT
    AddIndex,                                   // ADD [UNIQUE|FULLTEXT] INDEX|KEY, ADD PRIMARY KEY, CREATE INDEX
    DropIndex,                                  // DROP INDEX|KEY, DROP INDEX ON (standalone)
    DropPrimaryKey,
    AddForeignKey,
    DropForeignKey,
    RenameTable,
    SetAutoIncrement,
    TableOption                                 // ENGINE=, CHARSET=, COMMENT=, CONVERT TO ...: no-op
};

enum class ColumnPosition : uint8
{
    Default,                                    // append (ADD) or keep (MODIFY/CHANGE)
    First,
    After
};

struct AlterOp
{
    AlterOpKind kind = AlterOpKind::TableOption;
    ColumnModel column;                         // Add/Modify/Change: new definition
    std::string name;                           // old column / index / foreign key name
    std::string newName;                        // RenameColumn / RenameTable
    ColumnPosition position = ColumnPosition::Default;
    std::string after;                          // ColumnPosition::After
    IndexModel index;                           // AddIndex
    ForeignKeyModel foreignKey;                 // AddForeignKey
    std::optional<std::string> defaultValue;    // AlterColumnDefault; nullopt = DROP DEFAULT
    uint64 autoIncrement = 0;                   // SetAutoIncrement
    bool ifExists = false;
    bool ifNotExists = false;
};

struct AlterTableModel
{
    std::string table;
    std::vector<AlterOp> ops;
};

// MySQL DDL -> IR. On failure returns false and fills error.
bool ParseMySqlCreateTable(std::string_view sql, TableModel& table, bool& ifNotExists, std::string& error);
bool ParseMySqlAlterTable(std::string_view sql, AlterTableModel& alter, std::string& error);
// CREATE [UNIQUE|FULLTEXT] INDEX n ON t (...) / DROP INDEX n ON t
bool ParseMySqlIndexStatement(std::string_view sql, AlterTableModel& alter, std::string& error);

bool IdentifierEquals(std::string_view a, std::string_view b);

#endif
