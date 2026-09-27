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

#ifndef _SQLITESTATEMENT_H
#define _SQLITESTATEMENT_H

#include "IDbConnectionBackend.h"

struct sqlite3;
struct sqlite3_stmt;

class SQLiteStatement : public IDbStatement
{
public:
    explicit SQLiteStatement(sqlite3_stmt* stmt);
    ~SQLiteStatement() override;

    SQLiteStatement(SQLiteStatement const&) = delete;
    SQLiteStatement& operator=(SQLiteStatement const&) = delete;

    [[nodiscard]] uint32 GetParameterCount() const override { return _parameterCount; }
    [[nodiscard]] sqlite3_stmt* GetHandle() const { return _stmt; }

private:
    sqlite3_stmt* _stmt;
    uint32 _parameterCount;
};

namespace SQLite
{
    DbErrorClass ClassifyError(int code, std::string_view message);
    DbError MakeError(sqlite3* db, int code);

    bool BindParameters(sqlite3_stmt* stmt, std::span<PreparedStatementData const> params, DbError& err);
    // Steps to completion and discards rows. The caller resets or finalizes the statement.
    bool Run(sqlite3_stmt* stmt, DbError& err);
    // Steps to completion and materializes all rows. The caller resets or finalizes the statement.
    std::unique_ptr<RowSet> Fetch(sqlite3_stmt* stmt, DbError& err);

    std::string QuoteIdentifier(std::string_view identifier);
}

#endif
