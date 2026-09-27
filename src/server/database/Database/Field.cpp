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

#include "Field.h"
#include "Errors.h"
#include "Log.h"
#include "RowSet.h"
#include "StringConvert.h"
#include "StringFormat.h"

Field::Field() : _value(nullptr), _rows(nullptr), meta(nullptr) { }

namespace
{
    template<typename T>
    constexpr T GetDefaultValue()
    {
        if constexpr (std::is_same_v<T, bool>)
            return false;
        else if constexpr (std::is_integral_v<T>)
            return 0;
        else if constexpr (std::is_floating_point_v<T>)
            return 1.0f;
        else if constexpr (std::is_same_v<T, std::vector<uint8>> || std::is_same_v<std::string_view, T>)
            return {};
        else
            return "";
    }

#ifdef ACORE_STRICT_DATABASE_TYPE_CHECKS
    template<typename T>
    inline bool IsCorrectFieldType(DatabaseFieldTypes type)
    {
        // Int8
        if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, int8> || std::is_same_v<T, uint8>)
        {
            if (type == DatabaseFieldTypes::Int8)
                return true;
        }

        // In16
        if constexpr (std::is_same_v<T, uint16> || std::is_same_v<T, int16>)
        {
            if (type == DatabaseFieldTypes::Int16)
                return true;
        }

        // Int32
        if constexpr (std::is_same_v<T, uint32> || std::is_same_v<T, int32>)
        {
            if (type == DatabaseFieldTypes::Int32)
                return true;
        }

        // Int64
        if constexpr (std::is_same_v<T, uint64> || std::is_same_v<T, int64>)
        {
            if (type == DatabaseFieldTypes::Int64)
                return true;
        }

        // float
        if constexpr (std::is_same_v<T, float>)
        {
            if (type == DatabaseFieldTypes::Float)
                return true;
        }

        // dobule
        if constexpr (std::is_same_v<T, double>)
        {
            if (type == DatabaseFieldTypes::Double || type == DatabaseFieldTypes::Decimal)
                return true;
        }

        // Binary
        if constexpr (std::is_same_v<T, Binary>)
        {
            if (type == DatabaseFieldTypes::Binary)
                return true;
        }

        return false;
    }
#endif
}

bool Field::IsNull() const
{
    return !_value || _value->IsNull();
}

void Field::GetBinarySizeChecked(uint8* buf, std::size_t length) const
{
    std::string_view bytes = IsNull() ? std::string_view() : _value->AsBytes();
    ASSERT(!IsNull() && (bytes.size() == length), "Expected {}-byte binary blob, got {}data ({} bytes) instead", length, IsNull() ? "no " : "", bytes.size());
    memcpy(buf, bytes.data(), length);
}

void Field::SetValue(FieldValue const* value, RowSet const* rows)
{
    _value = value;
    _rows = rows;
}

bool Field::IsType(DatabaseFieldTypes type) const
{
    return meta->Type == type;
}

bool Field::IsNumeric() const
{
    return (meta->Type == DatabaseFieldTypes::Int8 ||
        meta->Type == DatabaseFieldTypes::Int16 ||
        meta->Type == DatabaseFieldTypes::Int32 ||
        meta->Type == DatabaseFieldTypes::Int64 ||
        meta->Type == DatabaseFieldTypes::Float ||
        meta->Type == DatabaseFieldTypes::Double);
}

void Field::LogWrongType(std::string_view getter, std::string_view typeName) const
{
    LOG_WARN("sql.sql", "Warning: {}<{}> on {} field {}.{} ({}.{}) at index {}.",
        getter, typeName, meta->TypeName, meta->TableAlias, meta->Alias, meta->TableName, meta->Name, meta->Index);
}

void Field::SetMetadata(QueryResultFieldMetadata const* fieldMeta)
{
    meta = fieldMeta;
}

template<typename T>
T Field::GetData() const
{
    static_assert(std::is_arithmetic_v<T>, "Unsurropt type for Field::GetData()");

    if (IsNull())
        return GetDefaultValue<T>();

#ifdef ACORE_STRICT_DATABASE_TYPE_CHECKS
    if (!IsCorrectFieldType<T>(meta->Type))
    {
        LogWrongType(__FUNCTION__, typeid(T).name());
        //return GetDefaultValue<T>();
    }
#endif

    switch (_value->Type)
    {
        case FieldValueType::Int:
            return static_cast<T>(_value->Int);
        case FieldValueType::Real:
            return static_cast<T>(_value->Real);
        default:
            break;
    }

    std::string_view text = _value->AsBytes();

    if (Optional<T> result = Acore::StringTo<T>(text))
        return *result;

    if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
    {
        if (Optional<int64> result = Acore::StringTo<int64>(text))
            return static_cast<T>(*result);

        if (Optional<double> result = Acore::StringTo<double>(text))
            return static_cast<T>(*result);
    }

    LOG_FATAL("sql.sql", "> Incorrect value '{}' for type '{}'", text, typeid(T).name());
    LOG_FATAL("sql.sql", "> Table name '{}'. Field name '{}'", meta->TableName, meta->Name);
    return GetDefaultValue<T>();
}

template bool Field::GetData() const;
template uint8 Field::GetData() const;
template uint16 Field::GetData() const;
template uint32 Field::GetData() const;
template uint64 Field::GetData() const;
template int8 Field::GetData() const;
template int16 Field::GetData() const;
template int32 Field::GetData() const;
template int64 Field::GetData() const;
template float Field::GetData() const;
template double Field::GetData() const;

std::string Field::GetDataString() const
{
    if (IsNull())
        return "";

#ifdef ACORE_STRICT_DATABASE_TYPE_CHECKS
    if (IsNumeric() && !_value->IsBytes())
        LogWrongType(__FUNCTION__, "std::string");
#endif

    switch (_value->Type)
    {
        case FieldValueType::Int:
            return Acore::StringFormat("{}", _value->Int);
        case FieldValueType::Real:
            if (meta->Type == DatabaseFieldTypes::Float)
                return Acore::StringFormat("{}", static_cast<float>(_value->Real));
            return Acore::StringFormat("{}", _value->Real);
        default:
            return std::string(_value->AsBytes());
    }
}

std::string_view Field::GetDataStringView() const
{
    if (IsNull())
        return {};

    if (_value->IsBytes())
        return _value->AsBytes();

#ifdef ACORE_STRICT_DATABASE_TYPE_CHECKS
    if (IsNumeric())
        LogWrongType(__FUNCTION__, "std::string_view");
#endif

    return _rows ? _rows->KeepText(GetDataString()) : std::string_view();
}

Binary Field::GetDataBinary() const
{
    Binary result = {};
    if (IsNull() || !_value->IsBytes() || !_value->Size)
        return result;

#ifdef ACORE_STRICT_DATABASE_TYPE_CHECKS
    if (!IsCorrectFieldType<Binary>(meta->Type))
    {
        LogWrongType(__FUNCTION__, "Binary");
        return {};
    }
#endif

    std::string_view bytes = _value->AsBytes();
    result.assign(bytes.begin(), bytes.end());
    return result;
}
