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

#include "SQLiteBackend.h"
#include "SQLiteStatement.h"

namespace
{
    // "alias.table" addresses an attached database.
    std::pair<std::string_view, std::string_view> SplitQualifiedName(std::string_view table)
    {
        std::size_t const dot = table.find('.');
        if (dot == std::string_view::npos)
            return { "main", table };

        return { table.substr(0, dot), table.substr(dot + 1) };
    }
}

bool SQLiteBackend::HasAnyTable()
{
    std::unique_ptr<RowSet> result = QueryText("SELECT 1 FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' LIMIT 1", {});
    return result && result->GetRowCount() > 0;
}

bool SQLiteBackend::TableExists(std::string_view table)
{
    auto [schema, name] = SplitQualifiedName(table);
    std::unique_ptr<RowSet> result = QueryText("SELECT 1 FROM " + SQLite::QuoteIdentifier(schema)
        + ".sqlite_schema WHERE type IN ('table', 'view') AND name = ? COLLATE NOCASE LIMIT 1", { name });
    return result && result->GetRowCount() > 0;
}

std::vector<std::string> SQLiteBackend::ListColumns(std::string_view table)
{
    auto [schema, name] = SplitQualifiedName(table);
    std::unique_ptr<RowSet> result = QueryText("SELECT name FROM pragma_table_info(?, ?) ORDER BY cid", { name, schema });

    std::vector<std::string> columns;
    if (!result)
        return columns;

    columns.reserve(std::size_t(result->GetRowCount()));
    for (uint64 row = 0; row < result->GetRowCount(); ++row)
        columns.emplace_back(result->Get(row, 0).AsBytes());

    return columns;
}
