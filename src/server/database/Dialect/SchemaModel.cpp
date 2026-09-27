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

#include "SchemaModel.h"

bool IdentifierEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = char(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = char(cb - 'A' + 'a');
        if (ca != cb)
            return false;
    }

    return true;
}

ColumnModel const* TableModel::FindColumn(std::string_view column) const
{
    for (ColumnModel const& c : columns)
        if (IdentifierEquals(c.name, column))
            return &c;

    return nullptr;
}

ColumnModel* TableModel::FindColumn(std::string_view column)
{
    for (ColumnModel& c : columns)
        if (IdentifierEquals(c.name, column))
            return &c;

    return nullptr;
}

int32 TableModel::ColumnIndex(std::string_view column) const
{
    for (std::size_t i = 0; i < columns.size(); ++i)
        if (IdentifierEquals(columns[i].name, column))
            return int32(i);

    return -1;
}

IndexModel const* TableModel::FindIndex(std::string_view index) const
{
    for (IndexModel const& i : indexes)
        if (IdentifierEquals(i.name, index))
            return &i;

    return nullptr;
}

IndexModel const* TableModel::PrimaryKey() const
{
    for (IndexModel const& i : indexes)
        if (i.kind == IndexKind::Primary)
            return &i;

    return nullptr;
}

std::vector<std::string> TableModel::ColumnNames() const
{
    std::vector<std::string> names;
    names.reserve(columns.size());
    for (ColumnModel const& c : columns)
        names.push_back(c.name);

    return names;
}
