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
#include "ScriptRunner.h"
#include "SqliteScriptTarget.h"
#include <cstdio>
#include <sqlite3.h>

namespace
{
    struct SchemaCase
    {
        char const* name;
        char const* script;         // MySQL dialect
        char const* query;          // SQLite, rows joined by ';', columns by '|'
        char const* expected;
    };

    // Scripts run with foreign keys off, like base and update files.
    SchemaCase const Cases[] =
    {
        { "create table: types, keys, collation",
            "CREATE TABLE `t` (\n"
            "  `id` int unsigned NOT NULL AUTO_INCREMENT COMMENT 'x',\n"
            "  `name` varchar(12) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin NOT NULL,\n"
            "  `note` text,\n"
            "  `salt` binary(32) DEFAULT NULL,\n"
            "  `kind` enum('A','B') NOT NULL DEFAULT 'B',\n"
            "  `flag` bit(1) NOT NULL DEFAULT b'1',\n"
            "  `rate` float NOT NULL DEFAULT '1',\n"
            "  `at` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,\n"
            "  PRIMARY KEY (`id`) USING BTREE,\n"
            "  UNIQUE KEY `idx_name` (`name`(8)),\n"
            "  KEY (`kind`, `flag`),\n"
            "  FULLTEXT KEY `ft` (`note`),\n"
            "  CONSTRAINT `t_chk_1` CHECK ((`rate` >= 0))\n"
            ") ENGINE=InnoDB AUTO_INCREMENT=10 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci ROW_FORMAT=DYNAMIC COMMENT='t';",
            "SELECT sql FROM sqlite_schema WHERE tbl_name = 't' ORDER BY type, name",
            "CREATE INDEX \"t__ft\" ON \"t\" (\"note\");"
            "CREATE UNIQUE INDEX \"t__idx_name\" ON \"t\" (\"name\");"
            "CREATE INDEX \"t__kind\" ON \"t\" (\"kind\", \"flag\");"
            "CREATE TABLE \"t\" (\n"
            "  \"id\" INTEGER PRIMARY KEY AUTOINCREMENT,\n"
            "  \"name\" VARCHAR(12) NOT NULL,\n"
            "  \"note\" TEXT COLLATE NOCASE,\n"
            "  \"salt\" BLOB,\n"
            "  \"kind\" TEXT NOT NULL DEFAULT 'B' COLLATE NOCASE CHECK (\"kind\" IN ('A', 'B')),\n"
            "  \"flag\" INTEGER NOT NULL DEFAULT 1,\n"
            "  \"rate\" FLOAT NOT NULL DEFAULT 1,\n"
            "  \"at\" TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,\n"
            "  CHECK ((\"rate\" >= 0))\n"
            ");"
            "CREATE TRIGGER \"t__at__on_update\" AFTER UPDATE ON \"t\" FOR EACH ROW WHEN NEW.\"at\" IS OLD.\"at\" BEGIN UPDATE \"t\" SET \"at\" = CURRENT_TIMESTAMP WHERE rowid = NEW.rowid; END" },

        { "auto_increment table option",
            "CREATE TABLE `t` (`id` int NOT NULL AUTO_INCREMENT, PRIMARY KEY (`id`)) AUTO_INCREMENT=42;"
            "INSERT INTO `t` VALUES (NULL);",
            "SELECT id FROM t", "42" },

        { "composite key keeps column order",
            "CREATE TABLE `t` (`b` int NOT NULL, `a` int NOT NULL, `c` int, PRIMARY KEY (`a`,`b`));",
            "SELECT group_concat(name) FROM pragma_table_info('t')", "b,a,c" },

        { "fast path values: hex, binary, escapes, numbers",
            "CREATE TABLE `t` (`i` bigint unsigned, `s` varchar(20), `b` varbinary(8), `f` double, `x` int);"
            "INSERT INTO `t` VALUES (18446744073709551615, 'it\\'s\\n', 0x0102, -1.5, 0x10),(-3, \"q\", _binary 'ab', 1e2, 2.5);",
            "SELECT i, s, hex(b), typeof(b), f, x FROM t ORDER BY rowid",
            "-1|it's\n|0102|blob|-1.5|16;-3|q|6162|blob|100.0|3" },

        { "variables",
            "CREATE TABLE `t` (`id` int, `v` varchar(10));"
            "SET @A := 5, @B = 'x';"
            "SET @C := @A * 2;"
            "INSERT INTO `t` VALUES (@C, @B), (@A+1, 'y');",
            "SELECT id, v FROM t ORDER BY id", "6|y;10|x" },

        { "native add column, rename, index",
            "CREATE TABLE `t` (`a` int NOT NULL, `b` int NOT NULL DEFAULT '0', PRIMARY KEY (`a`), KEY `idx_b` (`b`));"
            "INSERT INTO `t` VALUES (1, 2);"
            "ALTER TABLE `t` ADD COLUMN `c` varchar(5) NOT NULL DEFAULT 'z', CHANGE COLUMN `b` `bb` int NOT NULL DEFAULT 0;"
            "ALTER TABLE `t` DROP INDEX `idx_b`, ADD INDEX `idx_b` (`bb`, `c`);",
            "SELECT (SELECT group_concat(name) FROM pragma_table_info('t')), a, bb, c, (SELECT sql FROM sqlite_schema WHERE name = 't__idx_b') FROM t",
            "a,bb,c|1|2|z|CREATE INDEX \"t__idx_b\" ON \"t\" (\"bb\", \"c\")" },

        { "rebuild: add after, drop column, modify",
            "CREATE TABLE `t` (`a` int NOT NULL, `b` int NOT NULL, `c` int NOT NULL, PRIMARY KEY (`a`), KEY `idx_bc` (`b`, `c`));"
            "INSERT INTO `t` VALUES (1, 2, 3);"
            "ALTER TABLE `t` ADD COLUMN `x` int unsigned NOT NULL AFTER `a`, DROP COLUMN `c`, MODIFY `b` varchar(10) DEFAULT NULL;",
            "SELECT (SELECT group_concat(name || ' ' || type) FROM pragma_table_info('t')), a, x, b, typeof(b), (SELECT sql FROM sqlite_schema WHERE name = 't__idx_bc') FROM t",
            "a INT,x INT UNSIGNED,b VARCHAR(10)|1|0|2|text|CREATE INDEX \"t__idx_bc\" ON \"t\" (\"b\")" },

        { "rebuild: primary key change",
            "CREATE TABLE `t` (`p` int NOT NULL DEFAULT 0, `q` int NOT NULL, `r` int NOT NULL, PRIMARY KEY (`p`, `q`), KEY `idx_r` (`r`, `q`));"
            "INSERT INTO `t` VALUES (0, 1, 7), (0, 2, 8);"
            "ALTER TABLE `t` DROP PRIMARY KEY, ADD PRIMARY KEY (`r`, `q`), DROP INDEX `idx_r`;",
            "SELECT count(*), (SELECT group_concat(name) FROM (SELECT name FROM pragma_table_info('t') WHERE pk > 0 ORDER BY pk)), (SELECT count(*) FROM sqlite_schema WHERE type = 'index' AND sql IS NOT NULL) FROM t",
            "2|r,q|0" },

        { "rebuild: auto_increment added later",
            "CREATE TABLE `t` (`Entry` int NOT NULL, `v` int);"
            "INSERT INTO `t` VALUES (1, 1), (5, 5);"
            "ALTER TABLE `t` ADD PRIMARY KEY (`Entry`);"
            "ALTER TABLE `t` MODIFY `Entry` int NOT NULL AUTO_INCREMENT, AUTO_INCREMENT=3661;"
            "INSERT INTO `t` (`v`) VALUES (9);",
            "SELECT group_concat(Entry) FROM t", "1,5,3661" },

        { "rebuild keeps child rows of a parent table",
            "CREATE TABLE `p` (`id` int NOT NULL, PRIMARY KEY (`id`));"
            "CREATE TABLE `c` (`pid` int NOT NULL, KEY `fk` (`pid`), CONSTRAINT `fk_c` FOREIGN KEY (`pid`) REFERENCES `p` (`id`) ON DELETE CASCADE);"
            "INSERT INTO `p` VALUES (1);"
            "INSERT INTO `c` VALUES (1);"
            "ALTER TABLE `p` ADD COLUMN `n` int NOT NULL DEFAULT 0 FIRST;",
            "SELECT (SELECT count(*) FROM c), (SELECT group_concat(name) FROM pragma_table_info('p'))", "1|n,id" },

        { "enum and on update survive a rebuild",
            "CREATE TABLE `t` (`id` int NOT NULL, `e` enum('x','y') NOT NULL, `ts` timestamp NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, PRIMARY KEY (`id`));"
            "ALTER TABLE `t` ADD COLUMN `z` int NOT NULL DEFAULT 0 AFTER `id`;"
            "INSERT INTO `t` (`id`) VALUES (1);",
            "SELECT e, (SELECT count(*) FROM sqlite_schema WHERE type = 'trigger' AND name = 't__ts__on_update') FROM t", "x|1" },

        { "multi-table delete",
            "CREATE TABLE `a` (`id` int NOT NULL, `k` int NOT NULL, PRIMARY KEY (`id`, `k`));"
            "CREATE TABLE `b` (`id` int NOT NULL, PRIMARY KEY (`id`));"
            "INSERT INTO `a` VALUES (1, 1), (2, 1), (2, 2);"
            "INSERT INTO `b` VALUES (2);"
            "DELETE `x` FROM `a` `x` LEFT JOIN `b` `y` ON `y`.`id` = `x`.`id` WHERE `y`.`id` IS NULL;",
            "SELECT group_concat(id || '-' || k) FROM a", "2-1,2-2" },

        { "update with join",
            "CREATE TABLE `a` (`id` int NOT NULL, `v` int, PRIMARY KEY (`id`));"
            "CREATE TABLE `b` (`id` int NOT NULL, `w` int, PRIMARY KEY (`id`));"
            "INSERT INTO `a` VALUES (1, 0), (2, 0);"
            "INSERT INTO `b` VALUES (2, 7);"
            "UPDATE `a` JOIN `b` ON `b`.`id` = `a`.`id` SET `a`.`v` = `b`.`w` WHERE `b`.`w` > 0;",
            "SELECT group_concat(v) FROM a", "0,7" },

        { "update version limit",
            "CREATE TABLE `version` (`core_version` varchar(255), `cache_id` int);"
            "INSERT INTO `version` VALUES ('a', 1);"
            "UPDATE `version` SET `cache_id`=17 LIMIT 1;",
            "SELECT cache_id FROM version", "17" },

        { "dump header and no-op statements",
            "/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;\n"
            "/*!50503 SET NAMES utf8mb4 */;\n"
            "SET FOREIGN_KEY_CHECKS=0;\n"
            "CREATE TABLE `t` (`id` int);\n"
            "LOCK TABLES `t` WRITE;\n"
            "/*!40000 ALTER TABLE `t` DISABLE KEYS */;\n"
            "INSERT INTO `t` VALUES (1);\n"
            "/*!40000 ALTER TABLE `t` ENABLE KEYS */;\n"
            "UNLOCK TABLES;\n"
            "START TRANSACTION;\n"
            "COMMIT;",
            "SELECT count(*) FROM t", "1" },
    };

    std::string QueryText(sqlite3* db, char const* sql, std::string& error)
    {
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        {
            error = sqlite3_errmsg(db);
            return {};
        }

        std::string result;
        bool firstRow = true;
        while (sqlite3_step(stmt) == SQLITE_ROW)
        {
            if (!firstRow)
                result += ";";
            firstRow = false;
            for (int i = 0; i < sqlite3_column_count(stmt); ++i)
            {
                if (i)
                    result += "|";
                unsigned char const* text = sqlite3_column_text(stmt, i);
                result += text ? reinterpret_cast<char const*>(text) : "NULL";
            }
        }
        sqlite3_finalize(stmt);
        return result;
    }

    std::string WithoutSpaces(std::string_view text)
    {
        std::string result;
        for (char c : text)
            if (c != ' ' && c != '\n')
                result += c;
        return result;
    }

    bool RoundTrips(sqlite3* db, std::string& error)
    {
        sqlite3_stmt* stmt = nullptr;
        sqlite3_prepare_v2(db, "SELECT name, sql FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\'", -1, &stmt, nullptr);
        bool ok = true;
        while (ok && sqlite3_step(stmt) == SQLITE_ROW)
        {
            std::string const name = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0));
            std::string const stored = reinterpret_cast<char const*>(sqlite3_column_text(stmt, 1));
            TableModel model;
            TableModel reparsed;
            bool ifNotExists = false;
            if (!SqliteLoadTable(db, name, model, error))
                ok = false;
            else if (!ParseCreateTable(SqliteCreateTable(model, name, false), reparsed, ifNotExists, error, DdlSyntax::Sqlite))
                ok = false;
            else if (SqliteCreateTable(reparsed, name, false) != SqliteCreateTable(model, name, false))
            {
                error = "table " + name + " does not round-trip:\n" + SqliteCreateTable(model, name, false) + "\n  vs\n" + SqliteCreateTable(reparsed, name, false);
                ok = false;
            }
            else if (WithoutSpaces(stored) != WithoutSpaces(SqliteCreateTable(model, name, false)))
            {
                error = "table " + name + " reads back differently:\n" + SqliteCreateTable(model, name, false) + "\n  vs stored\n" + stored;
                ok = false;
            }
        }
        sqlite3_finalize(stmt);
        return ok;
    }
}

int SchemaSelfTestCommand(CommandArgs const& /*args*/)
{
    uint32 failed = 0;
    for (SchemaCase const& test : Cases)
    {
        sqlite3* db = nullptr;
        std::string error;
        db = OpenScratchDatabase(":memory:", true, true, error);
        std::string actual;
        if (db)
        {
            SqliteScriptTarget target(db);
            ScriptRunnerOptions options;
            options.disableForeignKeys = true;
            ScriptRunner runner(target, options);
            if (!runner.RunText(test.script, "case"))
                error = runner.GetError().ToString();
            else
            {
                actual = QueryText(db, test.query, error);
                if (error.empty() && actual == test.expected)
                    RoundTrips(db, error);
            }
            sqlite3_close_v2(db);
        }

        if (error.empty() && actual == test.expected)
            continue;

        ++failed;
        std::printf("FAIL %s\n", test.name);
        if (!error.empty())
            std::printf("  error: %s\n", error.c_str());
        else
            std::printf("  expected: %s\n  actual:   %s\n", test.expected, actual.c_str());
    }

    std::printf("schema selftest: %zu cases, %u failed\n", std::size(Cases), failed);
    return failed ? 1 : 0;
}
