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

#include "SqlDialect.h"
#include "StatementRewriter.h"

namespace
{
    class IdentityDialect final : public SqlDialect
    {
    public:
        explicit IdentityDialect(DatabaseBackend backend) : _backend(backend) { }

        DatabaseBackend Backend() const override { return _backend; }
        std::string Translate(std::string_view mysqlSql, ISchemaLookup const* /*schema*/) const override { return std::string(mysqlSql); }
        bool NeedsTranslation(std::string_view /*mysqlSql*/) const override { return false; }

    private:
        DatabaseBackend _backend;
    };

    class SqliteDialect final : public SqlDialect
    {
    public:
        DatabaseBackend Backend() const override { return DatabaseBackend::SQLite; }
        std::string Translate(std::string_view mysqlSql, ISchemaLookup const* /*schema*/) const override { return RewriteMySqlForSqlite(mysqlSql); }
        bool NeedsTranslation(std::string_view mysqlSql) const override { return MySqlNeedsSqliteRewrite(mysqlSql); }
    };
}

SqlDialect const& GetDialect(DatabaseBackend backend)
{
    static IdentityDialect const mysql(DatabaseBackend::MySQL);
    static SqliteDialect const sqlite;
    static IdentityDialect const pgsql(DatabaseBackend::PostgreSQL);

    switch (backend)
    {
        case DatabaseBackend::SQLite:     return sqlite;
        case DatabaseBackend::PostgreSQL: return pgsql;
        default:                          return mysql;
    }
}

std::string MySqlEscape(std::string_view str)
{
    std::string result;
    result.reserve(str.size() + str.size() / 8);

    for (char c : str)
    {
        switch (c)
        {
            case '\0':   result += "\\0";  break;
            case '\n':   result += "\\n";  break;
            case '\r':   result += "\\r";  break;
            case '\\':   result += "\\\\"; break;
            case '\'':   result += "\\'";  break;
            case '"':    result += "\\\""; break;
            case '\x1a': result += "\\Z";  break;
            default:     result += c;      break;
        }
    }

    return result;
}
