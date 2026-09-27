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

#include "DatabaseConnection.h"
#include "DatabaseWorker.h"
#include "Errors.h"
#include "IDbConnectionBackend.h"
#include "Log.h"
#include "MySqlLexer.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SqlDialect.h"
#include "Timer.h"
#include "Transaction.h"
#include <thread>

namespace
{
    DbError NotConnectedError()
    {
        return { DbErrorClass::ConnectionLost, 0, "not connected" };
    }

    uint32 CountParameters(std::string_view sql)
    {
        uint32 count = 0;
        for (MySqlToken const& token : MySqlTokenize(sql))
            if (token.type == MySqlTokenType::Parameter)
                ++count;

        return count;
    }
}

DatabaseConnection::DatabaseConnection(DatabaseConnectionInfo& connInfo) :
    m_reconnecting(false),
    m_prepareError(false),
    m_queue(nullptr),
    m_connectionInfo(connInfo),
    m_connectionFlags(CONNECTION_SYNCH) { }

DatabaseConnection::DatabaseConnection(ProducerConsumerQueue<SQLOperation*>* queue, DatabaseConnectionInfo& connInfo) :
    m_reconnecting(false),
    m_prepareError(false),
    m_queue(queue),
    m_connectionInfo(connInfo),
    m_connectionFlags(CONNECTION_ASYNC)
{
    m_worker = std::make_unique<DatabaseWorker>(m_queue, this);
}

DatabaseConnection::~DatabaseConnection()
{
    Close();
}

void DatabaseConnection::Close()
{
    // Stop the worker thread before the statements are cleared
    m_worker.reset();
    m_stmts.clear();

    if (m_backend)
    {
        m_backend->Close();
        m_backend.reset();
    }
}

DbError DatabaseConnection::Open(bool create)
{
    if (!m_backend)
    {
        m_backend = CreateBackend(m_connectionInfo);
        if (!m_backend)
        {
            DbError error{ DbErrorClass::Other, 0, Acore::StringFormat("{} backend is not available", DatabaseBackendName(GetBackend())) };
            SetError(error);
            return error;
        }
    }

    DbError error = m_backend->Open(create);
    if (error.IsError())
    {
        SetError(error);
        m_backend.reset();
    }

    return error;
}

bool DatabaseConnection::PrepareStatements()
{
    m_overrides.clear();
    DoPrepareStatementOverrides();
    DoPrepareStatements();
    return !m_prepareError;
}

std::string_view DatabaseConnection::Translate(std::string_view sql, std::string& buffer) const
{
    SqlDialect const& dialect = GetDialect(GetBackend());
    if (!dialect.NeedsTranslation(sql))
        return sql;

    buffer = dialect.Translate(sql);
    return buffer;
}

void DatabaseConnection::LogQueryError(std::string_view sql, std::string_view text, DbError const& error) const
{
    if (text.data() != sql.data())
        LOG_ERROR("sql.sql", "SQL: {}\n SQL ({}): {}\n [ERROR]: [{}] {}", sql, DatabaseBackendName(GetBackend()), text, error.native, error.message);
    else
        LOG_ERROR("sql.sql", "SQL: {}\n [ERROR]: [{}] {}", sql, error.native, error.message);
}

void DatabaseConnection::SetError(DbError const& error)
{
    m_lastError = error;
    if (!m_lastError.IsError())
        m_lastError.cls = DbErrorClass::Other;
}

bool DatabaseConnection::Execute(std::string_view sql)
{
    if (!m_backend)
    {
        SetError(NotConnectedError());
        return false;
    }

    std::string buffer;
    std::string_view const text = Translate(sql, buffer);

    uint32 _s = getMSTime();

    DbError error;
    if (!m_backend->Execute(text, error))
    {
        SetError(error);
        LogQueryError(sql, text, error);

        if (HandleError(error))  // If it returns true, an error was handled successfully (i.e. reconnection)
            return Execute(sql);       // Try again

        return false;
    }

    LOG_DEBUG("sql.sql", "[{} ms] SQL: {}", getMSTimeDiff(_s, getMSTime()), sql);
    return true;
}

bool DatabaseConnection::Execute(PreparedStatementBase* stmt)
{
    if (!m_backend)
    {
        SetError(NotConnectedError());
        return false;
    }

    uint32 index = stmt->GetIndex();

    IDbStatement* dbStmt = GetPreparedStatement(index);
    ASSERT(dbStmt); // Can only be null if preparation failed, server side error or bad query

    uint32 _s = getMSTime();

    DbError error;
    if (!m_backend->Execute(*dbStmt, stmt->GetParameters(), error))
    {
        SetError(error);
        LOG_ERROR("sql.sql", "SQL(p): {}\n [ERROR]: [{}] {}", GetQueryString(index, stmt), error.native, error.message);

        if (HandleError(error))  // If it returns true, an error was handled successfully (i.e. reconnection)
            return Execute(stmt);       // Try again

        return false;
    }

    LOG_DEBUG("sql.sql", "[{} ms] SQL(p): {}", getMSTimeDiff(_s, getMSTime()), GetQueryString(index, stmt));
    return true;
}

ResultSet* DatabaseConnection::Query(std::string_view sql)
{
    if (sql.empty())
        return nullptr;

    if (!m_backend)
    {
        SetError(NotConnectedError());
        return nullptr;
    }

    std::string buffer;
    std::string_view const text = Translate(sql, buffer);

    uint32 _s = getMSTime();

    DbError error;
    std::unique_ptr<RowSet> rows = m_backend->Query(text, error);
    if (!rows)
    {
        SetError(error);
        LogQueryError(sql, text, error);

        if (HandleError(error)) // If it returns true, an error was handled successfully (i.e. reconnection)
            return Query(sql);    // We try again

        return nullptr;
    }

    LOG_DEBUG("sql.sql", "[{} ms] SQL: {}", getMSTimeDiff(_s, getMSTime()), sql);
    return new ResultSet(std::move(rows));
}

PreparedResultSet* DatabaseConnection::Query(PreparedStatementBase* stmt)
{
    if (!m_backend)
    {
        SetError(NotConnectedError());
        return nullptr;
    }

    uint32 index = stmt->GetIndex();

    IDbStatement* dbStmt = GetPreparedStatement(index);
    ASSERT(dbStmt);            // Can only be null if preparation failed, server side error or bad query

    uint32 _s = getMSTime();

    DbError error;
    std::unique_ptr<RowSet> rows = m_backend->Query(*dbStmt, stmt->GetParameters(), error);
    if (!rows)
    {
        SetError(error);
        LOG_ERROR("sql.sql", "SQL(p): {}\n [ERROR]: [{}] {}", GetQueryString(index, stmt), error.native, error.message);

        if (HandleError(error))  // If it returns true, an error was handled successfully (i.e. reconnection)
            return Query(stmt);       // Try again

        return nullptr;
    }

    LOG_DEBUG("sql.sql", "[{} ms] SQL(p): {}", getMSTimeDiff(_s, getMSTime()), GetQueryString(index, stmt));
    return new PreparedResultSet(std::move(rows));
}

bool DatabaseConnection::BeginTransaction()
{
    DbError error = NotConnectedError();
    if (m_backend && m_backend->Begin(error))
        return true;

    SetError(error);
    LOG_ERROR("sql.sql", "Could not start a transaction: [{}] {}", error.native, error.message);

    if (HandleError(error))
        return BeginTransaction();

    return false;
}

void DatabaseConnection::RollbackTransaction()
{
    if (m_backend)
        m_backend->Rollback();
}

bool DatabaseConnection::CommitTransaction()
{
    DbError error = NotConnectedError();
    if (m_backend && m_backend->Commit(error))
        return true;

    SetError(error);
    LOG_ERROR("sql.sql", "Could not commit a transaction: [{}] {}", error.native, error.message);

    // The transaction is gone after a reconnect, so it is never retried from here
    HandleError(error);
    return false;
}

DbErrorClass DatabaseConnection::ExecuteTransaction(std::shared_ptr<TransactionBase> transaction)
{
    std::vector<SQLElementData> const& queries = transaction->m_queries;
    if (queries.empty())
        return DbErrorClass::Other;

    if (!BeginTransaction())
        return m_lastError.cls;

    for (auto const& data : queries)
    {
        switch (data.type)
        {
            case SQL_ELEMENT_PREPARED:
            {
                PreparedStatementBase* stmt = nullptr;

                try
                {
                    stmt = std::get<PreparedStatementBase*>(data.element);
                }
                catch (std::bad_variant_access const& ex)
                {
                    LOG_FATAL("sql.sql", "> PreparedStatementBase not found in SQLElementData. {}", ex.what());
                    ABORT();
                }

                ASSERT(stmt);

                if (!Execute(stmt))
                {
                    LOG_WARN("sql.sql", "Transaction aborted. {} queries not executed.", queries.size());
                    DbErrorClass errorClass = m_lastError.cls;
                    RollbackTransaction();
                    return errorClass;
                }
            }
            break;
            case SQL_ELEMENT_RAW:
            {
                std::string sql{};

                try
                {
                    sql = std::get<std::string>(data.element);
                }
                catch (std::bad_variant_access const& ex)
                {
                    LOG_FATAL("sql.sql", "> std::string not found in SQLElementData. {}", ex.what());
                    ABORT();
                }

                ASSERT(!sql.empty());

                if (!Execute(sql))
                {
                    LOG_WARN("sql.sql", "Transaction aborted. {} queries not executed.", queries.size());
                    DbErrorClass errorClass = m_lastError.cls;
                    RollbackTransaction();
                    return errorClass;
                }
            }
            break;
        }
    }

    // we might encounter errors during certain queries, and depending on the kind of error
    // we might want to restart the transaction. So to prevent data loss, we only clean up when it's all done.
    // This is done in calling functions DatabaseWorkerPool<T>::DirectCommitTransaction and TransactionTask::Execute,
    // and not while iterating over every element.

    if (!CommitTransaction())
    {
        DbErrorClass errorClass = m_lastError.cls;
        RollbackTransaction();
        return errorClass;
    }

    return DbErrorClass::None;
}

void DatabaseConnection::Ping()
{
    if (m_backend)
        m_backend->Ping();
}

bool DatabaseConnection::LockIfReady()
{
    return m_Mutex.try_lock();
}

void DatabaseConnection::Unlock()
{
    m_Mutex.unlock();
}

std::string DatabaseConnection::GetServerInfo() const
{
    return m_backend ? m_backend->ServerInfo() : std::string();
}

IDbStatement* DatabaseConnection::GetPreparedStatement(uint32 index)
{
    ASSERT(index < m_stmts.size(), "Tried to access invalid prepared statement index {} (max index {}) on database `{}`, connection type: {}",
        index, m_stmts.size(), m_connectionInfo.database, (m_connectionFlags & CONNECTION_ASYNC) ? "asynchronous" : "synchronous");

    IDbStatement* ret = m_stmts[index].get();

    if (!ret)
        LOG_ERROR("sql.sql", "Could not fetch prepared statement {} on database `{}`, connection type: {}.",
            index, m_connectionInfo.database, (m_connectionFlags & CONNECTION_ASYNC) ? "asynchronous" : "synchronous");

    return ret;
}

void DatabaseConnection::OverrideStatement(DatabaseBackend backend, uint32 index, std::string_view sql)
{
    if (backend == GetBackend())
        m_overrides[index] = std::string(sql);
}

void DatabaseConnection::PrepareStatement(uint32 index, std::string_view sql, ConnectionFlags flags)
{
    // Check if specified query should be prepared on this connection
    // i.e. don't prepare async statements on synchronous connections
    // to save memory that will not be used.
    if (!(m_connectionFlags & flags))
    {
        m_stmts[index].reset();
        return;
    }

    std::string text;
    auto itr = m_overrides.find(index);
    if (itr != m_overrides.end())
        text = itr->second;
    else
    {
        SqlDialect const& dialect = GetDialect(GetBackend());
        text = dialect.NeedsTranslation(sql) ? dialect.Translate(sql) : std::string(sql);
    }

    if (m_queries.size() < m_stmts.size())
        m_queries.resize(m_stmts.size());

    DbError error = NotConnectedError();
    std::unique_ptr<IDbStatement> stmt = m_backend ? m_backend->Prepare(text, error) : nullptr;
    if (!stmt)
    {
        LOG_ERROR("sql.sql", "Could not prepare statement id: {}, sql: \"{}\"", index, text);
        LOG_ERROR("sql.sql", "[{}] {}", error.native, error.message);
        m_prepareError = true;
        return;
    }

    if (text != sql)
    {
        uint32 const expected = CountParameters(sql);
        if (stmt->GetParameterCount() != expected)
            ABORT("Prepared statement {} on database `{}` has {} parameters as \"{}\" but {} as \"{}\"",
                index, m_connectionInfo.database, stmt->GetParameterCount(), text, expected, sql);
    }

    m_queries[index] = std::move(text);
    m_stmts[index] = std::move(stmt);
}

std::string DatabaseConnection::GetQueryString(uint32 index, PreparedStatementBase const* stmt) const
{
    std::string queryString = index < m_queries.size() ? m_queries[index] : std::string();

    std::size_t pos = 0;

    for (PreparedStatementData const& data : stmt->GetParameters())
    {
        pos = queryString.find('?', pos);
        if (pos == std::string::npos)
            break;

        std::string replaceStr = std::visit([&](auto&& data)
        {
            return PreparedStatementData::ToString(data);
        }, data.data);

        queryString.replace(pos, 1, replaceStr);
        pos += replaceStr.length();
    }

    return queryString;
}

bool DatabaseConnection::HandleError(DbError const& error, uint8 attempts /*= 5*/)
{
    std::string str = "";
    switch (error.cls)
    {
        case DbErrorClass::ConnectionLost:
        {
            if (!m_backend || !GetBackendCaps(GetBackend()).supportsReconnect)
                return false;

            LOG_ERROR("sql.sql", "Lost the connection to the {} server!", DatabaseBackendName(GetBackend()));

            m_reconnecting = true;

            for (;;)
            {
                LOG_INFO("sql.sql", "Attempting to reconnect to the {} server...", DatabaseBackendName(GetBackend()));

                DbError reconnectError;
                if (m_backend->Reconnect(reconnectError))
                {
                    // Don't remove 'this' pointer unless you want to skip loading all prepared statements...
                    if (!this->PrepareStatements())
                    {
                        str = "Could not re-prepare statements!";
                        LOG_FATAL("sql.sql", "{}", str);
                        std::this_thread::sleep_for(10s);
                        ABORT("{}\n\n[{}] {}", str, error.native, error.message);
                    }

                    LOG_INFO("sql.sql", "Successfully reconnected to {} @{}:{} ({}).",
                        m_connectionInfo.database, m_connectionInfo.host, m_connectionInfo.port_or_socket,
                            (m_connectionFlags & CONNECTION_ASYNC) ? "asynchronous" : "synchronous");

                    m_reconnecting = false;
                    return true;
                }

                if ((--attempts) == 0)
                {
                    // Shut down the server when the database server isn't
                    // reachable for some time
                    str = "Failed to reconnect to the database server, terminating the server to prevent data corruption!";
                    LOG_FATAL("sql.sql", "{}", str);

                    // We could also initiate a shutdown through using std::raise(SIGTERM)
                    std::this_thread::sleep_for(10s);
                    ABORT("{}\n\n[{}] {}", str, reconnectError.native, reconnectError.message);
                }

                // It's possible this attempted reconnect fails again right away.
                // To prevent hammering the server, sleep here.
                std::this_thread::sleep_for(3s);
            }
        }

        case DbErrorClass::Retryable:
            return false; // Implemented in TransactionTask::Execute and DatabaseWorkerPool<T>::DirectCommitTransaction

        // Query related errors - skip query
        case DbErrorClass::Constraint:
            return false;

        // Fail only this query; statements that fail to prepare still stop the server (m_prepareError)
        case DbErrorClass::SchemaMismatch:
        case DbErrorClass::Syntax:
            return false;
        default:
            LOG_ERROR("sql.sql", "Unhandled {} error {}. Unexpected behaviour possible.", DatabaseBackendName(GetBackend()), error.native);
            return false;
    }
}
