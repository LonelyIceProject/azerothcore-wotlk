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
#include "SqliteScriptTarget.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sqlite3.h>
#include <string_view>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    struct MigrateOptions
    {
        fs::path database;
        fs::path autoIncrement;
        fs::path columns;
        fs::path wrapped;
    };

    std::string Lower(std::string_view text)
    {
        std::string result(text);
        for (char& c : result)
            if (c >= 'A' && c <= 'Z')
                c = char(c - 'A' + 'a');
        return result;
    }

    bool ReadTsv(fs::path const& path, std::size_t minFields, std::vector<std::vector<std::string>>& rows)
    {
        std::ifstream in(path);
        if (!in)
        {
            std::fprintf(stderr, "migrate: cannot open %s\n", path.generic_string().c_str());
            return false;
        }

        for (std::string line; std::getline(in, line);)
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

            if (fields.size() >= minFields && !fields[0].empty())
                rows.push_back(std::move(fields));
        }
        return true;
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
                char const* text = reinterpret_cast<char const*>(sqlite3_column_text(stmt, i));
                row.emplace_back(text ? text : "NULL");
            }
        }

        if (rc != SQLITE_DONE)
            error = sqlite3_errmsg(db);
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE;
    }

    bool QueryInt(sqlite3* db, std::string const& sql, int64& value, std::string& error)
    {
        std::vector<std::vector<std::string>> rows;
        if (!QueryRows(db, sql, rows, error))
            return false;
        value = rows.empty() || rows.front().empty() ? 0 : std::strtoll(rows.front().front().c_str(), nullptr, 10);
        return true;
    }

    std::vector<std::string> Tables(sqlite3* db)
    {
        std::vector<std::vector<std::string>> rows;
        std::string error;
        QueryRows(db, "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' ORDER BY name", rows, error);

        std::vector<std::string> tables;
        for (std::vector<std::string>& row : rows)
            tables.push_back(std::move(row.front()));
        return tables;
    }

    bool HasSequenceTable(sqlite3* db)
    {
        int64 count = 0;
        std::string error;
        return QueryInt(db, "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name = 'sqlite_sequence'", count, error) && count > 0;
    }

    bool Exec(sqlite3* db, std::string_view sql, std::string_view context)
    {
        DbError err;
        if (SqliteExec(db, sql, err))
            return true;
        std::fprintf(stderr, "migrate: %.*s: %s\n", int(context.size()), context.data(), err.message.c_str());
        return false;
    }

    ColumnAffinity Normalized(ColumnAffinity affinity)
    {
        return affinity == ColumnAffinity::DateTime ? ColumnAffinity::Text : affinity;
    }

    char const* AllowedTypes(ColumnAffinity affinity)
    {
        switch (Normalized(affinity))
        {
            case ColumnAffinity::Integer: return "'integer','null'";
            case ColumnAffinity::Real:
            case ColumnAffinity::Decimal: return "'integer','real','null'";
            case ColumnAffinity::Blob:    return "'blob','null'";
            default:                      return "'text','null'";
        }
    }

    // table<TAB>count for every table.
    int Counts(sqlite3* db)
    {
        for (std::string const& table : Tables(db))
        {
            int64 count = 0;
            std::string error;
            if (!QueryInt(db, "SELECT COUNT(*) FROM " + SqliteQuoteIdentifier(table), count, error))
            {
                std::fprintf(stderr, "migrate: %s: %s\n", table.c_str(), error.c_str());
                return 1;
            }
            std::printf("%s\t%lld\n", table.c_str(), static_cast<long long>(count));
        }
        return 0;
    }

    // Deletes every row of every table and the sqlite_sequence entries, keeping the schema.
    int Empty(sqlite3* db)
    {
        if (!Exec(db, "PRAGMA foreign_keys = OFF", "foreign_keys") || !Exec(db, "BEGIN IMMEDIATE", "begin"))
            return 1;

        std::vector<std::string> const tables = Tables(db);
        for (std::string const& table : tables)
        {
            if (!Exec(db, "DELETE FROM " + SqliteQuoteIdentifier(table), table))
            {
                Exec(db, "ROLLBACK", "rollback");
                return 1;
            }
        }

        if ((HasSequenceTable(db) && !Exec(db, "DELETE FROM sqlite_sequence", "sqlite_sequence")) || !Exec(db, "COMMIT", "commit"))
        {
            Exec(db, "ROLLBACK", "rollback");
            return 1;
        }

        Exec(db, "PRAGMA foreign_keys = ON", "foreign_keys");
        std::printf("migrate empty: %zu table(s) emptied\n", tables.size());
        return 0;
    }

    // sqlite_sequence = max(MySQL AUTO_INCREMENT - 1, current sequence, max rowid) for AUTOINCREMENT tables.
    int Sequences(sqlite3* db, fs::path const& tsv)
    {
        std::vector<std::vector<std::string>> rows;
        if (!ReadTsv(tsv, 2, rows))
            return 1;

        std::vector<std::vector<std::string>> autoIncrementTables;
        std::string error;
        if (!QueryRows(db, "SELECT name FROM sqlite_schema WHERE type = 'table' AND sql LIKE '%AUTOINCREMENT%'", autoIncrementTables, error))
        {
            std::fprintf(stderr, "migrate: %s\n", error.c_str());
            return 1;
        }

        std::map<std::string, std::string> tables;
        for (std::vector<std::string> const& row : autoIncrementTables)
            tables.emplace(Lower(row.front()), row.front());

        if (!Exec(db, "BEGIN IMMEDIATE", "begin"))
            return 1;

        uint32 applied = 0;
        uint32 skipped = 0;
        for (std::vector<std::string> const& row : rows)
        {
            if (row[1].empty() || row[1] == "NULL")
                continue;

            auto itr = tables.find(Lower(row[0]));
            if (itr == tables.end())
            {
                std::printf("  skip %s: AUTO_INCREMENT %s, no AUTOINCREMENT column in sqlite\n", row[0].c_str(), row[1].c_str());
                ++skipped;
                continue;
            }

            std::string const& name = itr->second;
            std::string const quotedName = SqliteQuoteString(name);
            int64 const next = std::strtoll(row[1].c_str(), nullptr, 10);
            int64 current = 0;
            int64 maxRowid = 0;
            if (!QueryInt(db, "SELECT COALESCE(MAX(seq), 0) FROM sqlite_sequence WHERE name = " + quotedName, current, error) ||
                !QueryInt(db, "SELECT COALESCE(MAX(rowid), 0) FROM " + SqliteQuoteIdentifier(name), maxRowid, error))
            {
                std::fprintf(stderr, "migrate: %s: %s\n", name.c_str(), error.c_str());
                Exec(db, "ROLLBACK", "rollback");
                return 1;
            }

            int64 const seq = std::max({ next - 1, current, maxRowid, int64(0) });
            if (!Exec(db, "DELETE FROM sqlite_sequence WHERE name = " + quotedName + "; INSERT INTO sqlite_sequence (name, seq) VALUES (" + quotedName + ", " + std::to_string(seq) + ")", name))
            {
                Exec(db, "ROLLBACK", "rollback");
                return 1;
            }
            ++applied;
        }

        if (!Exec(db, "COMMIT", "commit"))
        {
            Exec(db, "ROLLBACK", "rollback");
            return 1;
        }

        std::printf("migrate sequences: %u set, %u skipped\n", applied, skipped);
        return 0;
    }

    uint32 CheckIntegrity(sqlite3* db)
    {
        uint32 problems = 0;
        std::vector<std::vector<std::string>> rows;
        std::string error;
        if (!QueryRows(db, "PRAGMA integrity_check", rows, error))
        {
            std::printf("integrity_check: %s\n", error.c_str());
            ++problems;
        }
        else if (rows.size() != 1 || rows.front().front() != "ok")
        {
            for (std::size_t i = 0; i < rows.size() && i < 10; ++i)
                std::printf("integrity_check: %s\n", rows[i].front().c_str());
            problems += uint32(rows.size());
        }
        else
            std::printf("integrity_check: ok\n");

        rows.clear();
        if (!QueryRows(db, "PRAGMA foreign_key_check", rows, error))
        {
            std::printf("foreign_key_check: %s\n", error.c_str());
            ++problems;
        }
        else if (!rows.empty())
        {
            for (std::size_t i = 0; i < rows.size() && i < 10; ++i)
                std::printf("foreign_key_check: %s rowid %s -> %s\n", rows[i][0].c_str(), rows[i][1].c_str(), rows[i][2].c_str());
            std::printf("foreign_key_check: %zu violation(s)\n", rows.size());
            problems += uint32(rows.size());
        }
        else
            std::printf("foreign_key_check: ok\n");

        return problems;
    }

    // tsv: table, column, MySQL COLUMN_TYPE. Declared types must match the emitter's mapping and stored values their affinity.
    uint32 CheckColumns(sqlite3* db, fs::path const& tsv, uint32& warnings)
    {
        std::vector<std::vector<std::string>> rows;
        if (!ReadTsv(tsv, 3, rows))
            return 1;

        std::map<std::string, std::vector<std::pair<std::string, std::string>>> expected;
        for (std::vector<std::string> const& row : rows)
            expected[Lower(row[0])].emplace_back(row[1], Lower(row[2]));

        uint32 problems = 0;
        for (auto const& [key, columns] : expected)
        {
            TableModel model;
            std::string error;
            if (!SqliteLoadTable(db, key, model, error))
            {
                if (!error.empty())
                {
                    std::printf("columns %s: %s\n", key.c_str(), error.c_str());
                    ++problems;
                }
                continue;
            }

            std::string select;
            std::vector<ColumnModel const*> checked;
            std::vector<ColumnAffinity> affinities;
            for (auto const& [name, mysqlType] : columns)
            {
                ColumnModel const* actual = model.FindColumn(name);
                if (!actual)
                    continue;

                ColumnModel wanted;
                wanted.name = name;
                wanted.type = mysqlType;
                wanted.affinity = MySqlTypeAffinity(mysqlType);
                wanted.isUnsigned = mysqlType.find(" unsigned") != std::string::npos;

                std::string const wantedType = SqliteColumnType(wanted);
                std::string const actualType = SqliteColumnType(*actual);
                if (Normalized(wanted.affinity) != Normalized(actual->affinity))
                {
                    std::printf("type %s.%s: mysql %s -> %s, sqlite %s\n", model.name.c_str(), actual->name.c_str(), mysqlType.c_str(), wantedType.c_str(), actualType.c_str());
                    ++problems;
                }
                else if (wantedType != actualType)
                {
                    std::printf("warning type %s.%s: mysql %s -> %s, sqlite %s\n", model.name.c_str(), actual->name.c_str(), mysqlType.c_str(), wantedType.c_str(), actualType.c_str());
                    ++warnings;
                }

                select += select.empty() ? "SELECT " : ", ";
                select += "COALESCE(SUM(typeof(" + SqliteQuoteIdentifier(actual->name) + ") NOT IN (" + AllowedTypes(wanted.affinity) + ")), 0)";
                checked.push_back(actual);
                affinities.push_back(wanted.affinity);
            }

            if (checked.empty())
                continue;

            std::vector<std::vector<std::string>> counts;
            if (!QueryRows(db, select + " FROM " + SqliteQuoteIdentifier(model.name), counts, error) || counts.empty())
            {
                std::printf("values %s: %s\n", model.name.c_str(), error.c_str());
                ++problems;
                continue;
            }

            for (std::size_t i = 0; i < checked.size(); ++i)
            {
                long long const bad = std::strtoll(counts.front()[i].c_str(), nullptr, 10);
                if (!bad)
                    continue;

                std::vector<std::vector<std::string>> sample;
                std::string const column = SqliteQuoteIdentifier(checked[i]->name);
                QueryRows(db, "SELECT typeof(" + column + "), quote(substr(" + column + ", 1, 40)) FROM " + SqliteQuoteIdentifier(model.name) +
                    " WHERE typeof(" + column + ") NOT IN (" + AllowedTypes(affinities[i]) + ") LIMIT 1", sample, error);
                std::printf("values %s.%s: %lld value(s) of unexpected storage class%s%s\n", model.name.c_str(), checked[i]->name.c_str(), bad,
                    sample.empty() ? "" : ", e.g. ", sample.empty() ? "" : (sample.front()[0] + " " + sample.front()[1]).c_str());
                ++problems;
            }
        }

        std::printf("columns: %zu column(s) in %zu table(s) checked\n", rows.size(), expected.size());
        return problems;
    }

    // tsv: table, column, number of MySQL values >= 2^63. They are stored as the same 64 bits, i.e. negative.
    uint32 CheckWrapped(sqlite3* db, fs::path const& tsv)
    {
        std::vector<std::vector<std::string>> rows;
        if (!ReadTsv(tsv, 3, rows))
            return 1;

        uint32 problems = 0;
        for (std::vector<std::string> const& row : rows)
        {
            long long const expected = row[2] == "NULL" ? 0 : std::strtoll(row[2].c_str(), nullptr, 10);
            int64 actual = 0;
            std::string error;
            std::string const column = SqliteQuoteIdentifier(row[1]);
            if (!QueryInt(db, "SELECT COUNT(*) FROM " + SqliteQuoteIdentifier(row[0]) + " WHERE typeof(" + column + ") = 'integer' AND " + column + " < 0", actual, error))
            {
                std::printf("unsigned %s.%s: %s\n", row[0].c_str(), row[1].c_str(), error.c_str());
                ++problems;
            }
            else if (actual != expected)
            {
                std::printf("unsigned %s.%s: mysql %lld value(s) >= 2^63, sqlite %lld negative\n", row[0].c_str(), row[1].c_str(), expected, static_cast<long long>(actual));
                ++problems;
            }
            else if (expected)
                std::printf("unsigned %s.%s: %lld value(s) >= 2^63 stored as two's complement\n", row[0].c_str(), row[1].c_str(), expected);
        }
        return problems;
    }

    int Check(sqlite3* db, MigrateOptions const& options)
    {
        uint32 warnings = 0;
        uint32 problems = CheckIntegrity(db);
        if (!options.columns.empty())
            problems += CheckColumns(db, options.columns, warnings);
        if (!options.wrapped.empty())
            problems += CheckWrapped(db, options.wrapped);

        std::printf("migrate check: %u problem(s), %u warning(s)\n", problems, warnings);
        return problems ? 1 : 0;
    }

    int Optimize(sqlite3* db)
    {
        if (!Exec(db, "ANALYZE", "analyze") || !Exec(db, "VACUUM", "vacuum"))
            return 1;
        std::printf("migrate optimize: done\n");
        return 0;
    }

    int Usage()
    {
        std::fputs("usage: sqlconv migrate counts --db <x.sqlite>                        print table<TAB>row count\n"
            "       sqlconv migrate empty --db <x.sqlite>                         delete all rows, keep the schema\n"
            "       sqlconv migrate sequences --auto-increment <tsv> --db <x.sqlite>\n"
            "                                                                     sqlite_sequence from table<TAB>AUTO_INCREMENT\n"
            "       sqlconv migrate check --db <x.sqlite> [--columns <tsv>] [--wrapped <tsv>]\n"
            "                                                                     integrity, foreign keys, table<TAB>column<TAB>COLUMN_TYPE,\n"
            "                                                                     table<TAB>column<TAB>count of unsigned values >= 2^63\n"
            "       sqlconv migrate optimize --db <x.sqlite>                      ANALYZE and VACUUM\n", stderr);
        return 1;
    }
}

int MigrateCommand(CommandArgs const& args)
{
    if (args.empty())
        return Usage();

    std::string_view const action = args[0];
    MigrateOptions options;
    for (std::size_t i = 1; i < args.size(); ++i)
    {
        std::string_view arg = args[i];
        if (i + 1 >= args.size())
            return Usage();
        if (arg == "--db")
            options.database = args[++i];
        else if (arg == "--auto-increment")
            options.autoIncrement = args[++i];
        else if (arg == "--columns")
            options.columns = args[++i];
        else if (arg == "--wrapped")
            options.wrapped = args[++i];
        else
            return Usage();
    }

    bool const known = action == "counts" || action == "empty" || action == "check" || action == "optimize" ||
        (action == "sequences" && !options.autoIncrement.empty());
    if (!known || options.database.empty())
        return Usage();

    std::string error;
    sqlite3* db = OpenScratchDatabase(options.database, false, true, error);
    if (!db)
    {
        std::fprintf(stderr, "migrate: %s\n", error.c_str());
        return 1;
    }

    int result;
    if (action == "counts")
        result = Counts(db);
    else if (action == "empty")
        result = Empty(db);
    else if (action == "sequences")
        result = Sequences(db, options.autoIncrement);
    else if (action == "check")
        result = Check(db, options);
    else
        result = Optimize(db);

    sqlite3_close_v2(db);
    return result;
}
