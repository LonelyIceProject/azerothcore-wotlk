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

#include "Commands.h"
#include "ScriptRunner.h"
#include "SqliteFunctions.h"
#include "SqliteScriptTarget.h"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <sqlite3.h>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

sqlite3* OpenScratchDatabase(std::filesystem::path const& path, bool create, bool keepJournal, std::string& error)
{
    if (!create && !std::filesystem::exists(path))
    {
        error = "database " + path.generic_string() + " does not exist";
        return nullptr;
    }

    sqlite3* db = nullptr;
    int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    std::u8string const name = path.u8string();
    if (sqlite3_open_v2(reinterpret_cast<char const*>(name.c_str()), &db, flags, nullptr) != SQLITE_OK)
    {
        error = db ? sqlite3_errmsg(db) : "out of memory";
        sqlite3_close_v2(db);
        return nullptr;
    }

    DbError err;
    std::string const pragmas = keepJournal ? "PRAGMA foreign_keys = ON; PRAGMA temp_store = MEMORY; PRAGMA cache_size = -262144"
        : "PRAGMA journal_mode = OFF; PRAGMA synchronous = OFF; PRAGMA foreign_keys = ON; PRAGMA temp_store = MEMORY; PRAGMA cache_size = -262144";
    if (!SqliteExec(db, pragmas, err) || RegisterMySqlFunctions(db) != SQLITE_OK)
    {
        error = err.message.empty() ? sqlite3_errmsg(db) : err.message;
        sqlite3_close_v2(db);
        return nullptr;
    }

    return db;
}

int LoadCommand(CommandArgs const& args)
{
    std::filesystem::path database;
    std::string input;
    bool truncate = false;
    bool create = false;

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        std::string_view arg = args[i];
        if (arg == "--db" && i + 1 < args.size())
            database = args[++i];
        else if (arg == "--truncate")
            truncate = true;
        else if (arg == "--create")
            create = true;
        else if (input.empty() && (arg == "-" || !arg.starts_with("--")))
            input = arg;
        else
        {
            std::fprintf(stderr, "load: unexpected argument '%.*s'\n", int(arg.size()), arg.data());
            return 1;
        }
    }

    if (database.empty() || input.empty())
    {
        std::fputs("usage: sqlconv load --db <x.sqlite> [--truncate] [--create] <file.sql | ->\n", stderr);
        return 1;
    }

    std::string error;
    sqlite3* db = OpenScratchDatabase(database, create, true, error);
    if (!db)
    {
        std::fprintf(stderr, "load: %s\n", error.c_str());
        return 1;
    }

    auto const start = std::chrono::steady_clock::now();
    bool ok;
    uint64 statements = 0;
    std::string message;
    {
        SqliteScriptTarget target(db);
        ScriptRunnerOptions options;
        options.disableForeignKeys = true;
        options.truncateOnFirstInsert = truncate;
        ScriptRunner runner(target, options);

        if (input == "-")
        {
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);
#endif
            std::ios::sync_with_stdio(false);
            ok = runner.RunStream(std::cin, "<stdin>");
        }
        else
            ok = runner.RunFile(input);

        statements = runner.GetExecutedStatements();
        if (!ok)
            message = runner.GetError().ToString();
    }

    sqlite3_close_v2(db);

    auto const ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (!ok)
    {
        std::fprintf(stderr, "load: %s\n", message.c_str());
        return 1;
    }

    std::printf("load: %llu statements applied to %s in %lld ms\n", static_cast<unsigned long long>(statements), database.generic_string().c_str(), static_cast<long long>(ms));
    return 0;
}
