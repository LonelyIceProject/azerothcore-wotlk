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

#ifndef _SCRIPTRUNNER_H
#define _SCRIPTRUNNER_H

#include "DatabaseBackend.h"
#include "SchemaModel.h"
#include <cstddef>
#include <filesystem>
#include <istream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

struct ISchemaLookup;

// Null, integer, real, text, blob
using ScriptValue = std::variant<std::nullptr_t, int64, double, std::string, std::vector<uint8>>;
using ScriptRow = std::vector<ScriptValue>;

enum class BulkInsertMode : uint8
{
    Insert,
    InsertIgnore,
    Replace
};

// Where a script is applied. SQL handed to Exec/Scalar is already in the target's dialect;
// DDL arrives as schema IR so each backend emits its own.
class IScriptTarget
{
public:
    virtual ~IScriptTarget() = default;

    [[nodiscard]] virtual DatabaseBackend Backend() const = 0;

    virtual bool Exec(std::string_view sql, DbError& err) = 0;
    // First column of the first row; Null when no rows.
    virtual bool Scalar(std::string_view sql, ScriptValue& value, DbError& err) = 0;
    // columns empty = all columns of the table in model order.
    virtual bool BulkInsert(std::string_view table, std::vector<std::string> const& columns, std::span<ScriptRow const> rows, BulkInsertMode mode, DbError& err) = 0;

    // false if the table does not exist.
    virtual bool LoadTable(std::string_view table, TableModel& model) = 0;
    virtual bool CreateTable(TableModel const& table, bool ifNotExists, DbError& err) = 0;
    virtual bool AlterTable(AlterTableModel const& alter, DbError& err) = 0;
    virtual bool DropTable(std::string_view table, bool ifExists, DbError& err) = 0;

    virtual bool Begin(DbError& err) = 0;
    virtual bool Commit(DbError& err) = 0;
    virtual void Rollback() = 0;
    // Must be called outside a transaction.
    virtual bool SetForeignKeys(bool enabled, DbError& err) = 0;
    // Fails with a Constraint error listing violations.
    virtual bool CheckForeignKeys(DbError& err) = 0;

    [[nodiscard]] virtual ISchemaLookup const* Schema() const { return nullptr; }

    // The target runs MySQL statements itself: ScriptRunner hands every statement of a script to Exec as written,
    // without translating it or going through the schema model.
    [[nodiscard]] virtual bool RunsMySqlAsIs() const { return false; }
};

struct ScriptRunnerOptions
{
    bool transaction = true;            // Begin/Commit around the whole script
    bool disableForeignKeys = false;    // off before Begin, CheckForeignKeys before Commit, back on after
    bool truncateOnFirstInsert = false; // empty each table before the first INSERT into it (sqlconv load --truncate)
};

struct ScriptError
{
    std::string source;                 // file name or stream name
    uint32 statement = 0;               // 1-based
    uint32 line = 0;
    std::string sql;                    // offending statement, MySQL text
    std::string message;

    [[nodiscard]] std::string ToString() const;
};

// Applies a MySQL-dialect script (base dump, update file, mysqldump output) to an IScriptTarget.
// @variables live for one Run* call.
class ScriptRunner
{
public:
    explicit ScriptRunner(IScriptTarget& target, ScriptRunnerOptions const& options = {});
    ~ScriptRunner();

    ScriptRunner(ScriptRunner const&) = delete;
    ScriptRunner& operator=(ScriptRunner const&) = delete;

    bool RunFile(std::filesystem::path const& file);
    bool RunStream(std::istream& in, std::string_view sourceName);
    bool RunText(std::string_view sql, std::string_view sourceName);

    [[nodiscard]] ScriptError const& GetError() const;
    [[nodiscard]] uint64 GetExecutedStatements() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

#endif
