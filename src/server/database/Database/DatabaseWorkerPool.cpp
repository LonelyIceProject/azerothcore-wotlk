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

#include "DatabaseWorkerPool.h"
#include "AdhocStatement.h"
#include "CharacterDatabase.h"
#include "Errors.h"
#include "IDbConnectionBackend.h"
#include "Log.h"
#include "LoginDatabase.h"
#include "PCQueue.h"
#include "PreparedStatement.h"
#include "QueryCallback.h"
#include "QueryHolder.h"
#include "QueryResult.h"
#include "SQLOperation.h"
#include "SqlDialect.h"
#include "Transaction.h"
#include "WorldDatabase.h"
#include <limits>
#include <vector>

#ifdef ACORE_DEBUG
#include <boost/stacktrace.hpp>
#include <sstream>
#endif

class PingOperation : public SQLOperation
{
    //! Operation for idle delaythreads
    bool Execute() override
    {
        m_conn->Ping();
        return true;
    }
};

template <class T>
DatabaseWorkerPool<T>::DatabaseWorkerPool() :
    _queue(new ProducerConsumerQueue<SQLOperation*>()),
    _async_threads(0),
    _synch_threads(0)
{
}

template <class T>
DatabaseWorkerPool<T>::~DatabaseWorkerPool()
{
    _queue->Cancel();
}

template <class T>
void DatabaseWorkerPool<T>::SetConnectionInfo(std::string_view infoString, uint8 asyncThreads, uint8 const synchThreads)
{
    _connectionInfo = std::make_unique<DatabaseConnectionInfo>(infoString);

    DbBackendCaps const caps = GetBackendCaps(_connectionInfo->backend);
    if (caps.maxAsyncWorkers && asyncThreads > caps.maxAsyncWorkers)
    {
        LOG_WARN("sql.driver", "DatabasePool '{}': the {} backend supports at most {} worker thread(s), {} configured.",
            GetDatabaseName(), DatabaseBackendName(_connectionInfo->backend), caps.maxAsyncWorkers, asyncThreads);
        asyncThreads = caps.maxAsyncWorkers;
    }

    _async_threads = asyncThreads;
    _synch_threads = synchThreads;
}

template <class T>
DbError DatabaseWorkerPool<T>::Open(bool create)
{
    WPFatal(_connectionInfo.get(), "Connection info was not set!");

    if (!_connectionInfo->IsValid())
    {
        LOG_ERROR("sql.driver", "DatabasePool '{}': invalid connection string.", GetDatabaseName());
        return { DbErrorClass::Other, 0, "invalid connection string" };
    }

    LOG_INFO("sql.driver", "Opening DatabasePool '{}'. Asynchronous connections: {}, synchronous connections: {}.",
        GetDatabaseName(), _async_threads, _synch_threads);

    _queue->Cancel();
    _connections[IDX_ASYNC].clear();
    _connections[IDX_SYNCH].clear();
    _queue->Reset();

    DbError error = OpenConnections(IDX_ASYNC, _async_threads, create);

    if (error.IsError())
        return error;

    error = OpenConnections(IDX_SYNCH, _synch_threads, create);

    if (!error.IsError())
    {
        LOG_INFO("sql.driver", "DatabasePool '{}' opened successfully. {} total connections running.",
            GetDatabaseName(), (_connections[IDX_SYNCH].size() + _connections[IDX_ASYNC].size()));
    }

    LOG_INFO("sql.driver", " ");

    return error;
}

template <class T>
void DatabaseWorkerPool<T>::Close()
{
    LOG_INFO("sql.driver", "Closing down DatabasePool '{}'. Waiting for {} queries to finish...", GetDatabaseName(), _queue->Size());

    // Gracefully close async query queue, worker threads will block when the destructor
    // is called from the .clear() functions below until the queue is empty
    _queue->Shutdown();

    //! Closes the actualy database connection.
    _connections[IDX_ASYNC].clear();

    LOG_INFO("sql.driver", "Asynchronous connections on DatabasePool '{}' terminated. Proceeding with synchronous connections.",
        GetDatabaseName());

    //! Shut down the synchronous connections
    //! There's no need for locking the connection, because DatabaseWorkerPool<>::Close
    //! should only be called after any other thread tasks in the core have exited,
    //! meaning there can be no concurrent access at this point.
    _connections[IDX_SYNCH].clear();

    LOG_INFO("sql.driver", "All connections on DatabasePool '{}' closed.", GetDatabaseName());
}

template <class T>
bool DatabaseWorkerPool<T>::PrepareStatements()
{
    for (auto const& connections : _connections)
    {
        for (auto const& connection : connections)
        {
            connection->LockIfReady();
            if (!connection->PrepareStatements())
            {
                connection->Unlock();
                Close();
                return false;
            }
            else
                connection->Unlock();

            std::size_t const preparedSize = connection->m_stmts.size();
            if (_preparedStatementSize.size() < preparedSize)
                _preparedStatementSize.resize(preparedSize);

            for (std::size_t i = 0; i < preparedSize; ++i)
            {
                // already set by another connection
                // (each connection only has prepared statements of it's own type sync/async)
                if (_preparedStatementSize[i] > 0)
                    continue;

                if (IDbStatement* stmt = connection->m_stmts[i].get())
                {
                    uint32 const paramCount = stmt->GetParameterCount();

                    // WH only supports uint8 indices.
                    ASSERT(paramCount < std::numeric_limits<uint8>::max());

                    _preparedStatementSize[i] = static_cast<uint8>(paramCount);
                }
            }
        }
    }

    return true;
}

template <class T>
QueryResult DatabaseWorkerPool<T>::Query(std::string_view sql)
{
    auto connection = GetFreeConnection();

    ResultSet* result = connection->Query(sql);
    connection->Unlock();

    if (!result || !result->GetRowCount() || !result->NextRow())
    {
        delete result;
        return QueryResult(nullptr);
    }

    return QueryResult(result);
}

template <class T>
PreparedQueryResult DatabaseWorkerPool<T>::Query(PreparedStatement<T>* stmt)
{
    auto connection = GetFreeConnection();
    PreparedResultSet* ret = connection->Query(stmt);
    connection->Unlock();

    //! Delete proxy-class. Not needed anymore
    delete stmt;

    if (!ret || !ret->GetRowCount())
    {
        delete ret;
        return PreparedQueryResult(nullptr);
    }

    return PreparedQueryResult(ret);
}

template <class T>
QueryCallback DatabaseWorkerPool<T>::AsyncQuery(std::string_view sql)
{
    BasicStatementTask* task = new BasicStatementTask(sql, true);
    // Store future result before enqueueing - task might get already processed and deleted before returning from this method
    QueryResultFuture result = task->GetFuture();
    Enqueue(task);
    return QueryCallback(std::move(result));
}

template <class T>
QueryCallback DatabaseWorkerPool<T>::AsyncQuery(PreparedStatement<T>* stmt)
{
    PreparedStatementTask* task = new PreparedStatementTask(stmt, true);
    // Store future result before enqueueing - task might get already processed and deleted before returning from this method
    PreparedQueryResultFuture result = task->GetFuture();
    Enqueue(task);
    return QueryCallback(std::move(result));
}

template <class T>
SQLQueryHolderCallback DatabaseWorkerPool<T>::DelayQueryHolder(std::shared_ptr<SQLQueryHolder<T>> holder)
{
    SQLQueryHolderTask* task = new SQLQueryHolderTask(holder);
    // Store future result before enqueueing - task might get already processed and deleted before returning from this method
    QueryResultHolderFuture result = task->GetFuture();
    Enqueue(task);
    return { std::move(holder), std::move(result) };
}

template <class T>
SQLTransaction<T> DatabaseWorkerPool<T>::BeginTransaction()
{
    return std::make_shared<Transaction<T>>();
}

template <class T>
void DatabaseWorkerPool<T>::CommitTransaction(SQLTransaction<T> transaction)
{
#ifdef ACORE_DEBUG
    //! Only analyze transaction weaknesses in Debug mode.
    //! Ideally we catch the faults in Debug mode and then correct them,
    //! so there's no need to waste these CPU cycles in Release mode.
    switch (transaction->GetSize())
    {
    case 0:
        LOG_DEBUG("sql.driver", "Transaction contains 0 queries. Not executing.");
        return;
    case 1:
        LOG_DEBUG("sql.driver", "Warning: Transaction only holds 1 query, consider removing Transaction context in code.");
        break;
    default:
        break;
    }
#endif // ACORE_DEBUG

    Enqueue(new TransactionTask(transaction));
}

template <class T>
TransactionCallback DatabaseWorkerPool<T>::AsyncCommitTransaction(SQLTransaction<T> transaction)
{
#ifdef ACORE_DEBUG
    //! Only analyze transaction weaknesses in Debug mode.
    //! Ideally we catch the faults in Debug mode and then correct them,
    //! so there's no need to waste these CPU cycles in Release mode.
    switch (transaction->GetSize())
    {
        case 0:
            LOG_DEBUG("sql.driver", "Transaction contains 0 queries. Not executing.");
            break;
        case 1:
            LOG_DEBUG("sql.driver", "Warning: Transaction only holds 1 query, consider removing Transaction context in code.");
            break;
        default:
            break;
    }
#endif // ACORE_DEBUG

    TransactionWithResultTask* task = new TransactionWithResultTask(transaction);
    TransactionFuture result = task->GetFuture();
    Enqueue(task);
    return TransactionCallback(std::move(result));
}

template <class T>
void DatabaseWorkerPool<T>::DirectCommitTransaction(SQLTransaction<T>& transaction)
{
    T* connection = GetFreeConnection();
    DbErrorClass errorClass = connection->ExecuteTransaction(transaction);

    if (errorClass == DbErrorClass::None)
    {
        connection->Unlock();      // OK, operation succesful
        return;
    }

    //! Handle deadlocks and busy databases without extending deadlock to the core itself
    /// @todo More elegant way
    if (errorClass == DbErrorClass::Retryable)
    {
        //todo: handle multiple sync threads deadlocking in a similar way as async threads
        uint8 loopBreaker = 5;

        for (uint8 i = 0; i < loopBreaker; ++i)
        {
            if (connection->ExecuteTransaction(transaction) == DbErrorClass::None)
                break;
        }
    }

    //! Clean up now.
    transaction->Cleanup();

    connection->Unlock();
}

template <class T>
PreparedStatement<T>* DatabaseWorkerPool<T>::GetPreparedStatement(PreparedStatementIndex index)
{
    return new PreparedStatement<T>(index, _preparedStatementSize[index]);
}

template <class T>
void DatabaseWorkerPool<T>::EscapeString(std::string& str)
{
    if (str.empty())
        return;

    str = MySqlEscape(str);
}

template <class T>
void DatabaseWorkerPool<T>::KeepAlive()
{
    if (!GetBackendCaps(GetBackend()).needsKeepAlive)
        return;

    //! Ping synchronous connections
    for (auto& connection : _connections[IDX_SYNCH])
    {
        if (connection->LockIfReady())
        {
            connection->Ping();
            connection->Unlock();
        }
    }

    //! Assuming all worker threads are free, every worker thread will receive 1 ping operation request
    //! If one or more worker threads are busy, the ping operations will not be split evenly, but this doesn't matter
    //! as the sole purpose is to prevent connections from idling.
    auto const count = _connections[IDX_ASYNC].size();

    for (uint8 i = 0; i < count; ++i)
        Enqueue(new PingOperation);
}

template <class T>
DbError DatabaseWorkerPool<T>::OpenConnections(InternalIndex type, uint8 numConnections, bool create)
{
    for (uint8 i = 0; i < numConnections; ++i)
    {
        // Create the connection
        auto connection = [&]
        {
            switch (type)
            {
            case IDX_ASYNC:
                return std::make_unique<T>(_queue.get(), *_connectionInfo);
            case IDX_SYNCH:
                return std::make_unique<T>(*_connectionInfo);
            default:
                ABORT();
            }
        }();

        DbError error = connection->Open(create);
        if (error.IsError())
        {
            // Failed to open a connection or invalid version, abort and cleanup
            _queue->Cancel();
            _connections[type].clear();
            return error;
        }

        _connections[type].push_back(std::move(connection));
    }

    // Everything is fine
    return {};
}

template <class T>
DatabaseBackend DatabaseWorkerPool<T>::GetBackend() const
{
    return _connectionInfo ? _connectionInfo->backend : DatabaseBackend::MySQL;
}

template <class T>
void DatabaseWorkerPool<T>::RunOnSyncConnection(std::function<void(DatabaseConnection&)> const& fn)
{
    T* connection = GetFreeConnection();
    try
    {
        fn(*connection);
    }
    catch (...)
    {
        connection->Unlock();
        throw;
    }
    connection->Unlock();
}
template <class T>
void DatabaseWorkerPool<T>::Enqueue(SQLOperation* op)
{
    _queue->Push(op);
}

template <class T>
std::size_t DatabaseWorkerPool<T>::QueueSize() const
{
    return _queue->Size();
}

template <class T>
T* DatabaseWorkerPool<T>::GetFreeConnection()
{
#ifdef ACORE_DEBUG
    if (_warnSyncQueries)
    {
        std::ostringstream ss;
        ss << boost::stacktrace::stacktrace();
        LOG_WARN("sql.performances", "Sync query at:\n{}", ss.str());
    }
#endif

    uint8 i = 0;
    auto const num_cons = _connections[IDX_SYNCH].size();
    T* connection = nullptr;

    //! Block forever until a connection is free
    for (;;)
    {
        connection = _connections[IDX_SYNCH][++i % num_cons].get();
        //! Must be matched with t->Unlock() or you will get deadlocks
        if (connection->LockIfReady())
            break;
    }

    return connection;
}

template <class T>
std::string_view DatabaseWorkerPool<T>::GetDatabaseName() const
{
    return std::string_view{ _connectionInfo->database };
}

template <class T>
void DatabaseWorkerPool<T>::Execute(std::string_view sql)
{
    if (sql.empty())
        return;

    BasicStatementTask* task = new BasicStatementTask(sql);
    Enqueue(task);
}

template <class T>
void DatabaseWorkerPool<T>::Execute(PreparedStatement<T>* stmt)
{
    PreparedStatementTask* task = new PreparedStatementTask(stmt);
    Enqueue(task);
}

template <class T>
void DatabaseWorkerPool<T>::DirectExecute(std::string_view sql)
{
    if (sql.empty())
        return;

    T* connection = GetFreeConnection();
    connection->Execute(sql);
    connection->Unlock();
}

template <class T>
void DatabaseWorkerPool<T>::DirectExecute(PreparedStatement<T>* stmt)
{
    T* connection = GetFreeConnection();
    connection->Execute(stmt);
    connection->Unlock();

    //! Delete proxy-class. Not needed anymore
    delete stmt;
}

template <class T>
void DatabaseWorkerPool<T>::ExecuteOrAppend(SQLTransaction<T>& trans, std::string_view sql)
{
    if (!trans)
        Execute(sql);
    else
        trans->Append(sql);
}

template <class T>
void DatabaseWorkerPool<T>::ExecuteOrAppend(SQLTransaction<T>& trans, PreparedStatement<T>* stmt)
{
    if (!trans)
        Execute(stmt);
    else
        trans->Append(stmt);
}

template class AC_DATABASE_API DatabaseWorkerPool<LoginDatabaseConnection>;
template class AC_DATABASE_API DatabaseWorkerPool<WorldDatabaseConnection>;
template class AC_DATABASE_API DatabaseWorkerPool<CharacterDatabaseConnection>;
