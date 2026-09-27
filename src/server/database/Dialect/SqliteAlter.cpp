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

#include "SchemaDdl.h"
#include "SqliteScriptTarget.h"
#include <algorithm>
#include <sqlite3.h>

namespace
{
    bool Fail(DbError& err, std::string message, DbErrorClass cls = DbErrorClass::SchemaMismatch)
    {
        err = { cls, 0, std::move(message) };
        return false;
    }

    bool QueryInt(sqlite3* db, std::string const& sql, int64& value, DbError& err)
    {
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db, sql.c_str(), int(sql.size()), &stmt, nullptr);
        if (rc != SQLITE_OK)
        {
            err = SqliteMakeError(db, rc);
            return false;
        }

        value = 0;
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW)
            value = sqlite3_column_int64(stmt, 0);
        else if (rc != SQLITE_DONE)
        {
            err = SqliteMakeError(db, rc);
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);
        return true;
    }

    bool HasRowidAlias(TableModel const& table)
    {
        return std::any_of(table.columns.begin(), table.columns.end(), [&table](ColumnModel const& c) { return SqliteIsRowidAlias(table, c); });
    }

    bool SetSequence(sqlite3* db, std::string const& table, int64 minimum, DbError& err)
    {
        std::string const name = SqliteQuoteString(table);
        return SqliteExec(db, "DELETE FROM sqlite_sequence WHERE name = " + name, err) &&
            SqliteExec(db, "INSERT INTO sqlite_sequence (name, seq) SELECT " + name + ", MAX(" + std::to_string(minimum) +
                ", COALESCE((SELECT MAX(rowid) FROM " + SqliteQuoteIdentifier(table) + "), 0))", err);
    }

    std::string ImplicitDefault(ColumnModel const& column)
    {
        switch (column.affinity)
        {
            case ColumnAffinity::Integer:
            case ColumnAffinity::Real:
            case ColumnAffinity::Decimal:
                return "0";
            case ColumnAffinity::Blob:
                return "X''";
            case ColumnAffinity::DateTime:
                if (column.type.starts_with("date") && column.type != "date")
                    return "'0000-00-00 00:00:00'";
                if (column.type == "date")
                    return "'0000-00-00'";
                if (column.type == "time")
                    return "'00:00:00'";
                return "'0000-00-00 00:00:00'";
            default:
                if (!column.enumValues.empty())
                    return SqliteQuoteString(column.enumValues.front());
                return "''";
        }
    }

    // Default that SQLite accepts for ALTER TABLE ADD COLUMN: constant, and non-NULL for NOT NULL columns.
    bool NativeAddable(TableModel const& table, ColumnModel const& column)
    {
        if (column.autoIncrement || column.onUpdateCurrentTimestamp)
            return false;
        if (IndexModel const* pk = table.PrimaryKey())
            if (std::find_if(pk->columns.begin(), pk->columns.end(), [&column](std::string const& c) { return IdentifierEquals(c, column.name); }) != pk->columns.end())
                return false;
        if (column.defaultValue && (*column.defaultValue == "CURRENT_TIMESTAMP" || column.defaultValue->front() == '('))
            return false;
        if (column.notNull && !column.defaultValue && column.enumValues.empty())
            return false;
        return true;
    }

    void RenameInIndexes(TableModel& table, std::string const& from, std::string const& to)
    {
        for (IndexModel& index : table.indexes)
            for (std::string& c : index.columns)
                if (IdentifierEquals(c, from))
                    c = to;
        for (ForeignKeyModel& fk : table.foreignKeys)
            for (std::string& c : fk.columns)
                if (IdentifierEquals(c, from))
                    c = to;
    }

    std::string TableSignature(TableModel const& table)
    {
        return SqliteCreateTable(table, "t", false);
    }

    bool AddIndex(TableModel& table, IndexModel index, DbError& err)
    {
        for (std::string const& c : index.columns)
            if (!table.FindColumn(c))
                return Fail(err, "Key column '" + c + "' doesn't exist in table");

        if (index.kind == IndexKind::Primary)
        {
            if (table.PrimaryKey())
                return Fail(err, "Multiple primary key defined");
            index.name = "PRIMARY";
            for (std::string const& c : index.columns)
                table.FindColumn(c)->notNull = true;
        }
        else
        {
            if (index.name.empty())
                index.name = AutoIndexName(table, index);
            else if (table.FindIndex(index.name))
                return Fail(err, "Duplicate key name '" + index.name + "'");
        }

        table.indexes.push_back(std::move(index));
        return true;
    }
}

bool SqliteAlterTable(sqlite3* db, AlterTableModel const& alter, DbError& err)
{
    TableModel current;
    std::string error;
    if (!SqliteLoadTable(db, alter.table, current, error))
        return Fail(err, error.empty() ? "Table '" + alter.table + "' doesn't exist" : error);

    TableModel next = current;
    std::vector<std::optional<std::string>> origin;
    for (ColumnModel const& column : current.columns)
        origin.emplace_back(column.name);

    bool rebuild = false;
    bool renamed = false;
    std::optional<uint64> autoIncrement;
    std::vector<std::string> native;

    for (AlterOp const& op : alter.ops)
    {
        switch (op.kind)
        {
            case AlterOpKind::AddColumn:
            {
                if (next.FindColumn(op.column.name))
                {
                    if (op.ifNotExists)
                        break;
                    return Fail(err, "Duplicate column name '" + op.column.name + "'");
                }

                std::size_t pos = next.columns.size();
                if (op.position == ColumnPosition::First)
                    pos = 0;
                else if (op.position == ColumnPosition::After)
                {
                    int32 const after = next.ColumnIndex(op.after);
                    if (after < 0)
                        return Fail(err, "Unknown column '" + op.after + "' in '" + alter.table + "'");
                    pos = std::size_t(after) + 1;
                }

                next.columns.insert(next.columns.begin() + pos, op.column);
                origin.insert(origin.begin() + pos, std::nullopt);

                bool const hasKey = !op.index.columns.empty();
                if (hasKey && !AddIndex(next, op.index, err))
                    return false;

                if (pos == next.columns.size() - 1 && NativeAddable(next, op.column))
                {
                    native.push_back("ALTER TABLE " + SqliteQuoteIdentifier(next.name) + " ADD COLUMN " + SqliteColumnDefinition(op.column, false));
                    if (hasKey)
                        native.push_back(SqliteCreateIndex(next.name, next.indexes.back(), false));
                }
                else
                    rebuild = true;
                break;
            }
            case AlterOpKind::DropColumn:
            {
                int32 const index = next.ColumnIndex(op.name);
                if (index < 0)
                {
                    if (op.ifExists)
                        break;
                    return Fail(err, "Can't DROP '" + op.name + "'; check that column/key exists");
                }

                std::string const name = next.columns[index].name;
                next.columns.erase(next.columns.begin() + index);
                origin.erase(origin.begin() + index);

                for (IndexModel& i : next.indexes)
                    i.columns.erase(std::remove_if(i.columns.begin(), i.columns.end(), [&name](std::string const& c) { return IdentifierEquals(c, name); }), i.columns.end());
                next.indexes.erase(std::remove_if(next.indexes.begin(), next.indexes.end(), [](IndexModel const& i) { return i.columns.empty(); }), next.indexes.end());
                next.foreignKeys.erase(std::remove_if(next.foreignKeys.begin(), next.foreignKeys.end(), [&name](ForeignKeyModel const& fk)
                {
                    return std::any_of(fk.columns.begin(), fk.columns.end(), [&name](std::string const& c) { return IdentifierEquals(c, name); });
                }), next.foreignKeys.end());
                rebuild = true;
                break;
            }
            case AlterOpKind::ModifyColumn:
            case AlterOpKind::ChangeColumn:
            {
                int32 const index = next.ColumnIndex(op.name);
                if (index < 0)
                    return Fail(err, "Unknown column '" + op.name + "' in '" + alter.table + "'");

                std::string const oldName = next.columns[index].name;
                std::string const newName = op.kind == AlterOpKind::ChangeColumn ? op.column.name : oldName;
                if (!IdentifierEquals(oldName, newName) && next.FindColumn(newName))
                    return Fail(err, "Duplicate column name '" + newName + "'");

                TableModel renamedOnly = next;
                renamedOnly.columns[index].name = newName;
                RenameInIndexes(renamedOnly, oldName, newName);

                ColumnModel column = op.column;
                column.name = newName;
                bool const hadTrigger = next.columns[index].onUpdateCurrentTimestamp;

                next = renamedOnly;
                next.columns[index] = column;

                if (op.position != ColumnPosition::Default)
                {
                    ColumnModel moved = next.columns[index];
                    std::optional<std::string> movedOrigin = origin[index];
                    next.columns.erase(next.columns.begin() + index);
                    origin.erase(origin.begin() + index);

                    std::size_t pos = 0;
                    if (op.position == ColumnPosition::After)
                    {
                        int32 const after = next.ColumnIndex(op.after);
                        if (after < 0)
                            return Fail(err, "Unknown column '" + op.after + "' in '" + alter.table + "'");
                        pos = std::size_t(after) + 1;
                    }
                    next.columns.insert(next.columns.begin() + pos, std::move(moved));
                    origin.insert(origin.begin() + pos, std::move(movedOrigin));
                    rebuild = true;
                }

                if (!op.index.columns.empty())
                {
                    IndexModel key = op.index;
                    key.columns = { newName };
                    if (!AddIndex(next, key, err))
                        return false;
                    if (key.kind == IndexKind::Primary)
                        rebuild = true;
                    else
                        native.push_back(SqliteCreateIndex(next.name, next.indexes.back(), false));
                }

                if (TableSignature(next) != TableSignature(renamedOnly) || hadTrigger != column.onUpdateCurrentTimestamp || (hadTrigger && oldName != newName))
                    rebuild = true;
                else if (oldName != newName)
                    native.push_back("ALTER TABLE " + SqliteQuoteIdentifier(next.name) + " RENAME COLUMN " + SqliteQuoteIdentifier(oldName) + " TO " + SqliteQuoteIdentifier(newName));
                break;
            }
            case AlterOpKind::RenameColumn:
            {
                int32 const index = next.ColumnIndex(op.name);
                if (index < 0)
                    return Fail(err, "Unknown column '" + op.name + "' in '" + alter.table + "'");
                if (!IdentifierEquals(op.name, op.newName) && next.FindColumn(op.newName))
                    return Fail(err, "Duplicate column name '" + op.newName + "'");

                std::string const oldName = next.columns[index].name;
                next.columns[index].name = op.newName;
                RenameInIndexes(next, oldName, op.newName);
                if (next.columns[index].onUpdateCurrentTimestamp)
                    rebuild = true;
                else
                    native.push_back("ALTER TABLE " + SqliteQuoteIdentifier(next.name) + " RENAME COLUMN " + SqliteQuoteIdentifier(oldName) + " TO " + SqliteQuoteIdentifier(op.newName));
                break;
            }
            case AlterOpKind::AlterColumnDefault:
            {
                ColumnModel* column = next.FindColumn(op.name);
                if (!column)
                    return Fail(err, "Unknown column '" + op.name + "' in '" + alter.table + "'");
                std::string const before = TableSignature(next);
                column->defaultValue = op.defaultValue;
                if (TableSignature(next) != before)
                    rebuild = true;
                break;
            }
            case AlterOpKind::AddIndex:
            {
                if (op.ifNotExists && op.index.kind != IndexKind::Primary && next.FindIndex(op.index.name))
                    break;
                if (!AddIndex(next, op.index, err))
                    return false;
                if (op.index.kind == IndexKind::Primary)
                    rebuild = true;
                else
                    native.push_back(SqliteCreateIndex(next.name, next.indexes.back(), false));
                break;
            }
            case AlterOpKind::DropIndex:
            {
                if (IdentifierEquals(op.name, "PRIMARY"))
                {
                    next.indexes.erase(std::remove_if(next.indexes.begin(), next.indexes.end(), [](IndexModel const& i) { return i.kind == IndexKind::Primary; }), next.indexes.end());
                    rebuild = true;
                    break;
                }

                IndexModel const* index = next.FindIndex(op.name);
                if (!index)
                {
                    if (op.ifExists)
                        break;
                    return Fail(err, "Can't DROP '" + op.name + "'; check that column/key exists");
                }

                native.push_back("DROP INDEX " + SqliteQuoteIdentifier(SqliteIndexName(next.name, index->name)));
                next.indexes.erase(next.indexes.begin() + (index - next.indexes.data()));
                break;
            }
            case AlterOpKind::DropPrimaryKey:
            {
                if (!next.PrimaryKey())
                    return Fail(err, "Can't DROP 'PRIMARY'; check that column/key exists");
                next.indexes.erase(std::remove_if(next.indexes.begin(), next.indexes.end(), [](IndexModel const& i) { return i.kind == IndexKind::Primary; }), next.indexes.end());
                rebuild = true;
                break;
            }
            case AlterOpKind::AddForeignKey:
                next.foreignKeys.push_back(op.foreignKey);
                rebuild = true;
                break;
            case AlterOpKind::DropForeignKey:
            {
                auto itr = std::find_if(next.foreignKeys.begin(), next.foreignKeys.end(), [&op](ForeignKeyModel const& fk) { return IdentifierEquals(fk.name, op.name); });
                if (itr == next.foreignKeys.end())
                {
                    if (op.ifExists)
                        break;
                    return Fail(err, "Can't DROP '" + op.name + "'; check that column/key exists");
                }
                next.foreignKeys.erase(itr);
                rebuild = true;
                break;
            }
            case AlterOpKind::RenameTable:
                next.name = op.newName;
                renamed = true;
                break;
            case AlterOpKind::SetAutoIncrement:
                autoIncrement = op.autoIncrement;
                break;
            case AlterOpKind::TableOption:
                break;
        }
    }

    if (renamed && !IdentifierEquals(current.name, next.name))
    {
        TableModel existing;
        if (SqliteLoadTable(db, next.name, existing, error))
            return Fail(err, "Table '" + next.name + "' already exists");
    }

    std::vector<std::string> script;
    int64 sequence = 0;
    bool const hadSequence = HasRowidAlias(current);

    if (rebuild || (renamed && !native.empty()))
    {
        int64 foreignKeys = 0;
        if (!QueryInt(db, "PRAGMA foreign_keys", foreignKeys, err))
            return false;
        if (foreignKeys)
            return Fail(err, "ALTER TABLE on '" + alter.table + "' needs a table rebuild, which requires foreign keys to be disabled", DbErrorClass::Other);

        if (hadSequence && !QueryInt(db, "SELECT seq FROM sqlite_sequence WHERE name = " + SqliteQuoteString(current.name), sequence, err))
            return false;

        std::string const temp = next.name + "__new";
        std::string columns;
        std::string values;
        for (std::size_t i = 0; i < next.columns.size(); ++i)
        {
            ColumnModel const& column = next.columns[i];
            std::string source;
            if (origin[i])
                source = SqliteQuoteIdentifier(*origin[i]);
            else if (column.notNull && !column.defaultValue && !SqliteIsRowidAlias(next, column))
                source = ImplicitDefault(column);
            else
                continue;

            if (!columns.empty())
            {
                columns += ", ";
                values += ", ";
            }
            columns += SqliteQuoteIdentifier(column.name);
            values += source;
        }

        script.push_back("DROP TABLE IF EXISTS " + SqliteQuoteIdentifier(temp));
        script.push_back(SqliteCreateTable(next, temp, false));
        if (!columns.empty())
            script.push_back("INSERT INTO " + SqliteQuoteIdentifier(temp) + " (" + columns + ") SELECT " + values + " FROM " + SqliteQuoteIdentifier(current.name));
        script.push_back("DROP TABLE " + SqliteQuoteIdentifier(current.name));
        script.push_back("ALTER TABLE " + SqliteQuoteIdentifier(temp) + " RENAME TO " + SqliteQuoteIdentifier(next.name));
        for (IndexModel const& index : next.indexes)
            if (index.kind != IndexKind::Primary)
                script.push_back(SqliteCreateIndex(next.name, index, false));
        for (ColumnModel const& column : next.columns)
            if (column.onUpdateCurrentTimestamp)
                script.push_back(SqliteCreateOnUpdateTrigger(next.name, column.name));
    }
    else if (renamed && !IdentifierEquals(current.name, next.name))
    {
        for (IndexModel const& index : current.indexes)
            if (index.kind != IndexKind::Primary)
                script.push_back("DROP INDEX " + SqliteQuoteIdentifier(SqliteIndexName(current.name, index.name)));
        for (ColumnModel const& column : current.columns)
            if (column.onUpdateCurrentTimestamp)
                script.push_back("DROP TRIGGER " + SqliteQuoteIdentifier(SqliteOnUpdateTriggerName(current.name, column.name)));
        script.push_back("ALTER TABLE " + SqliteQuoteIdentifier(current.name) + " RENAME TO " + SqliteQuoteIdentifier(next.name));
        for (IndexModel const& index : next.indexes)
            if (index.kind != IndexKind::Primary)
                script.push_back(SqliteCreateIndex(next.name, index, false));
        for (ColumnModel const& column : next.columns)
            if (column.onUpdateCurrentTimestamp)
                script.push_back(SqliteCreateOnUpdateTrigger(next.name, column.name));
    }
    else
        script = std::move(native);

    for (std::string const& sql : script)
        if (!sql.empty() && !SqliteExec(db, sql, err))
            return false;

    if (HasRowidAlias(next) && (autoIncrement || (rebuild && hadSequence)))
    {
        int64 minimum = rebuild && hadSequence ? sequence : 0;
        if (autoIncrement && *autoIncrement > 0)
            minimum = std::max<int64>(minimum, int64(*autoIncrement) - 1);
        if (!SetSequence(db, next.name, minimum, err))
            return false;
    }

    return true;
}
