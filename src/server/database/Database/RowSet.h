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

#ifndef _ROWSET_H
#define _ROWSET_H

#include "Define.h"
#include "Field.h"
#include <algorithm>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

enum class FieldValueType : uint8
{
    Null,
    Int,
    Real,
    Text,
    Blob
};

// Text is NUL-terminated in the arena, Size excludes the terminator.
struct FieldValue
{
    FieldValueType Type = FieldValueType::Null;
    uint32 Size = 0;
    union
    {
        int64 Int = 0;
        double Real;
        char const* Bytes;
    };

    [[nodiscard]] bool IsNull() const { return Type == FieldValueType::Null; }
    [[nodiscard]] bool IsBytes() const { return Type == FieldValueType::Text || Type == FieldValueType::Blob; }
    [[nodiscard]] std::string_view AsBytes() const { return IsBytes() ? std::string_view(Bytes, Size) : std::string_view(); }

    void SetNull() { Type = FieldValueType::Null; Size = 0; Int = 0; }
    void SetInt(int64 value) { Type = FieldValueType::Int; Size = 0; Int = value; }
    void SetReal(double value) { Type = FieldValueType::Real; Size = 0; Real = value; }
};

// Materialized result shared by all backends. Row-major cells, byte payloads in a
// block arena whose blocks never move, so FieldValue::Bytes stays valid for the RowSet lifetime.
class RowSet
{
public:
    explicit RowSet(std::vector<QueryResultFieldMetadata> columns) : _columns(std::move(columns)) { }

    RowSet(RowSet const&) = delete;
    RowSet& operator=(RowSet const&) = delete;

    [[nodiscard]] uint32 GetFieldCount() const { return uint32(_columns.size()); }
    [[nodiscard]] uint64 GetRowCount() const { return _columns.empty() ? 0 : _cells.size() / _columns.size(); }

    [[nodiscard]] std::vector<QueryResultFieldMetadata> const& GetColumns() const { return _columns; }
    [[nodiscard]] QueryResultFieldMetadata const& GetColumn(uint32 index) const { return _columns[index]; }
    QueryResultFieldMetadata& GetColumn(uint32 index) { return _columns[index]; }

    [[nodiscard]] FieldValue const* GetRow(uint64 row) const { return _cells.data() + row * _columns.size(); }
    [[nodiscard]] FieldValue const& Get(uint64 row, uint32 column) const { return GetRow(row)[column]; }

    void ReserveRows(uint64 rows) { _cells.reserve(rows * _columns.size()); }

    // Appends a row of Null cells. The pointer is invalidated by the next AppendRow().
    FieldValue* AppendRow()
    {
        std::size_t offset = _cells.size();
        _cells.resize(offset + _columns.size());
        return _cells.data() + offset;
    }

    void SetText(FieldValue& cell, std::string_view text)
    {
        cell.Type = FieldValueType::Text;
        cell.Size = uint32(text.size());
        cell.Bytes = Store(text.data(), text.size(), true);
    }

    void SetBlob(FieldValue& cell, void const* data, std::size_t size)
    {
        cell.Type = FieldValueType::Blob;
        cell.Size = uint32(size);
        cell.Bytes = Store(data, size, false);
    }

private:
    static constexpr std::size_t BlockSize = 64 * 1024;

    char const* Store(void const* data, std::size_t size, bool terminate)
    {
        std::size_t needed = size + (terminate ? 1 : 0);
        if (needed == 0)
            return "";

        if (_blocks.empty() || _blockUsed + needed > _blockCapacity)
        {
            _blockCapacity = std::max(BlockSize, needed);
            _blocks.push_back(std::make_unique_for_overwrite<char[]>(_blockCapacity));
            _blockUsed = 0;
        }

        char* dest = _blocks.back().get() + _blockUsed;
        if (size)
            std::memcpy(dest, data, size);
        if (terminate)
            dest[size] = '\0';

        _blockUsed += needed;
        return dest;
    }

    std::vector<QueryResultFieldMetadata> _columns;
    std::vector<FieldValue> _cells;
    std::vector<std::unique_ptr<char[]>> _blocks;
    std::size_t _blockUsed = 0;
    std::size_t _blockCapacity = 0;
};

#endif
