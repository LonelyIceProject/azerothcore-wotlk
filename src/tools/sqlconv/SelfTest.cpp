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
#include "SqliteFunctions.h"
#include <sqlite3.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using CommandArgs = std::vector<std::string_view>;

namespace
{
    class SelfTest
    {
    public:
        SelfTest()
        {
            sqlite3_open(":memory:", &_db);
            RegisterMySqlFunctions(_db);
        }

        ~SelfTest()
        {
            sqlite3_close(_db);
        }

        SelfTest(SelfTest const&) = delete;
        SelfTest& operator=(SelfTest const&) = delete;

        void Check(bool ok, std::string const& what, std::string const& detail = {})
        {
            ++_total;
            if (ok)
                return;
            ++_failed;
            std::printf("FAIL %s\n", what.c_str());
            if (!detail.empty())
                std::printf("     %s\n", detail.c_str());
        }

        void CheckEqual(std::string_view actual, std::string_view expected, std::string const& what)
        {
            Check(actual == expected, what, "expected: " + std::string(expected) + "\n     actual:   " + std::string(actual));
        }

        void RunLexerTests();
        void RunEscapeTests();
        bool RunCaseFile(std::filesystem::path const& path);

        int Finish() const
        {
            std::printf("%s: %u checks, %u failed\n", _failed ? "FAILED" : "OK", _total, _failed);
            return _failed ? 1 : 0;
        }

    private:
        sqlite3* _db = nullptr;
        uint32 _total = 0;
        uint32 _failed = 0;

        bool Exec(std::string const& sql, std::string& error)
        {
            char* message = nullptr;
            if (sqlite3_exec(_db, sql.c_str(), nullptr, nullptr, &message) == SQLITE_OK)
                return true;
            error = message ? message : "unknown error";
            sqlite3_free(message);
            return false;
        }

        bool Prepare(std::string const& sql, std::string& error)
        {
            sqlite3_stmt* stmt = nullptr;
            char const* tail = nullptr;
            int rc = sqlite3_prepare_v2(_db, sql.c_str(), static_cast<int>(sql.size()), &stmt, &tail);
            if (rc != SQLITE_OK)
            {
                error = sqlite3_errmsg(_db);
                return false;
            }
            sqlite3_finalize(stmt);
            if (tail && *tail)
            {
                error = "trailing text after first statement: " + std::string(tail);
                return false;
            }
            return true;
        }

        bool Evaluate(std::string const& sql, std::string& value, std::string& error)
        {
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(_db, sql.c_str(), static_cast<int>(sql.size()), &stmt, nullptr) != SQLITE_OK)
            {
                error = sqlite3_errmsg(_db);
                return false;
            }

            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_ROW)
            {
                if (sqlite3_column_type(stmt, 0) == SQLITE_NULL)
                    value = "NULL";
                else
                    value.assign(reinterpret_cast<char const*>(sqlite3_column_text(stmt, 0)), sqlite3_column_bytes(stmt, 0));
            }
            else if (rc == SQLITE_DONE)
                value = "<no rows>";
            else
                error = sqlite3_errmsg(_db);

            sqlite3_finalize(stmt);
            return rc == SQLITE_ROW || rc == SQLITE_DONE;
        }
    };

    std::string Describe(std::vector<MySqlToken> const& tokens)
    {
        static constexpr char const* Names[] =
        {
            "End", "Ws", "Comment", "Version", "Ident", "QIdent", "Str", "Num", "Hex", "Bit", "Var", "SysVar", "Param", "Op", "Punct", "Error"
        };

        std::string result;
        for (MySqlToken const& token : tokens)
        {
            if (!result.empty())
                result += ' ';
            result += Names[static_cast<uint8>(token.type)];
            result += '[';
            result += token.text;
            result += ']';
        }
        return result;
    }

    std::vector<MySqlStatementText> ReadAll(std::string const& script, std::string& error)
    {
        std::istringstream in(script);
        MySqlStatementReader reader(in);
        std::vector<MySqlStatementText> result;
        MySqlStatementText statement;
        while (reader.Next(statement))
            result.push_back(statement);
        error = reader.GetError();
        return result;
    }

    void SelfTest::RunLexerTests()
    {
        struct LexCase
        {
            std::string_view sql;
            std::string_view tokens;
        };

        static constexpr LexCase Cases[] =
        {
            { "SELECT `a``b`, 'it\\'s', \"q\"\"q\" FROM t", "Ident[SELECT] QIdent[`a``b`] Punct[,] Str['it\\'s'] Punct[,] Str[\"q\"\"q\"] Ident[FROM] Ident[t]" },
            { "x<=>y AND a:=1 OR b!=c||d", "Ident[x] Op[<=>] Ident[y] Ident[AND] Ident[a] Op[:=] Num[1] Ident[OR] Ident[b] Op[!=] Ident[c] Op[||] Ident[d]" },
            { "0x1F X'2a' b'01' 0b10 1.5e3 .5 12abc", "Hex[0x1F] Hex[X'2a'] Bit[b'01'] Bit[0b10] Num[1.5e3] Num[.5] Ident[12abc]" },
            { "@v @`w x` @@session.sql_mode ?", "Var[@v] Var[@`w x`] SysVar[@@session.sql_mode] Param[?]" },
            { "t.5col, c.order", "Ident[t] Punct[.] Ident[5col] Punct[,] Ident[c] Punct[.] Ident[order]" },
            { "a-- x\n-b --\n#c\n/*d*/ /*!40101 e */", "Ident[a] Op[-] Ident[b] Version[/*!40101 e */]" },
            { "'unterminated", "Error['unterminated]" },
            { "_utf8mb4'x' N'y'", "Ident[_utf8mb4] Str['x'] Ident[N] Str['y']" },
        };

        for (LexCase const& c : Cases)
        {
            CheckEqual(Describe(MySqlTokenize(c.sql)), c.tokens, "lex: " + std::string(c.sql));
        }

        {
            MySqlLexer lexer("a\n\nb /* x\ny */ c");
            MySqlToken a = lexer.NextSignificant();
            MySqlToken b = lexer.PeekSignificant();
            MySqlToken b2 = lexer.NextSignificant();
            MySqlToken c = lexer.NextSignificant();
            Check(a.line == 1 && b.line == 3 && b2.line == 3 && b.offset == b2.offset && c.line == 4 && lexer.NextSignificant().type == MySqlTokenType::End, "lex: line numbers and peek");
        }

        CheckEqual(MySqlDecodeString("'a\\'b\\\\c\\nd\\%e\\_f''g'"), "a'b\\c\nd\\%e\\_f'g", "decode string");
        CheckEqual(MySqlDecodeString("\"say \\\"hi\\\" \"\"x\"\"\""), "say \"hi\" \"x\"", "decode double-quoted string");
        CheckEqual(MySqlDecodeString("'\\0\\Z\\t'"), std::string_view("\0\x1a\t", 3), "decode control escapes");
        CheckEqual(MySqlUnquoteIdentifier("`a``b`"), "a`b", "unquote identifier");
        CheckEqual(MySqlUnquoteIdentifier("plain"), "plain", "unquote bare identifier");
        CheckEqual(MySqlDecodeHex("0x414243"), "ABC", "decode hex 0x");
        CheckEqual(MySqlDecodeHex("X'0a'"), "\n", "decode hex X''");
        CheckEqual(MySqlDecodeHex("0xABC"), std::string_view("\x0a\xbc", 2), "decode odd hex");
        CheckEqual(MySqlVersionCommentBody("/*!40101 SET NAMES utf8 */"), "SET NAMES utf8", "version comment body");

        std::string error;
        std::vector<MySqlStatementText> statements = ReadAll(
            "-- header; comment\n"
            "/*!40101 SET NAMES utf8 */;\n"
            "INSERT INTO t VALUES ('a;b', \"c;\\\"d\", `e;f`); # trailing; comment\n"
            "\n"
            ";;\n"
            "/* block ; */ UPDATE t SET a = 1\n"
            "WHERE b = 2;\n"
            "SELECT 1", error);
        Check(error.empty() && statements.size() == 4, "reader: statement count", "got " + std::to_string(statements.size()) + " " + error);
        if (statements.size() == 4)
        {
            CheckEqual(statements[0].sql, "/*!40101 SET NAMES utf8 */", "reader: version comment statement");
            Check(statements[0].line == 2 && statements[0].index == 1, "reader: first statement line");
            CheckEqual(statements[1].sql, "INSERT INTO t VALUES ('a;b', \"c;\\\"d\", `e;f`)", "reader: quoted semicolons");
            Check(statements[1].line == 3 && statements[1].index == 2, "reader: second statement line");
            CheckEqual(statements[2].sql, "UPDATE t SET a = 1\nWHERE b = 2", "reader: leading comment dropped");
            Check(statements[2].line == 6 && statements[2].index == 3, "reader: third statement line", std::to_string(statements[2].line));
            CheckEqual(statements[3].sql, "SELECT 1", "reader: last statement without ';'");
            Check(statements[3].line == 8, "reader: last statement line");
        }

        ReadAll("SELECT 1;\nSELECT 'open\n\n", error);
        Check(error.find("line 2") != std::string::npos, "reader: unterminated string error", error);

        std::string big = "INSERT INTO t VALUES ";
        for (uint32 i = 0; i < 40000; ++i)
            big += (i ? ",(" : "(") + std::to_string(i) + ",'x;\\'y" + std::string(i % 7, ';') + "')";
        big += ";\n-- tail\nSELECT 2;";
        statements = ReadAll(big, error);
        Check(error.empty() && statements.size() == 2 && statements[0].sql.size() == big.find(";\n-- tail") && statements[1].sql == "SELECT 2" && statements[1].line == 3,
            "reader: statement larger than the read buffer");
    }

    void SelfTest::RunEscapeTests()
    {
        CheckEqual(MySqlEscape(std::string_view("a'b\"c\\d\ne\rf\0g\x1a", 14)), "a\\'b\\\"c\\\\d\\ne\\rf\\0g\\Z", "MySqlEscape");

        SqlDialect const& sqlite = GetDialect(DatabaseBackend::SQLite);
        std::string const value = std::string("Earthen's \\ path\nline\t", 22);
        std::string const sql = "SELECT '" + MySqlEscape(value) + "'";
        std::string result;
        std::string error;
        Check(Evaluate(sqlite.Translate(sql), result, error) && result == value, "MySqlEscape round trip through the translator", error + " " + result);

        SqlDialect const& mysql = GetDialect(DatabaseBackend::MySQL);
        Check(!mysql.NeedsTranslation("SELECT `a` FROM t") && mysql.Translate("SELECT `a` FROM t") == "SELECT `a` FROM t", "mysql dialect is identity");
    }

    std::string_view Trim(std::string_view text)
    {
        while (!text.empty() && (text.back() == '\r' || text.back() == ' '))
            text.remove_suffix(1);
        return text;
    }

    bool SelfTest::RunCaseFile(std::filesystem::path const& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            Check(false, "open " + path.string());
            return false;
        }

        SqlDialect const& dialect = GetDialect(DatabaseBackend::SQLite);
        std::string const name = path.filename().string();
        std::string input;
        std::string line;
        uint32 lineNo = 0;
        while (std::getline(file, line))
        {
            ++lineNo;
            std::string_view text = Trim(line);
            if (text.empty() || text.front() == '#')
                continue;

            std::string const where = name + ":" + std::to_string(lineNo);
            if (text.starts_with("@ "))
            {
                std::string error;
                Check(Exec(std::string(text.substr(2)), error), where + " schema", error);
            }
            else if (text.starts_with("> "))
            {
                input = std::string(text.substr(2));
                std::string translated = dialect.Translate(input);
                Check(dialect.NeedsTranslation(input) || translated == input, where + " NeedsTranslation() is false but Translate() changes the text", translated);
            }
            else if (text == "=" || text == "~" || text.starts_with("= ") || text.starts_with("~ "))
            {
                std::string_view expected = text.size() > 2 ? text.substr(2) : std::string_view();
                std::string translated = dialect.Translate(input);
                CheckEqual(translated, expected, where + " " + input);
                if (text.front() == '=' && !translated.empty())
                {
                    std::string error;
                    Check(Prepare(translated, error), where + " prepare: " + translated, error);
                }
            }
            else if (text.starts_with("-> "))
            {
                std::string value;
                std::string error;
                std::string translated = dialect.Translate(input);
                if (Evaluate(translated, value, error))
                    CheckEqual(value, text.substr(3), where + " " + input);
                else
                    Check(false, where + " " + translated, error);
            }
            else
                Check(false, where + " unrecognized line", std::string(text));
        }
        return true;
    }
}

int SelfTestCommand(CommandArgs const& args)
{
    std::filesystem::path casesDir = args.empty() ? std::filesystem::path(__FILE__).parent_path() / "tests" : std::filesystem::path(args[0]);

    SelfTest test;
    test.RunLexerTests();
    test.RunEscapeTests();

    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (auto const& entry : std::filesystem::directory_iterator(casesDir, ec))
        if (entry.is_regular_file() && entry.path().extension() == ".txt")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    test.Check(!files.empty(), "case files in " + casesDir.string(), "use: sqlconv selftest <cases dir>");
    for (std::filesystem::path const& file : files)
        test.RunCaseFile(file);

    return test.Finish();
}
