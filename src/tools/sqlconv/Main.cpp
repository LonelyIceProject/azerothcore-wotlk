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

#include <cstdio>
#include <string_view>

namespace
{
    int PrintUsage()
    {
        std::fputs(
            "Usage: sqlconv <command> [options]\n"
            "\n"
            "Commands:\n"
            "  selftest                                     run built-in lexer/translator/DDL/ALTER checks\n"
            "  translate <file.sql>                         print the SQLite translation of a MySQL script\n"
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

    std::fprintf(stderr, "sqlconv: unknown command '%s'\n\n", argv[1]);
    return PrintUsage();
}
