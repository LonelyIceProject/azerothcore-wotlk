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

#ifndef _DATABASECONNECTION_H
#define _DATABASECONNECTION_H

#include "DatabaseBackend.h"
#include "DatabaseConnectionInfo.h"
#include "DatabaseEnvFwd.h"
#include "Define.h"
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

template <typename T>
class ProducerConsumerQueue;

class DatabaseWorker;
class IDbConnectionBackend;
class IDbStatement;
class SQLOperation;

enum ConnectionFlags
{
    CONNECTION_ASYNC = 0x1,
    CONNECTION_SYNCH = 0x2,
    CONNECTION_BOTH = CONNECTION_ASYNC | CONNECTION_SYNCH
};

class AC_DATABASE_API DatabaseConnection
{
template <class T>
friend class DatabaseWorkerPool;

friend class ModuleDatabasePool;
friend class PingOperation;

public:
    DatabaseConnection(DatabaseConnectionInfo& connInfo);                                               //! Constructor for synchronous connections.
    DatabaseConnection(ProducerConsumerQueue<SQLOperation*>* queue, DatabaseConnectionInfo& connInfo);  //! Constructor for asynchronous connections.
    virtual ~DatabaseConnection();

    DbError Open(bool create = false);
    void Close();

    bool PrepareStatements();

    bool Execute(std::string_view sql);
    bool Execute(PreparedStatementBase* stmt);
    ResultSet* Query(std::string_view sql);
    PreparedResultSet* Query(PreparedStatementBase* stmt);

    bool BeginTransaction();
    void RollbackTransaction();
    bool CommitTransaction();
    DbErrorClass ExecuteTransaction(std::shared_ptr<TransactionBase> transaction);
    void Ping();

    [[nodiscard]] DbError const& GetLastError() const { return m_lastError; }
    [[nodiscard]] DatabaseBackend GetBackend() const { return m_connectionInfo.backend; }
    [[nodiscard]] IDbConnectionBackend* GetBackendConnection() const { return m_backend.get(); }

protected:
    /// Tries to acquire lock. If lock is acquired by another thread
    /// the calling parent will just try another connection
    bool LockIfReady();

    /// Called by parent databasepool. Will let other threads access this connection
    void Unlock();

    [[nodiscard]] std::string GetServerInfo() const;
    IDbStatement* GetPreparedStatement(uint32 index);
    void PrepareStatement(uint32 index, std::string_view sql, ConnectionFlags flags);
    //! Replaces the translated text of statement index on the given backend; call from DoPrepareStatementOverrides().
    void OverrideStatement(DatabaseBackend backend, uint32 index, std::string_view sql);

    virtual void DoPrepareStatements() = 0;
    virtual void DoPrepareStatementOverrides() { }
    virtual bool HandleError(DbError const& error, uint8 attempts = 5);

    typedef std::vector<std::unique_ptr<IDbStatement>> PreparedStatementContainer;

    PreparedStatementContainer m_stmts; //! PreparedStatements storage
    bool m_reconnecting;  //! Are we reconnecting?
    bool m_prepareError;  //! Was there any error while preparing statements?

private:
    std::string_view Translate(std::string_view sql, std::string& buffer) const;
    std::string GetQueryString(uint32 index, PreparedStatementBase const* stmt) const;
    void SetError(DbError const& error);

    std::unique_ptr<IDbConnectionBackend> m_backend;    //! Physical connection
    std::vector<std::string> m_queries;                 //! Prepared statement text as sent to the backend
    std::unordered_map<uint32, std::string> m_overrides;
    DbError m_lastError;
    ProducerConsumerQueue<SQLOperation*>* m_queue;      //! Queue shared with other asynchronous connections.
    std::unique_ptr<DatabaseWorker> m_worker;           //! Core worker task.
    DatabaseConnectionInfo& m_connectionInfo;           //! Connection info (used for logging)
    ConnectionFlags m_connectionFlags;                  //! Connection flags (for preparing relevant statements)
    std::mutex m_Mutex;

    DatabaseConnection(DatabaseConnection const& right) = delete;
    DatabaseConnection& operator=(DatabaseConnection const& right) = delete;
};

#endif
