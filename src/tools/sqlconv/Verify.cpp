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
#include "SchemaDdl.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sqlite3.h>
#include <string_view>
#include <vector>

namespace
{
    std::string Lower(std::string_view text)
    {
        std::string result(text);
        for (char& c : result)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return result;
    }

    std::vector<std::string> SplitTabs(std::string line)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        std::vector<std::string> fields;
        std::size_t start = 0;
        while (true)
        {
            std::size_t end = line.find('\t', start);
            fields.push_back(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
        return fields;
    }

    bool ParseArgs(CommandArgs const& args, std::string_view tsvOption, std::filesystem::path& tsv, std::filesystem::path& database)
    {
        for (std::size_t i = 0; i < args.size(); ++i)
        {
            std::string_view arg = args[i];
            if (arg == tsvOption && i + 1 < args.size())
                tsv = args[++i];
            else if (arg == "--db" && i + 1 < args.size())
                database = args[++i];
            else
                return false;
        }
        return !tsv.empty() && !database.empty();
    }

    std::vector<std::string> Tables(sqlite3* db)
    {
        std::vector<std::string> tables;
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY name", -1, &stmt, nullptr);
        while (stmt && sqlite3_step(stmt) == SQLITE_ROW)
            tables.emplace_back(reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0)));
        sqlite3_finalize(stmt);
        return tables;
    }

    std::string Join(std::vector<std::string> const& names)
    {
        std::string text;
        for (std::string const& name : names)
            text += (text.empty() ? "" : ",") + name;
        return text;
    }
}

int SchemaDiffCommand(CommandArgs const& args)
{
    std::filesystem::path tsv;
    std::filesystem::path database;
    if (!ParseArgs(args, "--mysql-columns", tsv, database))
    {
        std::fputs("usage: sqlconv schema-diff --mysql-columns <table\\tcolumn\\tordinal tsv> --db <x.sqlite>\n", stderr);
        return 1;
    }

    std::ifstream in(tsv);
    if (!in)
    {
        std::fprintf(stderr, "schema-diff: cannot open %s\n", tsv.generic_string().c_str());
        return 1;
    }

    std::map<std::string, std::map<long, std::string>> expected;
    std::map<std::string, std::string> displayName;
    for (std::string line; std::getline(in, line);)
    {
        std::vector<std::string> fields = SplitTabs(line);
        if (fields.size() < 3 || fields[0].empty())
            continue;
        std::string const key = Lower(fields[0]);
        displayName.emplace(key, fields[0]);
        expected[key][std::strtol(fields[2].c_str(), nullptr, 10)] = fields[1];
    }

    std::string error;
    sqlite3* db = OpenScratchDatabase(database, false, true, error);
    if (!db)
    {
        std::fprintf(stderr, "schema-diff: %s\n", error.c_str());
        return 1;
    }

    uint32 differences = 0;
    std::map<std::string, std::vector<std::string>> actual;
    for (std::string const& table : Tables(db))
    {
        std::vector<std::string>& columns = actual[Lower(table)];
        displayName.emplace(Lower(table), table);
        sqlite3_stmt* stmt = nullptr;
        std::string const sql = "SELECT name FROM pragma_table_info(" + SqliteQuoteString(table) + ") ORDER BY cid";
        sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr);
        while (stmt && sqlite3_step(stmt) == SQLITE_ROW)
            columns.emplace_back(reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0)));
        sqlite3_finalize(stmt);
    }
    sqlite3_close_v2(db);

    for (auto const& [key, ordinals] : expected)
    {
        std::vector<std::string> columns;
        for (auto const& [ordinal, name] : ordinals)
            columns.push_back(name);

        auto itr = actual.find(key);
        if (itr == actual.end())
        {
            std::printf("missing table %s\n", displayName[key].c_str());
            ++differences;
            continue;
        }

        bool same = columns.size() == itr->second.size() &&
            std::equal(columns.begin(), columns.end(), itr->second.begin(), [](std::string const& a, std::string const& b) { return Lower(a) == Lower(b); });
        if (!same)
        {
            std::printf("table %s:\n  mysql:  %s\n  sqlite: %s\n", displayName[key].c_str(), Join(columns).c_str(), Join(itr->second).c_str());
            ++differences;
        }
    }

    for (auto const& [key, columns] : actual)
    {
        if (!expected.count(key))
        {
            std::printf("extra table %s\n", displayName[key].c_str());
            ++differences;
        }
    }

    std::printf("schema-diff: %u difference(s)\n", differences);
    return differences ? 1 : 0;
}

int VerifyCommand(CommandArgs const& args)
{
    std::filesystem::path tsv;
    std::filesystem::path database;
    if (!ParseArgs(args, "--counts", tsv, database))
    {
        std::fputs("usage: sqlconv verify --counts <table\\tcount tsv> --db <x.sqlite>\n", stderr);
        return 1;
    }

    std::ifstream in(tsv);
    if (!in)
    {
        std::fprintf(stderr, "verify: cannot open %s\n", tsv.generic_string().c_str());
        return 1;
    }

    std::string error;
    sqlite3* db = OpenScratchDatabase(database, false, true, error);
    if (!db)
    {
        std::fprintf(stderr, "verify: %s\n", error.c_str());
        return 1;
    }

    uint32 checked = 0;
    uint32 differences = 0;
    for (std::string line; std::getline(in, line);)
    {
        std::vector<std::string> fields = SplitTabs(line);
        if (fields.size() < 2 || fields[0].empty())
            continue;

        ++checked;
        long long const expected = std::strtoll(fields[1].c_str(), nullptr, 10);
        sqlite3_stmt* stmt = nullptr;
        std::string const sql = "SELECT COUNT(*) FROM " + SqliteQuoteIdentifier(fields[0]);
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        {
            std::printf("%s: %s\n", fields[0].c_str(), sqlite3_errmsg(db));
            ++differences;
            continue;
        }

        long long count = -1;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);

        if (count != expected)
        {
            std::printf("%s: mysql %lld, sqlite %lld\n", fields[0].c_str(), expected, count);
            ++differences;
        }
    }
    sqlite3_close_v2(db);

    std::printf("verify: %u table(s) checked, %u difference(s)\n", checked, differences);
    return differences ? 1 : 0;
}
