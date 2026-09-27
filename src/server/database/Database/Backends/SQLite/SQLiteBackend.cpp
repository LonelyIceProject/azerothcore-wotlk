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

#include "SQLiteBackend.h"
#include "Config.h"
#include "Log.h"
#include "SQLiteStatement.h"
#include "SqliteFunctions.h"
#include "StringFormat.h"
#include "Util.h"
#include <sqlite3.h>
#include <array>
#include <cctype>
#include <filesystem>

namespace
{
    std::filesystem::path Utf8Path(std::string_view path)
    {
        return std::filesystem::path(std::u8string_view(reinterpret_cast<char8_t const*>(path.data()), path.size()));
    }

    bool IsSpecialPath(std::string_view path)
    {
        return path == ":memory:" || StringStartsWithI(path, "file:");
    }

    // Read-write URI that fails instead of creating a missing file.
    std::string MakeAttachUri(std::string_view path)
    {
        if (IsSpecialPath(path))
            return std::string(path);

        std::string uri = "file:";
        if (path.size() >= 2 && std::isalpha(uint8(path[0])) && path[1] == ':')
            uri += "///";

        for (char c : path)
        {
            if (c == '\\')
                uri += '/';
            else if (c == '%' || c == '?' || c == '#')
                uri += Acore::StringFormat("%{:02X}", uint8(c));
            else
                uri += c;
        }

        uri += "?mode=rw";
        return uri;
    }

    std::string GetSynchronousMode()
    {
        static constexpr std::array<std::string_view, 4> Modes = { "OFF", "NORMAL", "FULL", "EXTRA" };

        std::string const mode = sConfigMgr->GetOption<std::string>("Database.SQLite.Synchronous", "NORMAL", false);
        for (std::string_view known : Modes)
            if (StringEqualI(mode, known))
                return std::string(known);

        LOG_ERROR("sql.driver", "Invalid Database.SQLite.Synchronous '{}', using NORMAL", mode);
        return "NORMAL";
    }

    class StatementReset
    {
    public:
        explicit StatementReset(sqlite3_stmt* stmt) : _stmt(stmt) { }
        ~StatementReset()
        {
            sqlite3_reset(_stmt);
            sqlite3_clear_bindings(_stmt);
        }

        StatementReset(StatementReset const&) = delete;
        StatementReset& operator=(StatementReset const&) = delete;

    private:
        sqlite3_stmt* _stmt;
    };
}

SQLiteBackend::SQLiteBackend(DatabaseConnectionInfo const& info) :
    _info(info),
    _db(nullptr)
{
}

SQLiteBackend::~SQLiteBackend()
{
    Close();
}

DbError SQLiteBackend::Open(bool create)
{
    Close();

    if (_info.path.empty())
        return { DbErrorClass::Other, SQLITE_MISUSE, "SQLite database path is empty" };

    if (!IsSpecialPath(_info.path))
    {
        std::filesystem::path const path = Utf8Path(_info.path);
        std::error_code ec;
        if (!create && !std::filesystem::exists(path, ec))
            return { DbErrorClass::DatabaseMissing, SQLITE_CANTOPEN, Acore::StringFormat("Database file '{}' does not exist", _info.path) };

        if (create && path.has_parent_path())
            std::filesystem::create_directories(path.parent_path(), ec);
    }

    int const flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI | SQLITE_OPEN_EXRESCODE | (create ? SQLITE_OPEN_CREATE : 0);
    int const rc = sqlite3_open_v2(_info.path.c_str(), &_db, flags, nullptr);
    if (rc != SQLITE_OK)
    {
        DbError err = SQLite::MakeError(_db, rc);
        err.message = Acore::StringFormat("Cannot open '{}': {}", _info.path, err.message);
        sqlite3_close_v2(_db);
        _db = nullptr;
        return err;
    }

    sqlite3_extended_result_codes(_db, 1);

    DbError err = Configure();
    if (err.IsError())
    {
        sqlite3_close_v2(_db);
        _db = nullptr;
    }

    return err;
}

DbError SQLiteBackend::Configure()
{
    sqlite3_busy_timeout(_db, sConfigMgr->GetOption<int32>("Database.SQLite.BusyTimeoutMs", 10000, false));

    DbError err;
    std::unique_ptr<RowSet> journal;
    if (!Run("PRAGMA journal_mode=WAL", &journal, err))
        return err;

    if (journal && journal->GetRowCount() && journal->GetFieldCount())
    {
        std::string_view const mode = journal->Get(0, 0).AsBytes();
        if (!StringEqualI(mode, "wal") && !StringEqualI(mode, "memory"))
            LOG_WARN("sql.driver", "SQLite database '{}' stays in journal mode '{}', concurrent access will block", _info.path, mode);
    }

    std::string const pragmas = Acore::StringFormat("PRAGMA synchronous={}; PRAGMA temp_store=MEMORY; PRAGMA cache_size=-65536; PRAGMA foreign_keys=ON;",
        GetSynchronousMode());
    if (!Run(pragmas, nullptr, err))
        return err;

    for (auto const& [alias, path] : _info.attach)
    {
        err = Attach(alias, path);
        if (err.IsError())
            return err;
    }

    int const rc = RegisterMySqlFunctions(_db);
    if (rc != SQLITE_OK)
    {
        err = SQLite::MakeError(_db, rc);
        err.message = Acore::StringFormat("Cannot register SQL functions: {}", err.message);
        return err;
    }

    return err;
}

DbError SQLiteBackend::Attach(std::string const& alias, std::string const& path)
{
    DbError err;
    std::unique_ptr<IDbStatement> stmt = Prepare("ATTACH DATABASE ? AS " + SQLite::QuoteIdentifier(alias), err);
    if (!stmt)
        return err;

    std::array<PreparedStatementData, 1> const params = { PreparedStatementData{ MakeAttachUri(path) } };
    if (!Execute(*stmt, params, err))
    {
        if (err.cls != DbErrorClass::Retryable)
            err.cls = DbErrorClass::Other;
        err.message = Acore::StringFormat("Cannot attach '{}' as '{}': {}", path, alias, err.message);
    }

    return err;
}

void SQLiteBackend::Close()
{
    if (!_db)
        return;

    sqlite3_busy_timeout(_db, 0);
    sqlite3_exec(_db, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr, nullptr);
    sqlite3_close_v2(_db);
    _db = nullptr;
}

bool SQLiteBackend::IsOpen(DbError& err) const
{
    if (_db)
        return true;

    err = { DbErrorClass::ConnectionLost, SQLITE_MISUSE, "SQLite connection is not open" };
    return false;
}

std::unique_ptr<IDbStatement> SQLiteBackend::Prepare(std::string_view sql, DbError& err)
{
    if (!IsOpen(err))
        return nullptr;

    sqlite3_stmt* handle = nullptr;
    char const* tail = nullptr;
    int const rc = sqlite3_prepare_v3(_db, sql.data(), int(sql.size()), SQLITE_PREPARE_PERSISTENT, &handle, &tail);
    if (rc != SQLITE_OK)
    {
        err = SQLite::MakeError(_db, rc);
        return nullptr;
    }

    if (!handle)
    {
        err = { DbErrorClass::Syntax, SQLITE_MISUSE, "Empty statement" };
        return nullptr;
    }

    std::unique_ptr<SQLiteStatement> stmt = std::make_unique<SQLiteStatement>(handle);

    char const* end = sql.data() + sql.size();
    if (tail && tail < end)
    {
        sqlite3_stmt* extra = nullptr;
        sqlite3_prepare_v2(_db, tail, int(end - tail), &extra, nullptr);
        if (extra)
        {
            sqlite3_finalize(extra);
            err = { DbErrorClass::Syntax, SQLITE_MISUSE, "Prepared statement contains more than one statement" };
            return nullptr;
        }
    }

    return stmt;
}

bool SQLiteBackend::Execute(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err)
{
    if (!IsOpen(err))
        return false;

    sqlite3_stmt* handle = static_cast<SQLiteStatement&>(stmt).GetHandle();
    StatementReset reset(handle);

    return SQLite::BindParameters(handle, params, err) && SQLite::Run(handle, err);
}

std::unique_ptr<RowSet> SQLiteBackend::Query(IDbStatement& stmt, std::span<PreparedStatementData const> params, DbError& err)
{
    if (!IsOpen(err))
        return nullptr;

    sqlite3_stmt* handle = static_cast<SQLiteStatement&>(stmt).GetHandle();
    StatementReset reset(handle);

    if (!SQLite::BindParameters(handle, params, err))
        return nullptr;

    return SQLite::Fetch(handle, err);
}

bool SQLiteBackend::Run(std::string_view sql, std::unique_ptr<RowSet>* result, DbError& err)
{
    if (!IsOpen(err))
        return false;

    char const* pos = sql.data();
    char const* const end = pos + sql.size();
    while (pos < end)
    {
        sqlite3_stmt* handle = nullptr;
        char const* tail = nullptr;
        int const rc = sqlite3_prepare_v2(_db, pos, int(end - pos), &handle, &tail);
        if (rc != SQLITE_OK)
        {
            err = SQLite::MakeError(_db, rc);
            return false;
        }

        if (!tail || tail <= pos)
            tail = end;

        pos = tail;
        if (!handle)
            continue;

        SQLiteStatement stmt(handle);
        if (result && sqlite3_column_count(handle) > 0)
        {
            std::unique_ptr<RowSet> rows = SQLite::Fetch(handle, err);
            if (!rows)
                return false;

            *result = std::move(rows);
        }
        else if (!SQLite::Run(handle, err))
            return false;
    }

    return true;
}

bool SQLiteBackend::Execute(std::string_view sql, DbError& err)
{
    return Run(sql, nullptr, err);
}

std::unique_ptr<RowSet> SQLiteBackend::Query(std::string_view sql, DbError& err)
{
    std::unique_ptr<RowSet> result;
    if (!Run(sql, &result, err))
        return nullptr;

    if (!result)
        result = std::make_unique<RowSet>(std::vector<QueryResultFieldMetadata>());

    return result;
}

bool SQLiteBackend::Begin(DbError& err)
{
    return Run("BEGIN IMMEDIATE", nullptr, err);
}

bool SQLiteBackend::Commit(DbError& err)
{
    return Run("COMMIT", nullptr, err);
}

void SQLiteBackend::Rollback()
{
    if (_db && !sqlite3_get_autocommit(_db))
        sqlite3_exec(_db, "ROLLBACK", nullptr, nullptr, nullptr);
}

std::unique_ptr<RowSet> SQLiteBackend::QueryText(std::string_view sql, std::initializer_list<std::string_view> params)
{
    DbError err;
    std::unique_ptr<IDbStatement> stmt = Prepare(sql, err);
    if (!stmt)
    {
        LOG_ERROR("sql.sql", "SQLite: {} [{}]", err.message, sql);
        return nullptr;
    }

    std::vector<PreparedStatementData> data;
    data.reserve(params.size());
    for (std::string_view param : params)
        data.push_back({ std::string(param) });

    std::unique_ptr<RowSet> result = Query(*stmt, data, err);
    if (!result)
        LOG_ERROR("sql.sql", "SQLite: {} [{}]", err.message, sql);

    return result;
}

std::string SQLiteBackend::ServerInfo() const
{
    return sqlite3_libversion();
}

std::unique_ptr<IDbConnectionBackend> CreateSQLiteBackend(DatabaseConnectionInfo const& info)
{
    return std::make_unique<SQLiteBackend>(info);
}
