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

#include "MySqlLexer.h"
#include "SqlDialect.h"
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string_view>
#include <vector>

using CommandArgs = std::vector<std::string_view>;

int SelfTestCommand(CommandArgs const& args);

namespace
{
    int PrintUsage()
    {
        std::fputs(
            "Usage: sqlconv <command> [options]\n"
            "\n"
            "Commands:\n"
            "  selftest [<cases dir>]                       run built-in lexer/translator/DDL/ALTER checks\n"
            "  translate <file.sql | ->                     print the SQLite translation of a MySQL script\n"
            "  lint --source <dir> [--modules <list>] [--keep <dir>]\n"
            "                                               build scratch databases from base + updates + modules\n"
            "  lint --statements --source <dir>             prepare every core and playerbots prepared statement\n"
            "  load --db <x.sqlite> [--truncate] <file.sql | ->\n"
            "                                               apply a MySQL dump or script\n"
            "  schema-diff --mysql-columns <tsv> --db <x.sqlite>\n"
            "                                               compare column sets and order\n"
            "  verify --counts <tsv> --db <x.sqlite>        compare row counts per table\n",
            stderr);
        return 1;
    }

    int TranslateCommand(CommandArgs const& args)
    {
        if (args.size() != 1)
            return PrintUsage();

        std::ifstream file;
        std::istream* in = &std::cin;
        if (args[0] != "-")
        {
            file.open(std::string(args[0]), std::ios::binary);
            if (!file)
            {
                std::fprintf(stderr, "sqlconv: cannot open '%.*s'\n", static_cast<int>(args[0].size()), args[0].data());
                return 1;
            }
            in = &file;
        }

        SqlDialect const& dialect = GetDialect(DatabaseBackend::SQLite);
        MySqlStatementReader reader(*in);
        MySqlStatementText statement;
        while (reader.Next(statement))
        {
            std::string translated = dialect.Translate(statement.sql);
            if (!translated.empty())
                std::cout << translated << ";\n";
        }

        if (reader.HasError())
        {
            std::fprintf(stderr, "sqlconv: %s\n", reader.GetError().c_str());
            return 1;
        }
        return 0;
    }

    struct Command
    {
        std::string_view name;
        int (*run)(CommandArgs const& args);
    };

    constexpr Command Commands[] =
    {
        { "selftest",  SelfTestCommand },
        { "translate", TranslateCommand },
    };
}

int main(int argc, char** argv)
{
    if (argc < 2)
        return PrintUsage();

    std::string_view command = argv[1];
    if (command == "help" || command == "--help" || command == "-h")
    {
        PrintUsage();
        return 0;
    }

    CommandArgs args(argv + 2, argv + argc);
    for (Command const& entry : Commands)
        if (entry.name == command)
            return entry.run(args);

    std::fprintf(stderr, "sqlconv: unknown command '%s'\n\n", argv[1]);
    return PrintUsage();
}
