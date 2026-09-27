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

#include "MySQLStatement.h"
#include "Errors.h"
#include "Log.h"
#include "MySQLBackend.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include <algorithm>
#include <cstring>

template<typename T>
struct MySQLType { };

template<> struct MySQLType<uint8> : std::integral_constant<enum_field_types, MYSQL_TYPE_TINY> { };
template<> struct MySQLType<uint16> : std::integral_constant<enum_field_types, MYSQL_TYPE_SHORT> { };
template<> struct MySQLType<uint32> : std::integral_constant<enum_field_types, MYSQL_TYPE_LONG> { };
template<> struct MySQLType<uint64> : std::integral_constant<enum_field_types, MYSQL_TYPE_LONGLONG> { };
template<> struct MySQLType<int8> : std::integral_constant<enum_field_types, MYSQL_TYPE_TINY> { };
template<> struct MySQLType<int16> : std::integral_constant<enum_field_types, MYSQL_TYPE_SHORT> { };
template<> struct MySQLType<int32> : std::integral_constant<enum_field_types, MYSQL_TYPE_LONG> { };
template<> struct MySQLType<int64> : std::integral_constant<enum_field_types, MYSQL_TYPE_LONGLONG> { };
template<> struct MySQLType<float> : std::integral_constant<enum_field_types, MYSQL_TYPE_FLOAT> { };
template<> struct MySQLType<double> : std::integral_constant<enum_field_types, MYSQL_TYPE_DOUBLE> { };

MySQLStatement::MySQLStatement(MYSQL_STMT* stmt, std::string_view queryString) :
    m_Mstmt(stmt),
    m_queryString(queryString)
{
    /// Initialize variable parameters
    m_paramCount = mysql_stmt_param_count(stmt);
    m_paramsSet.assign(m_paramCount, false);
    m_bind.resize(m_paramCount);
    m_buffers.resize(m_paramCount);
    m_lengths.resize(m_paramCount);

    /// "If set to 1, causes mysql_stmt_store_result() to update the metadata MYSQL_FIELD->max_length value."
    MySQLBool bool_tmp = MySQLBool(1);
    mysql_stmt_attr_set(stmt, STMT_ATTR_UPDATE_MAX_LENGTH, &bool_tmp);
}

MySQLStatement::~MySQLStatement()
{
    ClearParameters();
    mysql_stmt_close(m_Mstmt);
}

void MySQLStatement::BindParameters(std::span<PreparedStatementData const> params)
{
    uint8 pos = 0;
    for (PreparedStatementData const& data : params)
    {
        std::visit([&](auto&& param)
        {
            SetParameter(pos, param);
        }, data.data);

        ++pos;
    }

#ifdef _DEBUG
    if (pos < m_paramCount)
        LOG_WARN("sql.sql", "[WARNING]: BindParameters() for statement \"{}\" did not bind all allocated parameters", m_queryString);
#endif
}

void MySQLStatement::ClearParameters()
{
    for (uint32 i = 0; i < m_paramCount; ++i)
    {
        m_bind[i].buffer = nullptr;
        m_bind[i].length = nullptr;
        m_buffers[i].reset();
        m_paramsSet[i] = false;
    }
}

void MySQLStatement::AssertValidIndex(uint8 index)
{
    ASSERT(index < m_paramCount, "Attempted to bind parameter {} on a PreparedStatement \"{}\" (statement has only {} parameters)",
        uint32(index) + 1, m_queryString, m_paramCount);

    if (m_paramsSet[index])
        LOG_ERROR("sql.sql", "[ERROR] Prepared Statement \"{}\" trying to bind value on already bound index ({}).", m_queryString, index);
}

MYSQL_BIND* MySQLStatement::ResetParameter(uint8 index)
{
    AssertValidIndex(index);
    m_paramsSet[index] = true;
    m_buffers[index].reset();

    MYSQL_BIND* param = &m_bind[index];
    param->buffer = nullptr;
    param->buffer_length = 0;
    param->is_null_value = 0;
    param->length = nullptr;
    param->is_unsigned = false;
    return param;
}

template<typename T>
void MySQLStatement::SetParameter(uint8 index, T value)
{
    MYSQL_BIND* param = ResetParameter(index);
    m_buffers[index] = std::make_unique<char[]>(sizeof(T));
    memcpy(m_buffers[index].get(), &value, sizeof(T));

    param->buffer_type = MySQLType<T>::value;
    param->buffer = m_buffers[index].get();
    param->is_unsigned = std::is_unsigned_v<T>;
}

void MySQLStatement::SetParameter(uint8 index, bool value)
{
    SetParameter(index, uint8(value ? 1 : 0));
}

void MySQLStatement::SetParameter(uint8 index, std::nullptr_t /*value*/)
{
    MYSQL_BIND* param = ResetParameter(index);
    param->buffer_type = MYSQL_TYPE_NULL;
    param->is_null_value = 1;
}

void MySQLStatement::SetParameter(uint8 index, std::string const& value)
{
    MYSQL_BIND* param = ResetParameter(index);
    m_buffers[index] = std::make_unique<char[]>(value.size());
    memcpy(m_buffers[index].get(), value.data(), value.size());
    m_lengths[index] = static_cast<unsigned long>(value.size());

    param->buffer_type = MYSQL_TYPE_VAR_STRING;
    param->buffer = m_buffers[index].get();
    param->buffer_length = m_lengths[index];
    param->length = &m_lengths[index];
}

void MySQLStatement::SetParameter(uint8 index, std::vector<uint8> const& value)
{
    MYSQL_BIND* param = ResetParameter(index);
    m_buffers[index] = std::make_unique<char[]>(value.size());
    if (!value.empty())
        memcpy(m_buffers[index].get(), value.data(), value.size());
    m_lengths[index] = static_cast<unsigned long>(value.size());

    param->buffer_type = MYSQL_TYPE_BLOB;
    param->buffer = m_buffers[index].get();
    param->buffer_length = m_lengths[index];
    param->length = &m_lengths[index];
}

namespace
{
    constexpr unsigned int BINARY_CHARSET = 63;

    DatabaseFieldTypes MysqlTypeToFieldType(enum_field_types type)
    {
        switch (type)
        {
            case MYSQL_TYPE_NULL:
                return DatabaseFieldTypes::Null;
            case MYSQL_TYPE_TINY:
                return DatabaseFieldTypes::Int8;
            case MYSQL_TYPE_YEAR:
            case MYSQL_TYPE_SHORT:
                return DatabaseFieldTypes::Int16;
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONG:
                return DatabaseFieldTypes::Int32;
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_BIT:
                return DatabaseFieldTypes::Int64;
            case MYSQL_TYPE_FLOAT:
                return DatabaseFieldTypes::Float;
            case MYSQL_TYPE_DOUBLE:
                return DatabaseFieldTypes::Double;
            case MYSQL_TYPE_DECIMAL:
            case MYSQL_TYPE_NEWDECIMAL:
                return DatabaseFieldTypes::Decimal;
            case MYSQL_TYPE_TIMESTAMP:
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_TIME:
            case MYSQL_TYPE_DATETIME:
                return DatabaseFieldTypes::Date;
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_VAR_STRING:
                return DatabaseFieldTypes::Binary;
            default:
                LOG_WARN("sql.sql", "MysqlTypeToFieldType(): invalid field type {}", uint32(type));
                break;
        }

        return DatabaseFieldTypes::Null;
    }

    std::string FieldTypeToString(enum_field_types type)
    {
        switch (type)
        {
            case MYSQL_TYPE_BIT:         return "BIT";
            case MYSQL_TYPE_BLOB:        return "BLOB";
            case MYSQL_TYPE_DATE:        return "DATE";
            case MYSQL_TYPE_DATETIME:    return "DATETIME";
            case MYSQL_TYPE_NEWDECIMAL:  return "NEWDECIMAL";
            case MYSQL_TYPE_DECIMAL:     return "DECIMAL";
            case MYSQL_TYPE_DOUBLE:      return "DOUBLE";
            case MYSQL_TYPE_ENUM:        return "ENUM";
            case MYSQL_TYPE_FLOAT:       return "FLOAT";
            case MYSQL_TYPE_GEOMETRY:    return "GEOMETRY";
            case MYSQL_TYPE_INT24:       return "INT24";
            case MYSQL_TYPE_LONG:        return "LONG";
            case MYSQL_TYPE_LONGLONG:    return "LONGLONG";
            case MYSQL_TYPE_LONG_BLOB:   return "LONG_BLOB";
            case MYSQL_TYPE_MEDIUM_BLOB: return "MEDIUM_BLOB";
            case MYSQL_TYPE_NEWDATE:     return "NEWDATE";
            case MYSQL_TYPE_NULL:        return "NULL";
            case MYSQL_TYPE_SET:         return "SET";
            case MYSQL_TYPE_SHORT:       return "SHORT";
            case MYSQL_TYPE_STRING:      return "STRING";
            case MYSQL_TYPE_TIME:        return "TIME";
            case MYSQL_TYPE_TIMESTAMP:   return "TIMESTAMP";
            case MYSQL_TYPE_TINY:        return "TINY";
            case MYSQL_TYPE_TINY_BLOB:   return "TINY_BLOB";
            case MYSQL_TYPE_VAR_STRING:  return "VAR_STRING";
            case MYSQL_TYPE_YEAR:        return "YEAR";
            default:                     return "-Unknown-";
        }
    }

    std::vector<QueryResultFieldMetadata> GetColumns(MYSQL_FIELD const* fields, uint32 fieldCount)
    {
        std::vector<QueryResultFieldMetadata> columns(fieldCount);
        for (uint32 i = 0; i < fieldCount; ++i)
        {
            QueryResultFieldMetadata& meta = columns[i];
            meta.TableName = fields[i].org_table;
            meta.TableAlias = fields[i].table;
            meta.Name = fields[i].org_name;
            meta.Alias = fields[i].name;
            meta.TypeName = FieldTypeToString(fields[i].type);
            meta.Index = i;
            meta.Type = MysqlTypeToFieldType(fields[i].type);
        }

        return columns;
    }

    bool IsIntegerType(enum_field_types type)
    {
        switch (type)
        {
            case MYSQL_TYPE_TINY:
            case MYSQL_TYPE_SHORT:
            case MYSQL_TYPE_INT24:
            case MYSQL_TYPE_LONG:
            case MYSQL_TYPE_LONGLONG:
            case MYSQL_TYPE_YEAR:
                return true;
            default:
                return false;
        }
    }

    bool IsTimeType(enum_field_types type)
    {
        switch (type)
        {
            case MYSQL_TYPE_TIMESTAMP:
            case MYSQL_TYPE_DATE:
            case MYSQL_TYPE_TIME:
            case MYSQL_TYPE_DATETIME:
                return true;
            default:
                return false;
        }
    }

    bool IsBinaryField(MYSQL_FIELD const& field)
    {
        if (field.charsetnr != BINARY_CHARSET)
            return false;

        switch (field.type)
        {
            case MYSQL_TYPE_TINY_BLOB:
            case MYSQL_TYPE_MEDIUM_BLOB:
            case MYSQL_TYPE_LONG_BLOB:
            case MYSQL_TYPE_BLOB:
            case MYSQL_TYPE_STRING:
            case MYSQL_TYPE_VAR_STRING:
                return true;
            default:
                return false;
        }
    }

    int64 BitValue(char const* data, unsigned long length)
    {
        uint64 value = 0;
        for (unsigned long i = 0; i < length; ++i)
            value = (value << 8) | uint8(data[i]);

        return int64(value);
    }

    std::string FormatTime(MYSQL_TIME const& time, enum_field_types type)
    {
        if (type == MYSQL_TYPE_DATE)
            return Acore::StringFormat("{:04}-{:02}-{:02}", time.year, time.month, time.day);

        if (type == MYSQL_TYPE_TIME)
            return Acore::StringFormat("{}{:02}:{:02}:{:02}", time.neg ? "-" : "", time.day * 24 + time.hour, time.minute, time.second);

        return Acore::StringFormat("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", time.year, time.month, time.day, time.hour, time.minute, time.second);
    }

    void SetTextCell(RowSet& rows, FieldValue& cell, MYSQL_FIELD const& field, char const* data, unsigned long length)
    {
        std::string_view text(data, length);

        if (IsIntegerType(field.type))
        {
            if (field.flags & UNSIGNED_FLAG)
            {
                if (Optional<uint64> value = Acore::StringTo<uint64>(text))
                {
                    cell.SetInt(int64(*value));
                    return;
                }
            }
            else if (Optional<int64> value = Acore::StringTo<int64>(text))
            {
                cell.SetInt(*value);
                return;
            }
        }
        else if (field.type == MYSQL_TYPE_FLOAT || field.type == MYSQL_TYPE_DOUBLE)
        {
            if (Optional<double> value = Acore::StringTo<double>(text))
            {
                cell.SetReal(*value);
                return;
            }
        }
        else if (field.type == MYSQL_TYPE_BIT)
        {
            cell.SetInt(BitValue(data, length));
            return;
        }

        if (IsBinaryField(field))
            rows.SetBlob(cell, data, length);
        else
            rows.SetText(cell, text);
    }
}

std::unique_ptr<RowSet> MySQLFetchResult(MYSQL_RES* result)
{
    uint32 const fieldCount = mysql_num_fields(result);
    MYSQL_FIELD const* fields = mysql_fetch_fields(result);

    auto rows = std::make_unique<RowSet>(GetColumns(fields, fieldCount));
    rows->ReserveRows(mysql_num_rows(result));

    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        unsigned long const* lengths = mysql_fetch_lengths(result);
        FieldValue* cells = rows->AppendRow();

        for (uint32 i = 0; i < fieldCount; ++i)
            if (row[i])
                SetTextCell(*rows, cells[i], fields[i], row[i], lengths[i]);
    }

    return rows;
}

std::unique_ptr<RowSet> MySQLStatement::FetchResult(DbError& err)
{
    MYSQL_STMT* stmt = m_Mstmt;
    MYSQL_RES* metadata = mysql_stmt_result_metadata(stmt);
    if (!metadata)
    {
        if (mysql_stmt_errno(stmt))
        {
            err = MakeMySQLError(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
            return nullptr;
        }

        return std::make_unique<RowSet>(std::vector<QueryResultFieldMetadata>());
    }

    //- This is where we store the (entire) resultset
    if (mysql_stmt_store_result(stmt))
    {
        err = MakeMySQLError(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
        mysql_free_result(metadata);
        return nullptr;
    }

    uint32 const fieldCount = mysql_num_fields(metadata);
    MYSQL_FIELD const* fields = mysql_fetch_fields(metadata);

    auto rows = std::make_unique<RowSet>(GetColumns(fields, fieldCount));

    std::vector<MYSQL_BIND> binds(fieldCount);
    std::vector<std::unique_ptr<char[]>> buffers(fieldCount);
    std::vector<unsigned long> lengths(fieldCount);
    std::unique_ptr<MySQLBool[]> isNull = std::make_unique<MySQLBool[]>(fieldCount);

    //- This is where we prepare the buffer based on metadata
    for (uint32 i = 0; i < fieldCount; ++i)
    {
        MYSQL_FIELD const& field = fields[i];
        MYSQL_BIND& bind = binds[i];
        unsigned long size;

        if (IsIntegerType(field.type))
        {
            bind.buffer_type = MYSQL_TYPE_LONGLONG;
            bind.is_unsigned = (field.flags & UNSIGNED_FLAG) != 0;
            size = sizeof(int64);
        }
        else if (field.type == MYSQL_TYPE_FLOAT)
        {
            bind.buffer_type = MYSQL_TYPE_FLOAT;
            size = sizeof(float);
        }
        else if (field.type == MYSQL_TYPE_DOUBLE)
        {
            bind.buffer_type = MYSQL_TYPE_DOUBLE;
            size = sizeof(double);
        }
        else if (IsTimeType(field.type))
        {
            bind.buffer_type = field.type;
            size = sizeof(MYSQL_TIME);
        }
        else
        {
            bind.buffer_type = MYSQL_TYPE_BLOB;
            size = std::max<unsigned long>(field.max_length + 1, 66);
        }

        buffers[i] = std::make_unique<char[]>(size);
        bind.buffer = buffers[i].get();
        bind.buffer_length = size;
        bind.length = &lengths[i];
        bind.is_null = &isNull[i];
    }

    //- This is where we bind the bind the buffer to the statement
    if (mysql_stmt_bind_result(stmt, binds.data()))
    {
        err = MakeMySQLError(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
        mysql_stmt_free_result(stmt);
        mysql_free_result(metadata);
        return nullptr;
    }

    rows->ReserveRows(mysql_stmt_num_rows(stmt));

    for (;;)
    {
        int status = mysql_stmt_fetch(stmt);
        if (status == MYSQL_NO_DATA)
            break;

        if (status != 0 && status != MYSQL_DATA_TRUNCATED)
        {
            err = MakeMySQLError(mysql_stmt_errno(stmt), mysql_stmt_error(stmt));
            rows.reset();
            break;
        }

        FieldValue* cells = rows->AppendRow();

        for (uint32 i = 0; i < fieldCount; ++i)
        {
            if (isNull[i])
                continue;

            FieldValue& cell = cells[i];
            char const* data = buffers[i].get();

            switch (binds[i].buffer_type)
            {
                case MYSQL_TYPE_LONGLONG:
                {
                    int64 value;
                    memcpy(&value, data, sizeof(value));
                    cell.SetInt(value);
                    break;
                }
                case MYSQL_TYPE_FLOAT:
                {
                    float value;
                    memcpy(&value, data, sizeof(value));
                    cell.SetReal(value);
                    break;
                }
                case MYSQL_TYPE_DOUBLE:
                {
                    double value;
                    memcpy(&value, data, sizeof(value));
                    cell.SetReal(value);
                    break;
                }
                case MYSQL_TYPE_BLOB:
                {
                    unsigned long length = std::min(lengths[i], binds[i].buffer_length);
                    if (fields[i].type == MYSQL_TYPE_BIT)
                        cell.SetInt(BitValue(data, length));
                    else if (IsBinaryField(fields[i]))
                        rows->SetBlob(cell, data, length);
                    else
                        rows->SetText(cell, std::string_view(data, length));
                    break;
                }
                default:
                {
                    MYSQL_TIME time;
                    memcpy(&time, data, sizeof(time));
                    rows->SetText(cell, FormatTime(time, fields[i].type));
                    break;
                }
            }
        }
    }

    m_resultBind.swap(binds);
    m_resultBuffers.swap(buffers);
    m_resultLengths.swap(lengths);
    m_resultIsNull.swap(isNull);

    /// All data is buffered, let go of mysql c api structures
    mysql_stmt_free_result(stmt);
    mysql_free_result(metadata);
    return rows;
}
