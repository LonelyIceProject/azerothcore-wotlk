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

#include "QueryResult.h"
#include "Errors.h"
#include "Field.h"
#include "RowSet.h"

ResultSet::ResultSet(std::unique_ptr<RowSet> rows) :
    _rows(std::move(rows)),
    _rowCount(_rows->GetRowCount()),
    _nextRow(0),
    _fieldCount(_rows->GetFieldCount())
{
    _currentRow = std::make_unique<Field[]>(_fieldCount);

    for (uint32 i = 0; i < _fieldCount; i++)
        _currentRow[i].SetMetadata(&_rows->GetColumn(i));
}

ResultSet::~ResultSet() = default;

bool ResultSet::NextRow()
{
    if (_nextRow >= _rowCount)
        return false;

    FieldValue const* row = _rows->GetRow(_nextRow++);
    for (uint32 i = 0; i < _fieldCount; i++)
        _currentRow[i].SetValue(&row[i], _rows.get());

    return true;
}

std::string ResultSet::GetFieldName(uint32 index) const
{
    ASSERT(index < _fieldCount);
    return _rows->GetColumn(index).Alias;
}

Field const& ResultSet::operator[](std::size_t index) const
{
    ASSERT(index < _fieldCount);
    return _currentRow[index];
}

void ResultSet::AssertRows(std::size_t sizeRows)
{
    ASSERT(sizeRows == _fieldCount);
}

PreparedResultSet::PreparedResultSet(std::unique_ptr<RowSet> rows) :
    m_rowSet(std::move(rows)),
    m_rowCount(m_rowSet->GetRowCount()),
    m_rowPosition(0),
    m_fieldCount(m_rowSet->GetFieldCount())
{
    RowSet const& rowSet = *m_rowSet;

    m_rows.resize(uint32(m_rowCount) * m_fieldCount);

    for (uint64 row = 0; row < m_rowCount; ++row)
    {
        FieldValue const* values = rowSet.GetRow(row);
        for (uint32 fIndex = 0; fIndex < m_fieldCount; ++fIndex)
        {
            Field& field = m_rows[uint32(row) * m_fieldCount + fIndex];
            field.SetMetadata(&rowSet.GetColumn(fIndex));
            field.SetValue(&values[fIndex], &rowSet);
        }
    }
}

PreparedResultSet::~PreparedResultSet() = default;

bool PreparedResultSet::NextRow()
{
    /// Only updates the m_rowPosition so upper level code knows in which element
    /// of the rows vector to look
    if (++m_rowPosition >= m_rowCount)
        return false;

    return true;
}

Field* PreparedResultSet::Fetch() const
{
    ASSERT(m_rowPosition < m_rowCount);
    return const_cast<Field*>(&m_rows[uint32(m_rowPosition) * m_fieldCount]);
}

Field const& PreparedResultSet::operator[](std::size_t index) const
{
    ASSERT(m_rowPosition < m_rowCount);
    ASSERT(index < m_fieldCount);
    return m_rows[uint32(m_rowPosition) * m_fieldCount + index];
}

void PreparedResultSet::AssertRows(std::size_t sizeRows)
{
    ASSERT(m_rowPosition < m_rowCount);
    ASSERT(sizeRows == m_fieldCount, "> Tuple size != count fields");
}
