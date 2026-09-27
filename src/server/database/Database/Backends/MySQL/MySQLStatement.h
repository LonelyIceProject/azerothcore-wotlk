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

#ifndef _MYSQLSTATEMENT_H
#define _MYSQLSTATEMENT_H

#include "IDbConnectionBackend.h"
#include "MySQLHacks.h"
#include <memory>
#include <span>
#include <string>
#include <vector>

class MySQLStatement final : public IDbStatement
{
public:
    MySQLStatement(MYSQL_STMT* stmt, std::string_view queryString);
    ~MySQLStatement() override;

    [[nodiscard]] uint32 GetParameterCount() const override { return m_paramCount; }

    void BindParameters(std::span<PreparedStatementData const> params);
    void ClearParameters();

    MYSQL_STMT* GetSTMT() { return m_Mstmt; }
    MYSQL_BIND* GetBind() { return m_bind.data(); }

    // Copies the stored result of the last execution into a RowSet (binary protocol).
    std::unique_ptr<RowSet> FetchResult(DbError& err);

private:
    void SetParameter(uint8 index, bool value);
    void SetParameter(uint8 index, std::nullptr_t /*value*/);
    void SetParameter(uint8 index, std::string const& value);
    void SetParameter(uint8 index, std::vector<uint8> const& value);

    template<typename T>
    void SetParameter(uint8 index, T value);

    void AssertValidIndex(uint8 index);
    MYSQL_BIND* ResetParameter(uint8 index);

    MYSQL_STMT* m_Mstmt;
    uint32 m_paramCount;
    std::vector<bool> m_paramsSet;
    std::vector<MYSQL_BIND> m_bind;
    std::vector<std::unique_ptr<char[]>> m_buffers;
    std::vector<unsigned long> m_lengths;
    std::string m_queryString;

    // libmysql keeps pointers into the result bindings until the next mysql_stmt_bind_result
    std::vector<MYSQL_BIND> m_resultBind;
    std::vector<std::unique_ptr<char[]>> m_resultBuffers;
    std::vector<unsigned long> m_resultLengths;
    std::unique_ptr<MySQLBool[]> m_resultIsNull;

    MySQLStatement(MySQLStatement const& right) = delete;
    MySQLStatement& operator=(MySQLStatement const& right) = delete;
};

// Copy a stored text protocol result (mysql_store_result) into a RowSet.
std::unique_ptr<RowSet> MySQLFetchResult(MYSQL_RES* result);

#endif
