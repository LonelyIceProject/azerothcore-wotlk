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

#include "ModuleDatabasePool.h"
#include "Errors.h"
#include "IDbConnectionBackend.h"
#include "Log.h"
#include "QueryResult.h"
#include "Transaction.h"
#include <limits>
#include <thread>

ModuleDatabasePool::ModuleDatabasePool()
    : _synchThreads(0)
{
}

ModuleDatabasePool::~ModuleDatabasePool()
{
    Close();
}

void ModuleDatabasePool::SetConnectionInfo(std::string_view infoString, uint8 synchThreads)
{
    _connectionInfo = DatabaseConnectionInfo(infoString);
    _synchThreads = synchThreads;
}

uint32 ModuleDatabasePool::Open()
{
    DbError const error = OpenEx();
    if (!error.IsError())
        return 0;

    return error.native ? uint32(error.native) : 1;
}

DbError ModuleDatabasePool::OpenEx(bool create)
{
    if (!_connectionInfo.IsValid())
    {
        LOG_ERROR("sql.driver", "ModuleDatabasePool: database `{}` has an invalid connection string.", _connectionInfo.database);
        return { DbErrorClass::Other, 0, "invalid connection string" };
    }

    if (!_synchThreads)
    {
        LOG_ERROR("sql.driver", "ModuleDatabasePool: database `{}` was configured with 0 synchronous connections, "
            "at least one is required.", _connectionInfo.database);
        return { DbErrorClass::Other, 0, "no synchronous connections configured" };
    }

    Close();

    for (uint8 i = 0; i < _synchThreads; ++i)
    {
        auto conn = std::unique_ptr<DatabaseConnection>(CreateConnection(_connectionInfo));
        DbError const error = conn->Open(create);
        if (error.IsError())
        {
            LOG_ERROR("sql.driver", "ModuleDatabasePool: could not open connection {}/{} to database `{}`, error {}",
                i + 1, _synchThreads, _connectionInfo.database, error.native);
            Close();
            return error;
        }

        _connections.push_back(std::move(conn));
    }

    return {};
}

bool ModuleDatabasePool::PrepareStatements()
{
    for (auto const& conn : _connections)
    {
        conn->LockIfReady();
        if (!conn->PrepareStatements())
        {
            conn->Unlock();
            Close();
            return false;
        }

        conn->Unlock();
    }

    if (!_connections.empty())
    {
        DatabaseConnection const* conn = _connections.front().get();
        _preparedStatementSize.assign(conn->m_stmts.size(), 0);
        for (std::size_t i = 0; i < conn->m_stmts.size(); ++i)
        {
            if (IDbStatement* stmt = conn->m_stmts[i].get())
            {
                uint32 const paramCount = stmt->GetParameterCount();
                ASSERT(paramCount < std::numeric_limits<uint8>::max());
                _preparedStatementSize[i] = static_cast<uint8>(paramCount);
            }
        }
    }

    return true;
}

void ModuleDatabasePool::Close()
{
    _connections.clear();

    _preparedStatementSize.clear();
}

void ModuleDatabasePool::Execute(std::string_view sql)
{
    // Synchronous for now - kept separate from DirectExecute so async execution
    // can be added later without touching callers.
    DirectExecute(sql);
}

void ModuleDatabasePool::DirectExecute(std::string_view sql)
{
    if (sql.empty())
        return;

    if (_connections.empty())
        return;

    DatabaseConnection* conn = GetFreeConnection();
    conn->Execute(sql);
    conn->Unlock();
}

QueryResult ModuleDatabasePool::Query(std::string_view sql)
{
    if (_connections.empty())
        return QueryResult(nullptr);

    DatabaseConnection* conn = GetFreeConnection();
    ResultSet* result = conn->Query(sql);
    conn->Unlock();

    // Mirror DatabaseWorkerPool<T>::Query semantics: nullptr for empty results,
    // and the first row loaded before the result is handed out.
    if (!result || !result->GetRowCount() || !result->NextRow())
    {
        delete result;
        return QueryResult(nullptr);
    }

    return QueryResult(result);
}

void ModuleDatabasePool::Execute(PreparedStatementBase* stmt)
{
    if (_connections.empty())
    {
        delete stmt;
        return;
    }

    DatabaseConnection* conn = GetFreeConnection();
    conn->Execute(stmt);
    conn->Unlock();

    delete stmt;
}

PreparedQueryResult ModuleDatabasePool::Query(PreparedStatementBase* stmt)
{
    if (_connections.empty())
    {
        delete stmt;
        return PreparedQueryResult(nullptr);
    }

    DatabaseConnection* conn = GetFreeConnection();
    PreparedResultSet* result = conn->Query(stmt);
    conn->Unlock();

    //! Delete proxy-class. Not needed anymore
    delete stmt;

    if (!result || !result->GetRowCount())
    {
        delete result;
        return PreparedQueryResult(nullptr);
    }

    return PreparedQueryResult(result);
}

uint8 ModuleDatabasePool::GetPreparedStatementParamCount(uint32 index) const
{
    return index < _preparedStatementSize.size() ? _preparedStatementSize[index] : 0;
}

void ModuleDatabasePool::DirectCommitTransaction(std::shared_ptr<TransactionBase> transaction)
{
    if (_connections.empty())
        return;

    DatabaseConnection* conn = GetFreeConnection();
    DbErrorClass errorClass = conn->ExecuteTransaction(transaction);
    if (errorClass == DbErrorClass::None)
    {
        conn->Unlock();
        return;
    }

    //! Handle deadlocks and busy databases without extending deadlock to the core itself
    if (errorClass == DbErrorClass::Retryable)
    {
        uint8 constexpr loopBreaker = 5;
        for (uint8 i = 0; i < loopBreaker; ++i)
        {
            if (conn->ExecuteTransaction(transaction) == DbErrorClass::None)
                break;
        }
    }

    transaction->Cleanup();
    conn->Unlock();
}

void ModuleDatabasePool::KeepAlive()
{
    if (!GetBackendCaps(_connectionInfo.backend).needsKeepAlive)
        return;

    //! Ping connections that are not busy; a locked connection is in use and alive.
    for (auto const& conn : _connections)
    {
        if (conn->LockIfReady())
        {
            conn->Ping();
            conn->Unlock();
        }
    }
}

DatabaseConnection* ModuleDatabasePool::GetFreeConnection()
{
    uint8 i = 0;
    auto const num_cons = _connections.size();
    DatabaseConnection* connection = nullptr;

    //! Block forever until a connection is free
    for (;;)
    {
        connection = _connections[++i % num_cons].get();
        //! Must be matched with connection->Unlock() or you will get deadlocks
        if (connection->LockIfReady())
            break;

        if (i % num_cons == 0)
            std::this_thread::yield();
    }

    return connection;
}

DatabaseConnectionInfo const* ModuleDatabasePool::GetConnectionInfo() const
{
    return &_connectionInfo;
}

void ModuleDatabasePool::RunOnSyncConnection(std::function<void(DatabaseConnection&)> const& fn)
{
    if (_connections.empty())
        return;

    DatabaseConnection* conn = GetFreeConnection();
    fn(*conn);
    conn->Unlock();
}
