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

#include "MySQLBackend.h"
#include "Errors.h"
#include "Log.h"
#include "MySQLScriptTarget.h"
#include "MySQLStatement.h"
#include "SqlDialect.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include <errmsg.h>
#include <mysqld_error.h>
#include <sstream>

/**
* @def MIN_MYSQL_CLIENT_VERSION
* The minimum MySQL Client Version
*/
#define MIN_MYSQL_CLIENT_VERSION 80000u

/**
* @def MIN_MYSQL_SERVER_VERSION
* The minimum MySQL Server Version
*/
#define MIN_MYSQL_SERVER_VERSION "8.0.0"

namespace
{
    /**
    * @brief Returns true if the version string given is incompatible
    *
    * Intended to be used with mysql_get_server_info()'s output as the source
    *
    * DatabaseIncompatibleVersion("8.0.35") => false
    * DatabaseIncompatibleVersion("5.6.6") => true
    *
    * Adapted from stackoverflow response
    * https://stackoverflow.com/a/2941508
    *
    * @param mysqlVersion The output from mysql_get_server_info()
    * @return Returns true if the Server version is incompatible
    */
    bool DatabaseIncompatibleVersion(std::string const mysqlVersion)
    {
        // anon func to turn a version string into an array of uint8
        // "1.2.3" => [1, 2, 3]
        auto parse = [](std::string const& input)
        {
            std::vector<uint8> result;
            std::istringstream parser(input);
            result.push_back(parser.get());
            for (int i = 1; i < 3; i++)
            {
                // Skip period
                parser.get();
                // Append int from parser to output
                result.push_back(parser.get());
            }
            return result;
        };

        // default to values for MySQL
        uint8 offset = 0;
        std::string minVersion = MIN_MYSQL_SERVER_VERSION;

        auto parsedMySQLVersion = parse(mysqlVersion.substr(offset));
        auto parsedMinVersion = parse(minVersion);

        return std::lexicographical_compare(parsedMySQLVersion.begin(), parsedMySQLVersion.end(),
                                            parsedMinVersion.begin(), parsedMinVersion.end());
    }

    std::string QuoteString(std::string_view value)
    {
        return Acore::StringFormat("'{}'", MySqlEscape(value));
    }
}

DbError MakeMySQLError(uint32 errNo, char const* message)
{
    DbError error;
    error.native = int32(errNo);
    error.message = message ? message : "";

    switch (errNo)
    {
        case 0:
            error.cls = DbErrorClass::Other;
            break;
        case CR_SERVER_GONE_ERROR:
        case CR_SERVER_LOST:
        case CR_SERVER_LOST_EXTENDED:
        case CR_CONN_HOST_ERROR:
        case CR_CONNECTION_ERROR:
            error.cls = DbErrorClass::ConnectionLost;
            break;
        case ER_BAD_DB_ERROR:
            error.cls = DbErrorClass::DatabaseMissing;
            break;
        case ER_LOCK_DEADLOCK:
            error.cls = DbErrorClass::Retryable;
            break;
        case ER_WRONG_VALUE_COUNT:
        case ER_DUP_ENTRY:
            error.cls = DbErrorClass::Constraint;
            break;
        case ER_BAD_FIELD_ERROR:
        case ER_NO_SUCH_TABLE:
            error.cls = DbErrorClass::SchemaMismatch;
            break;
        case ER_PARSE_ERROR:
            error.cls = DbErrorClass::Syntax;
            break;
        default:
            error.cls = DbErrorClass::Other;
            break;
    }

    return error;
}

std::unique_ptr<IDbConnectionBackend> CreateMySQLBackend(DatabaseConnectionInfo const& info)
{
    return std::make_unique<MySQLBackend>(info);
}

MySQLBackend::MySQLBackend(DatabaseConnectionInfo const& info) :
    _info(info),
    _mysql(nullptr),
    _connectedBefore(false) { }

MySQLBackend::~MySQLBackend()
{
    Close();
}

DbError MySQLBackend::LastError() const
{
    if (!_mysql)
        return MakeMySQLError(CR_SERVER_GONE_ERROR, "MySQL server has gone away");

    return MakeMySQLError(mysql_errno(_mysql), mysql_error(_mysql));
}

DbError MySQLBackend::Open(bool create)
{
    Close();

    MYSQL* mysqlInit = mysql_init(nullptr);
    if (!mysqlInit)
    {
        LOG_ERROR("sql.driver", "Could not initialize Mysql connection to database `{}`", _info.database);
        return MakeMySQLError(CR_UNKNOWN_ERROR, "mysql_init failed");
    }

    uint32 port;
    char const* unix_socket;

    mysql_options(mysqlInit, MYSQL_SET_CHARSET_NAME, "utf8");

#ifdef _WIN32
    if (_info.host == ".")                                           // named pipe use option (Windows)
    {
        unsigned int opt = MYSQL_PROTOCOL_PIPE;
        mysql_options(mysqlInit, MYSQL_OPT_PROTOCOL, (char const*)&opt);
        port = 0;
        unix_socket = 0;
    }
    else                                                    // generic case
    {
        port = Acore::StringTo<uint32>(_info.port_or_socket).value_or(0);
        unix_socket = 0;
    }
#else
    if (_info.host == ".")                                           // socket use option (Unix/Linux)
    {
        unsigned int opt = MYSQL_PROTOCOL_SOCKET;
        mysql_options(mysqlInit, MYSQL_OPT_PROTOCOL, (char const*)&opt);
        _info.host = "localhost";
        port = 0;
        unix_socket = _info.port_or_socket.c_str();
    }
    else                                                    // generic case
    {
        port = Acore::StringTo<uint32>(_info.port_or_socket).value_or(0);
        unix_socket = nullptr;
    }
#endif

    if (_info.ssl != "")
    {
        mysql_ssl_mode opt_use_ssl = SSL_MODE_DISABLED;
        if (_info.ssl == "ssl")
        {
            opt_use_ssl = SSL_MODE_REQUIRED;
        }

        mysql_options(mysqlInit, MYSQL_OPT_SSL_MODE, (char const*)&opt_use_ssl);
    }

    // create: the database does not exist yet, so the server is reached without one and it is made below
    _mysql = mysql_real_connect(mysqlInit, _info.host.c_str(), _info.user.c_str(),
        _info.password.c_str(), create ? nullptr : _info.database.c_str(), port, unix_socket, 0);

    if (!_mysql)
    {
        LOG_ERROR("sql.driver", "Could not connect to MySQL database at {}: {}", _info.host, mysql_error(mysqlInit));
        uint32 const errNo = mysql_errno(mysqlInit);
        DbError error = MakeMySQLError(errNo, mysql_error(mysqlInit));
        if (!_connectedBefore && errNo != CR_CONNECTION_ERROR && error.cls == DbErrorClass::ConnectionLost)
            error.cls = DbErrorClass::Other;

        mysql_close(mysqlInit);
        return error;
    }

    if (!_connectedBefore)
    {
        LOG_INFO("sql.sql", "MySQL client library: {}", mysql_get_client_info());
        LOG_INFO("sql.sql", "MySQL server ver: {} ", mysql_get_server_info(_mysql));
    }

    if (DatabaseIncompatibleVersion(mysql_get_server_info(_mysql)))
    {
        LOG_ERROR("sql.driver", "AzerothCore does not support MySQL versions below 8.0\n\nFound server version: {}. Server compiled with: {}.",
            mysql_get_server_info(_mysql), MYSQL_VERSION_ID);
        Close();
        return { DbErrorClass::Other, 0, "unsupported MySQL server version" };
    }

    _connectedBefore = true;

    LOG_INFO("sql.sql", "Connected to MySQL database at {}", _info.host);
    mysql_autocommit(_mysql, 1);

    // set connection properties to UTF8 to properly handle locales for different
    // server configs - core sends data in UTF8, so MySQL must expect UTF8 too
    mysql_set_character_set(_mysql, "utf8mb4");

    if (create)
    {
        DbError err;
        if (!Execute(Acore::StringFormat("CREATE DATABASE IF NOT EXISTS `{}` DEFAULT CHARACTER SET UTF8MB4 COLLATE utf8mb4_general_ci", _info.database), err))
        {
            Close();
            return err;
        }

        if (mysql_select_db(_mysql, _info.database.c_str()))
        {
            err = LastError();
            Close();
            return err;
        }
    }

    return {};
}

void MySQLBackend::Close()
{
    if (_mysql)
    {
        mysql_close(_mysql);
        _mysql = nullptr;
    }
}

bool MySQLBackend::Reconnect(DbError& err)
{
    err = Open(false);
    return !err.IsError();
}

void MySQLBackend::Ping()
{
    if (_mysql)
        mysql_ping(_mysql);
}

std::unique_ptr<IDbStatement> MySQLBackend::Prepare(std::string_view sql, DbError& err)
{
    if (!_mysql)
    {
        err = LastError();
        return nullptr;
    }

    MYSQL_STMT* stmt = mysql_stmt_init(_mysql);
    if (!stmt)
    {
        err = LastError();
        return nullptr;
    }

    if (mysql_stmt_prepare(stmt, sql.data(), static_cast<unsigned long>(sql.size())))
    {
        err = MakeMySQLError(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
        mysql_stmt_close(stmt);
        return nullptr;
    }

    return std::make_unique<MySQLStatement>(stmt, sql);
}

bool MySQLBackend::ExecuteStatement(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err)
{
    if (!_mysql)
    {
        err = LastError();
        return false;
    }

    MySQLStatement& mysqlStmt = static_cast<MySQLStatement&>(stmt);
    mysqlStmt.BindParameters(params);

    MYSQL_STMT* msql_STMT = mysqlStmt.GetSTMT();
    MYSQL_BIND* msql_BIND = mysqlStmt.GetBind();

#if MYSQL_VERSION_ID >= 80300
    if (mysql_stmt_bind_named_param(msql_STMT, msql_BIND, mysqlStmt.GetParameterCount(), nullptr))
#else
    if (mysql_stmt_bind_param(msql_STMT, msql_BIND))
#endif
    {
        err = MakeMySQLError(mysql_stmt_errno(msql_STMT), mysql_stmt_error(msql_STMT));
        mysqlStmt.ClearParameters();
        return false;
    }

    if (mysql_stmt_execute(msql_STMT))
    {
        err = MakeMySQLError(mysql_stmt_errno(msql_STMT), mysql_stmt_error(msql_STMT));
        mysqlStmt.ClearParameters();
        return false;
    }

    mysqlStmt.ClearParameters();
    return true;
}

bool MySQLBackend::Execute(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err)
{
    return ExecuteStatement(stmt, params, err);
}

std::unique_ptr<RowSet> MySQLBackend::Query(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err)
{
    if (!ExecuteStatement(stmt, params, err))
        return nullptr;

    std::unique_ptr<RowSet> rows = static_cast<MySQLStatement&>(stmt).FetchResult(err);

    if (mysql_more_results(_mysql))
    {
        mysql_next_result(_mysql);
    }

    return rows;
}

bool MySQLBackend::Execute(std::string_view sql, DbError& err)
{
    if (!_mysql || mysql_real_query(_mysql, sql.data(), static_cast<unsigned long>(sql.size())))
    {
        err = LastError();
        return false;
    }

    if (MYSQL_RES* result = mysql_store_result(_mysql))
        mysql_free_result(result);

    return true;
}

std::unique_ptr<RowSet> MySQLBackend::Query(std::string_view sql, DbError& err)
{
    if (!_mysql || mysql_real_query(_mysql, sql.data(), static_cast<unsigned long>(sql.size())))
    {
        err = LastError();
        return nullptr;
    }

    MYSQL_RES* result = mysql_store_result(_mysql);
    if (!result)
    {
        if (mysql_field_count(_mysql))
        {
            err = LastError();
            return nullptr;
        }

        return std::make_unique<RowSet>(std::vector<QueryResultFieldMetadata>());
    }

    std::unique_ptr<RowSet> rows = MySQLFetchResult(result);
    mysql_free_result(result);
    return rows;
}

bool MySQLBackend::Begin(DbError& err)
{
    return Execute("START TRANSACTION", err);
}

bool MySQLBackend::Commit(DbError& err)
{
    return Execute("COMMIT", err);
}

void MySQLBackend::Rollback()
{
    DbError err;
    Execute("ROLLBACK", err);
}

bool MySQLBackend::HasAnyTable()
{
    DbError err;
    std::unique_ptr<RowSet> rows = Query("SHOW TABLES", err);
    return rows && rows->GetRowCount() > 0;
}

bool MySQLBackend::TableExists(std::string_view table)
{
    DbError err;
    std::unique_ptr<RowSet> rows = Query(Acore::StringFormat("SELECT 1 FROM information_schema.tables WHERE table_schema = DATABASE() AND table_name = {}",
        QuoteString(table)), err);
    return rows && rows->GetRowCount() > 0;
}

std::vector<std::string> MySQLBackend::ListColumns(std::string_view table)
{
    std::vector<std::string> columns;

    DbError err;
    std::unique_ptr<RowSet> rows = Query(Acore::StringFormat("SELECT column_name FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = {} ORDER BY ordinal_position",
        QuoteString(table)), err);
    if (!rows)
        return columns;

    for (uint64 i = 0; i < rows->GetRowCount(); ++i)
        columns.emplace_back(rows->Get(i, 0).AsBytes());

    return columns;
}

std::unique_ptr<IScriptTarget> MySQLBackend::CreateScriptTarget()
{
    return std::make_unique<MySQLScriptTarget>(*this);
}

std::string MySQLBackend::ServerInfo() const
{
    return _mysql ? mysql_get_server_info(_mysql) : "";
}

void MySQLLibrary::Init()
{
    mysql_library_init(-1, nullptr, nullptr);

    WPFatal(mysql_thread_safe(), "Used MySQL library isn't thread-safe.");

    bool isSupportClientDB = mysql_get_client_version() >= MIN_MYSQL_CLIENT_VERSION;
    bool isSameClientDB = mysql_get_client_version() == MYSQL_VERSION_ID;

    WPFatal(isSupportClientDB, "AzerothCore does not support MySQL versions below 8.0\n\nFound version: {} / {}. Server compiled with: {}.\nSearch the wiki for ACE00043 in Common Errors (https://www.azerothcore.org/wiki/common-errors#ace00043).",
        mysql_get_client_info(), mysql_get_client_version(), MYSQL_VERSION_ID);
    WPFatal(isSameClientDB, "Used MySQL library version ({} id {}) does not match the version id used to compile AzerothCore (id {}).\nSearch the wiki for ACE00046 in Common Errors (https://www.azerothcore.org/wiki/common-errors#ace00046).",
        mysql_get_client_info(), mysql_get_client_version(), MYSQL_VERSION_ID);
}

void MySQLLibrary::End()
{
    mysql_library_end();
}

std::string MySQLLibrary::Version()
{
    return Acore::StringFormat("MySQL {}", mysql_get_client_info());
}
