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
#include "CryptoHash.h"
#include "MySqlLexer.h"
#include "SchemaDdl.h"
#include "ScriptRunner.h"
#include "SqlDialect.h"
#include "SqliteScriptTarget.h"
#include "Util.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sqlite3.h>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace
{
    constexpr std::string_view OverrideDirectory = "data/sql/overrides/sqlite";

    struct DatabaseSpec
    {
        std::string name;
        fs::path root;              // '$' in updates_include
        fs::path base;
        std::string moduleFilter;   // substring of module data/sql/<dir>; empty = no module directories
    };

    struct UpdateFile
    {
        fs::path path;
        std::string state;
    };

    struct AppliedEntry
    {
        std::string hash;
        std::string state;
    };

    struct LintContext
    {
        fs::path source;
        std::vector<std::string> modules;
        std::vector<std::string> errors;
        std::set<fs::path> overridesUsed;
    };

    std::vector<std::string> Split(std::string_view text, char separator)
    {
        std::vector<std::string> parts;
        std::size_t start = 0;
        while (start <= text.size())
        {
            std::size_t end = text.find(separator, start);
            if (end == std::string_view::npos)
                end = text.size();
            if (end > start)
                parts.emplace_back(text.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    }

    double ElapsedSeconds(std::chrono::steady_clock::time_point start)
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    // SHA1 of the file as the updater reads it (text mode).
    std::string FileHash(fs::path const& path)
    {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string content = ss.str();
#ifdef _WIN32
        std::string text;
        text.reserve(content.size());
        for (std::size_t i = 0; i < content.size(); ++i)
            if (!(content[i] == '\r' && i + 1 < content.size() && content[i + 1] == '\n'))
                text += content[i];
        content = std::move(text);
#endif
        return ByteArrayToHexStr(Acore::Crypto::SHA1::GetDigestOf(content));
    }

    // Module directory (modules/<name>) that contains path, if any.
    fs::path ModuleOf(LintContext const& context, fs::path const& path)
    {
        fs::path const modules = context.source / "modules";
        fs::path const relative = path.lexically_relative(modules);
        if (relative.empty() || *relative.begin() == "..")
            return {};
        return modules / *relative.begin();
    }

    fs::path FindOverride(LintContext& context, DatabaseSpec const& spec, UpdateFile const& file)
    {
        std::vector<fs::path> candidates = { spec.root / OverrideDirectory / file.path.filename() };
        if (fs::path module = ModuleOf(context, file.path); !module.empty())
            candidates.push_back(module / OverrideDirectory / file.path.filename());

        for (fs::path const& candidate : candidates)
        {
            if (fs::is_regular_file(candidate))
            {
                context.overridesUsed.insert(fs::weakly_canonical(candidate));
                return candidate;
            }
        }
        return {};
    }

    bool ApplyFile(LintContext& context, DatabaseSpec const& spec, SqliteScriptTarget& target, UpdateFile const& file, ScriptRunnerOptions const& options)
    {
        fs::path actual = file.path;
        if (fs::path override = FindOverride(context, spec, file); !override.empty())
        {
            std::printf("  %s: override %s\n", spec.name.c_str(), override.lexically_relative(context.source).generic_string().c_str());
            actual = override;
        }

        ScriptRunner runner(target, options);
        if (runner.RunFile(actual))
            return true;

        context.errors.push_back(spec.name + ": " + runner.GetError().ToString());
        std::printf("  ERROR %s\n", context.errors.back().c_str());
        return false;
    }

    bool QueryRows(sqlite3* db, std::string const& sql, std::vector<std::vector<std::string>>& rows, std::string& error)
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            error = sqlite3_errmsg(db);
            return false;
        }

        int rc;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        {
            std::vector<std::string>& row = rows.emplace_back();
            for (int i = 0; i < sqlite3_column_count(stmt); ++i)
            {
                unsigned char const* text = sqlite3_column_text(stmt, i);
                row.emplace_back(text ? reinterpret_cast<char const*>(text) : "");
            }
        }

        if (rc != SQLITE_DONE)
            error = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }

    void CollectFiles(LintContext& context, DatabaseSpec const& spec, fs::path const& directory, std::string const& state, std::map<std::string, UpdateFile>& files, uint32 depth)
    {
        std::error_code ec;
        for (fs::directory_iterator itr(directory, ec), end; !ec && itr != end; itr.increment(ec))
        {
            if (itr->is_directory())
            {
                if (depth < 10)
                    CollectFiles(context, spec, itr->path(), state, files, depth + 1);
            }
            else if (itr->path().extension() == ".sql")
            {
                std::string const name = itr->path().filename().string();
                if (!files.emplace(name, UpdateFile{ itr->path(), state }).second)
                {
                    context.errors.push_back(spec.name + ": duplicate update file name " + itr->path().generic_string());
                    std::printf("  ERROR %s\n", context.errors.back().c_str());
                }
            }
        }
    }

    bool LintDatabase(LintContext& context, DatabaseSpec const& spec, fs::path const& directory, bool keep)
    {
        auto const start = std::chrono::steady_clock::now();
        std::size_t const errorsBefore = context.errors.size();
        auto fail = [&](std::string message)
        {
            context.errors.push_back(spec.name + ": " + message);
            std::printf("  ERROR %s\n", context.errors.back().c_str());
            return false;
        };

        fs::path const path = directory / (spec.name + ".sqlite");
        for (char const* suffix : { "", "-journal", "-wal", "-shm" })
        {
            std::error_code ec;
            fs::remove(fs::path(path.string() + suffix), ec);
        }

        std::string error;
        sqlite3* db = OpenScratchDatabase(path, true, false, error);
        if (!db)
            return fail(error);

        std::printf("%s: %s\n", spec.name.c_str(), path.generic_string().c_str());

        uint32 baseFiles = 0;
        uint32 updateFiles = 0;
        {
            SqliteScriptTarget target(db);
            DbError err;

            std::vector<fs::path> base;
            std::error_code ec;
            for (fs::directory_iterator itr(spec.base, ec), end; !ec && itr != end; itr.increment(ec))
                if (itr->path().extension() == ".sql")
                    base.push_back(itr->path());
            std::sort(base.begin(), base.end());
            if (base.empty())
                fail("no base files in " + spec.base.generic_string());

            target.SetForeignKeys(false, err);
            ScriptRunnerOptions baseOptions;
            for (fs::path const& file : base)
            {
                ApplyFile(context, spec, target, { file, "BASE" }, baseOptions);
                ++baseFiles;
            }
            if (!target.CheckForeignKeys(err))
                fail("after base: " + err.message);
            target.SetForeignKeys(true, err);

            std::vector<std::vector<std::string>> rows;
            if (!QueryRows(db, "SELECT path, state FROM updates_include", rows, error))
                fail("cannot read updates_include: " + error);

            std::map<std::string, UpdateFile> files;
            for (std::vector<std::string> const& row : rows)
            {
                std::string dir = row[0];
                if (dir.starts_with("$"))
                    dir = spec.root.generic_string() + dir.substr(1);
                if (fs::is_directory(dir))
                    CollectFiles(context, spec, dir, row[1], files, 1);
            }

            if (!spec.moduleFilter.empty())
            {
                for (std::string const& module : context.modules)
                {
                    fs::path const sqlDir = context.source / "modules" / module / "data" / "sql";
                    for (fs::directory_iterator itr(sqlDir, ec), end; !ec && itr != end; itr.increment(ec))
                        if (itr->is_directory() && itr->path().filename().string().find(spec.moduleFilter) != std::string::npos)
                            CollectFiles(context, spec, itr->path(), "MODULE", files, 1);
                    ec.clear();
                }
            }

            rows.clear();
            if (!QueryRows(db, "SELECT name, hash, state FROM updates", rows, error))
                fail("cannot read updates: " + error);

            std::map<std::string, AppliedEntry> applied;
            std::map<std::string, std::string> hashToName;
            for (std::vector<std::string> const& row : rows)
            {
                applied[row[0]] = { row[1], row[2] };
                hashToName.emplace(row[1], row[0]);
            }

            sqlite3_stmt* record = nullptr;
            sqlite3_prepare_v2(db, "REPLACE INTO updates (name, hash, state, speed) VALUES (?1, ?2, ?3, 0)", -1, &record, nullptr);

            ScriptRunnerOptions updateOptions;
            updateOptions.disableForeignKeys = true;
            for (bool late : { false, true })
            {
                for (auto const& [name, file] : files)
                {
                    bool const isLate = file.state == "PENDING" || file.state == "CUSTOM" || file.state == "MODULE";
                    if (isLate != late)
                        continue;

                    auto itr = applied.find(name);
                    if (itr != applied.end() && itr->second.state == "ARCHIVED" && file.state == "ARCHIVED")
                        continue;

                    std::string const hash = FileHash(file.path);
                    if (itr != applied.end() && itr->second.hash == hash)
                        continue;

                    if (itr == applied.end())
                    {
                        auto renamed = hashToName.find(hash);
                        if (renamed != hashToName.end() && !files.count(renamed->second))
                            continue;
                    }

                    ApplyFile(context, spec, target, file, updateOptions);
                    ++updateFiles;

                    if (record)
                    {
                        sqlite3_bind_text(record, 1, name.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(record, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(record, 3, file.state.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_step(record);
                        sqlite3_reset(record);
                    }
                }
            }
            sqlite3_finalize(record);
        }

        std::vector<std::vector<std::string>> rows;
        if (!QueryRows(db, "PRAGMA integrity_check", rows, error) || rows.empty() || rows.front().front() != "ok")
            fail("integrity_check: " + (rows.empty() ? error : rows.front().front()));

        rows.clear();
        if (!QueryRows(db, "PRAGMA foreign_key_check", rows, error))
            fail("foreign_key_check: " + error);
        else if (!rows.empty())
            fail("foreign_key_check: " + std::to_string(rows.size()) + " violation(s), first in " + rows.front()[0]);

        rows.clear();
        QueryRows(db, "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\'", rows, error);
        std::string const tables = rows.empty() ? "?" : rows.front().front();

        if (keep)
        {
            DbError err;
            SqliteExec(db, "ANALYZE", err);
        }
        sqlite3_close_v2(db);

        std::printf("%s: %u base files, %u update files, %s tables, %zu error(s), %.1f s\n", spec.name.c_str(), baseFiles, updateFiles, tables.c_str(),
            context.errors.size() - errorsBefore, ElapsedSeconds(start));
        return context.errors.size() == errorsBefore;
    }

    // ---- lint --statements ----

    struct PreparedText
    {
        std::string id;
        std::string sql;
        uint32 line = 0;
    };

    // Reads a C++ string literal sequence ("a" "b" ...) starting at pos; false if the argument is not a literal.
    bool ReadCppStrings(std::string_view source, std::size_t& pos, std::string& out)
    {
        bool any = false;
        while (true)
        {
            while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' || source[pos] == '\r' || source[pos] == '\n'))
                ++pos;
            if (pos >= source.size() || source[pos] != '"')
                return any;

            ++pos;
            while (pos < source.size() && source[pos] != '"')
            {
                char c = source[pos++];
                if (c == '\\' && pos < source.size())
                {
                    char e = source[pos++];
                    switch (e)
                    {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case '0': out += '\0'; break;
                        default: out += e; break;
                    }
                }
                else
                    out += c;
            }
            ++pos;
            any = true;
        }
    }

    // Collects Function(<ids...>, "sql" ...) calls: the last identifier before the literal is the statement id.
    std::vector<PreparedText> ExtractStatements(fs::path const& file, std::string_view function, std::vector<std::string>& skipped)
    {
        std::vector<PreparedText> result;
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return result;
        std::ostringstream ss;
        ss << in.rdbuf();
        std::string const source = ss.str();

        std::string const call = std::string(function) + "(";
        for (std::size_t pos = source.find(call); pos != std::string::npos; pos = source.find(call, pos + 1))
        {
            if (pos > 0 && (std::isalnum(uint8(source[pos - 1])) || source[pos - 1] == '_'))
                continue;

            std::size_t p = pos + call.size();
            std::size_t const argsEnd = source.find_first_of("\";", p);
            if (argsEnd == std::string::npos || source[argsEnd] != '"')
                continue;

            std::string_view args(source.data() + p, argsEnd - p);
            if (args.find_first_of("(){}") != std::string_view::npos)
                continue;
            std::vector<std::string> parts = Split(args, ',');
            if (parts.empty())
                continue;
            std::string id = parts.back();
            id.erase(std::remove_if(id.begin(), id.end(), [](char c) { return std::isspace(uint8(c)); }), id.end());
            if (id.empty() && parts.size() > 1)
            {
                id = parts[parts.size() - 2];
                id.erase(std::remove_if(id.begin(), id.end(), [](char c) { return std::isspace(uint8(c)); }), id.end());
            }

            PreparedText text;
            text.id = id;
            text.line = uint32(std::count(source.begin(), source.begin() + pos, '\n') + 1);
            p = argsEnd;
            bool literal = ReadCppStrings(source, p, text.sql);
            while (p < source.size() && std::isspace(uint8(source[p])))
                ++p;
            if (!literal || p >= source.size() || (source[p] != ',' && source[p] != ')'))
            {
                skipped.push_back(file.filename().string() + ":" + std::to_string(text.line) + " " + id);
                continue;
            }
            result.push_back(std::move(text));
        }
        return result;
    }

    uint32 CountParameters(std::string_view sql)
    {
        uint32 count = 0;
        for (MySqlToken const& token : MySqlTokenize(sql))
            if (token.type == MySqlTokenType::Parameter)
                ++count;
        return count;
    }

    std::string ReplaceFormatFields(std::string_view sql)
    {
        std::string result;
        for (std::size_t i = 0; i < sql.size(); ++i)
        {
            if (sql[i] == '{')
            {
                std::size_t const close = sql.find('}', i);
                if (close != std::string_view::npos && sql.substr(i + 1, close - i - 1).find_first_of("{ \n") == std::string_view::npos)
                {
                    result += '0';
                    i = close;
                    continue;
                }
            }
            result += sql[i];
        }
        return result;
    }

    // Best effort: ad-hoc SQL literals passed to Query/Execute/Append, prepared against the scratch schemas.
    void ReportAdhoc(LintContext const& context, fs::path const& schemas)
    {
        static constexpr std::pair<std::string_view, std::string_view> Receivers[] =
        {
            { "CharacterDatabase", "characters" }, { "WorldDatabase", "world" }, { "LoginDatabase", "auth" }, { "PlayerbotsDatabase", "playerbots" }
        };
        static constexpr std::string_view Calls[] = { "Query(", "Execute(", "DirectExecute(", "AsyncQuery(", "Append(" };

        std::map<std::string, sqlite3*> databases;
        for (std::string_view name : { "characters", "world", "auth", "playerbots" })
        {
            std::string error;
            if (sqlite3* db = OpenScratchDatabase(schemas / (std::string(name) + ".sqlite"), false, true, error))
                databases[std::string(name)] = db;
        }
        if (databases.count("playerbots") && fs::exists(schemas / "characters.sqlite"))
        {
            DbError err;
            SqliteExec(databases["playerbots"], "ATTACH " + SqliteQuoteString((schemas / "characters.sqlite").generic_string()) + " AS characters", err);
        }

        SqlDialect const& dialect = GetDialect(DatabaseBackend::SQLite);
        uint32 checked = 0;
        uint32 suspicious = 0;

        for (fs::path const root : { context.source / "src", context.source / "modules" })
        {
            std::error_code ec;
            for (fs::recursive_directory_iterator itr(root, ec), end; !ec && itr != end; itr.increment(ec))
            {
                if (!itr->is_regular_file() || (itr->path().extension() != ".cpp" && itr->path().extension() != ".h"))
                    continue;

                std::ifstream in(itr->path(), std::ios::binary);
                std::ostringstream ss;
                ss << in.rdbuf();
                std::string const source = ss.str();

                for (std::string_view call : Calls)
                {
                    for (std::size_t pos = source.find(call); pos != std::string::npos; pos = source.find(call, pos + 1))
                    {
                        std::size_t p = pos + call.size();
                        while (p < source.size() && std::isspace(uint8(source[p])))
                            ++p;
                        if (p >= source.size() || source[p] != '"')
                            continue;

                        std::string sql;
                        if (!ReadCppStrings(source, p, sql))
                            continue;
                        while (p < source.size() && std::isspace(uint8(source[p])))
                            ++p;
                        if (p >= source.size() || (source[p] != ',' && source[p] != ')'))
                            continue;

                        MySqlToken const first = MySqlLexer(sql).NextSignificant();
                        if (first.type != MySqlTokenType::Identifier)
                            continue;

                        std::size_t const lineStart = source.rfind('\n', pos) + 1;
                        std::string_view const prefix(source.data() + lineStart, pos - lineStart);
                        if (prefix.find("//") != std::string_view::npos)
                            continue;
                        std::vector<sqlite3*> candidates;
                        for (auto const& [receiver, database] : Receivers)
                            if (prefix.find(receiver) != std::string_view::npos && databases.count(std::string(database)))
                                candidates.push_back(databases[std::string(database)]);
                        if (candidates.empty())
                            for (auto const& [name, db] : databases)
                                candidates.push_back(db);

                        ++checked;
                        std::string const translated = dialect.Translate(ReplaceFormatFields(sql));
                        if (translated.empty())
                            continue;

                        std::string problem;
                        for (sqlite3* db : candidates)
                        {
                            sqlite3_stmt* stmt = nullptr;
                            if (sqlite3_prepare_v2(db, translated.c_str(), int(translated.size()), &stmt, nullptr) == SQLITE_OK)
                            {
                                problem.clear();
                                sqlite3_finalize(stmt);
                                break;
                            }
                            if (problem.empty())
                                problem = sqlite3_errmsg(db);
                        }

                        if (!problem.empty())
                        {
                            ++suspicious;
                            uint32 const line = uint32(std::count(source.begin(), source.begin() + pos, '\n') + 1);
                            std::printf("WARNING ad-hoc %s:%u: %s\n    > %s\n", itr->path().lexically_relative(context.source).generic_string().c_str(), line,
                                problem.c_str(), translated.c_str());
                        }
                    }
                }
            }
        }

        for (auto const& [name, db] : databases)
            sqlite3_close_v2(db);

        std::printf("ad-hoc: %u literals checked, %u suspicious (not a gate)\n", checked, suspicious);
    }

    int LintStatements(LintContext& context, fs::path const& schemas)
    {
        struct Source
        {
            std::string database;
            fs::path file;
            fs::path overrides;
        };

        fs::path const impl = context.source / "src/server/database/Database/Implementation";
        fs::path const playerbots = context.source / "modules/mod-playerbots/src/Db";
        std::vector<Source> sources =
        {
            { "characters", impl / "CharacterDatabase.cpp", impl / "CharacterDatabaseSQLite.cpp" },
            { "auth", impl / "LoginDatabase.cpp", impl / "LoginDatabaseSQLite.cpp" },
            { "world", impl / "WorldDatabase.cpp", impl / "WorldDatabaseSQLite.cpp" },
            { "playerbots", playerbots / "PlayerbotsDatabase.cpp", playerbots / "PlayerbotsDatabaseSQLite.cpp" }
        };

        SqlDialect const& dialect = GetDialect(DatabaseBackend::SQLite);
        uint32 total = 0;
        uint32 failed = 0;
        std::vector<std::string> skipped;

        for (Source const& src : sources)
        {
            if (!fs::exists(src.file))
                continue;

            std::string error;
            sqlite3* db = OpenScratchDatabase(schemas / (src.database + ".sqlite"), false, true, error);
            if (!db)
            {
                std::printf("ERROR %s: %s\n", src.database.c_str(), error.c_str());
                ++failed;
                continue;
            }

            if (src.database == "playerbots" && fs::exists(schemas / "characters.sqlite"))
            {
                DbError err;
                SqliteExec(db, "ATTACH " + SqliteQuoteString((schemas / "characters.sqlite").generic_string()) + " AS characters", err);
            }

            uint32 const before = total;
            std::map<std::string, std::string> overrides;
            for (PreparedText const& o : ExtractStatements(src.overrides, "OverrideStatement", skipped))
                overrides[o.id] = o.sql;

            for (PreparedText const& statement : ExtractStatements(src.file, "PrepareStatement", skipped))
            {
                ++total;
                auto itr = overrides.find(statement.id);
                bool const overridden = itr != overrides.end();
                std::string const text = overridden ? itr->second : statement.sql;
                if (overridden)
                    overrides.erase(itr);

                std::string const translated = overridden ? text : dialect.Translate(text);
                uint32 const expected = CountParameters(statement.sql);

                sqlite3_stmt* stmt = nullptr;
                std::string problem;
                if (sqlite3_prepare_v3(db, translated.c_str(), int(translated.size()), 0, &stmt, nullptr) != SQLITE_OK)
                    problem = sqlite3_errmsg(db);
                else if (!stmt)
                    problem = "empty statement";
                else if (uint32(sqlite3_bind_parameter_count(stmt)) != expected)
                    problem = "parameter count " + std::to_string(sqlite3_bind_parameter_count(stmt)) + ", expected " + std::to_string(expected);
                sqlite3_finalize(stmt);

                if (!problem.empty())
                {
                    ++failed;
                    std::printf("ERROR %s:%u %s%s: %s\n    > %s\n", src.file.filename().string().c_str(), statement.line, statement.id.c_str(),
                        overridden ? " (override)" : "", problem.c_str(), translated.c_str());
                }
            }

            for (auto const& [id, sql] : overrides)
                std::printf("WARNING %s: override for unknown statement %s\n", src.overrides.filename().string().c_str(), id.c_str());

            std::printf("%s: %u prepared statements\n", src.file.filename().string().c_str(), total - before);

            sqlite3_close_v2(db);
        }

        for (std::string const& s : skipped)
            std::printf("WARNING not a string literal, skipped: %s\n", s.c_str());

        std::printf("statements: %u checked, %u failed\n", total, failed);
        ReportAdhoc(context, schemas);
        return failed ? 1 : 0;
    }

    void ReportUnusedOverrides(LintContext& context)
    {
        std::vector<fs::path> roots = { context.source / OverrideDirectory };
        std::error_code ec;
        for (fs::directory_iterator itr(context.source / "modules", ec), end; !ec && itr != end; itr.increment(ec))
            roots.push_back(itr->path() / OverrideDirectory);

        for (fs::path const& root : roots)
        {
            std::error_code rec;
            for (fs::directory_iterator itr(root, rec), end; !rec && itr != end; itr.increment(rec))
                if (itr->path().extension() == ".sql" && !context.overridesUsed.count(fs::weakly_canonical(itr->path())))
                    std::printf("WARNING unused override (original not applied): %s\n", itr->path().lexically_relative(context.source).generic_string().c_str());
        }
    }
}

int LintCommand(CommandArgs const& args)
{
    LintContext context;
    fs::path keep;
    fs::path schemas;
    bool statements = false;
    bool modulesGiven = false;
    std::vector<std::string> only;

    for (std::size_t i = 0; i < args.size(); ++i)
    {
        std::string_view arg = args[i];
        if (arg == "--source" && i + 1 < args.size())
            context.source = args[++i];
        else if (arg == "--modules" && i + 1 < args.size())
        {
            context.modules = Split(args[++i], ',');
            modulesGiven = true;
        }
        else if (arg == "--keep" && i + 1 < args.size())
            keep = args[++i];
        else if (arg == "--schemas" && i + 1 < args.size())
            schemas = args[++i];
        else if (arg == "--databases" && i + 1 < args.size())
            only = Split(args[++i], ',');
        else if (arg == "--statements")
            statements = true;
        else
        {
            std::fprintf(stderr, "lint: unexpected argument '%.*s'\n", int(arg.size()), arg.data());
            return 1;
        }
    }

    if (context.source.empty() || !fs::is_directory(context.source / "data" / "sql"))
    {
        std::fputs("usage: sqlconv lint --source <SourceDirectory> [--modules <list>] [--keep <dir>] [--databases <list>]\n"
            "       sqlconv lint --statements --source <SourceDirectory> [--schemas <dir>]\n", stderr);
        return 1;
    }

    context.source = fs::absolute(context.source).lexically_normal();
    if (!modulesGiven)
    {
        std::error_code ec;
        for (fs::directory_iterator itr(context.source / "modules", ec), end; !ec && itr != end; itr.increment(ec))
            if (itr->is_directory())
                context.modules.push_back(itr->path().filename().string());
        std::sort(context.modules.begin(), context.modules.end());
    }

    if (statements && !schemas.empty())
        return LintStatements(context, schemas);

    fs::path directory = keep;
    if (directory.empty())
        directory = fs::temp_directory_path() / "sqlconv-lint";
    std::error_code ec;
    fs::create_directories(directory, ec);

    std::vector<DatabaseSpec> specs =
    {
        { "auth", context.source, context.source / "data/sql/base/db_auth", "auth" },
        { "characters", context.source, context.source / "data/sql/base/db_characters", "characters" },
        { "world", context.source, context.source / "data/sql/base/db_world", "world" }
    };

    fs::path const playerbots = context.source / "modules/mod-playerbots";
    if (std::find(context.modules.begin(), context.modules.end(), "mod-playerbots") != context.modules.end() && fs::is_directory(playerbots / "data/sql/playerbots/base"))
        specs.push_back({ "playerbots", playerbots, playerbots / "data/sql/playerbots/base", "" });

    auto const start = std::chrono::steady_clock::now();
    for (DatabaseSpec const& spec : specs)
        if (only.empty() || std::find(only.begin(), only.end(), spec.name) != only.end())
            LintDatabase(context, spec, directory, !keep.empty());

    if (only.empty())
        ReportUnusedOverrides(context);

    std::printf("lint: %zu error(s), %.1f s, databases in %s\n", context.errors.size(), ElapsedSeconds(start), directory.generic_string().c_str());
    for (std::string const& error : context.errors)
        std::printf("  %s\n", error.c_str());

    int result = context.errors.empty() ? 0 : 1;
    if (statements && LintStatements(context, directory))
        result = 1;
    return result;
}
