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

#ifndef _SQLDIALECT_H
#define _SQLDIALECT_H

#include "DatabaseBackend.h"
#include <string>
#include <string_view>

struct TableModel;

struct ISchemaLookup
{
    virtual ~ISchemaLookup() = default;
    virtual TableModel const* Find(std::string_view table) const = 0;
};

// All SQL above the driver is MySQL-dialect source text; each backend's dialect emits its own SQL from it.
// Translation never changes the number or order of '?' parameters.
class SqlDialect
{
public:
    virtual ~SqlDialect() = default;

    [[nodiscard]] virtual DatabaseBackend Backend() const = 0;
    // schema may be nullptr (SQLite ignores it; PostgreSQL needs primary keys for ON CONFLICT).
    [[nodiscard]] virtual std::string Translate(std::string_view mysqlSql, ISchemaLookup const* schema = nullptr) const = 0;
    // Cheap pre-check for ad-hoc SQL; false means Translate() would return the input unchanged.
    [[nodiscard]] virtual bool NeedsTranslation(std::string_view mysqlSql) const = 0;
};

SqlDialect const& GetDialect(DatabaseBackend backend);

// mysql_real_escape_string-compatible escaping, used by EscapeString for every backend.
std::string MySqlEscape(std::string_view str);

#endif
