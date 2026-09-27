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

#include "SqliteFunctions.h"
#include "Define.h"
#include <sqlite3.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <string>
#include <string_view>

namespace
{
    struct DateTime
    {
        int64 year = 1970;
        int32 month = 1;
        int32 day = 1;
        int32 hour = 0;
        int32 minute = 0;
        int32 second = 0;
        int32 micro = 0;
    };

    int64 DaysFromCivil(int64 y, int32 m, int32 d)
    {
        y -= m <= 2;
        int64 const era = (y >= 0 ? y : y - 399) / 400;
        int64 const yoe = y - era * 400;
        int64 const doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
        int64 const doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + doe - 719468;
    }

    DateTime CivilFromEpoch(int64 epoch)
    {
        int64 days = epoch / 86400;
        int64 secs = epoch % 86400;
        if (secs < 0)
        {
            secs += 86400;
            --days;
        }

        int64 const z = days + 719468;
        int64 const era = (z >= 0 ? z : z - 146096) / 146097;
        int64 const doe = z - era * 146097;
        int64 const yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        int64 const doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int64 const mp = (5 * doy + 2) / 153;

        DateTime dt;
        dt.day = static_cast<int32>(doy - (153 * mp + 2) / 5 + 1);
        dt.month = static_cast<int32>(mp < 10 ? mp + 3 : mp - 9);
        dt.year = yoe + era * 400 + (dt.month <= 2);
        dt.hour = static_cast<int32>(secs / 3600);
        dt.minute = static_cast<int32>(secs / 60 % 60);
        dt.second = static_cast<int32>(secs % 60);
        return dt;
    }

    int64 ToEpoch(DateTime const& dt)
    {
        return DaysFromCivil(dt.year, dt.month, dt.day) * 86400 + dt.hour * 3600 + dt.minute * 60 + dt.second;
    }

    bool ReadNumber(std::string_view text, std::size_t& pos, std::size_t digits, int64& value)
    {
        value = 0;
        std::size_t read = 0;
        while (pos < text.size() && read < digits && text[pos] >= '0' && text[pos] <= '9')
        {
            value = value * 10 + (text[pos] - '0');
            ++pos;
            ++read;
        }
        return read > 0;
    }

    // 'YYYY-MM-DD[ HH:MM:SS[.ffffff]]'
    std::optional<DateTime> ParseDateTime(std::string_view text)
    {
        while (!text.empty() && text.front() == ' ')
            text.remove_prefix(1);

        DateTime dt;
        std::size_t pos = 0;
        int64 value = 0;
        if (!ReadNumber(text, pos, 4, value) || pos >= text.size() || text[pos] != '-')
            return std::nullopt;
        dt.year = value;
        ++pos;
        if (!ReadNumber(text, pos, 2, value) || pos >= text.size() || text[pos] != '-')
            return std::nullopt;
        dt.month = static_cast<int32>(value);
        ++pos;
        if (!ReadNumber(text, pos, 2, value))
            return std::nullopt;
        dt.day = static_cast<int32>(value);

        if (pos < text.size() && (text[pos] == ' ' || text[pos] == 'T'))
        {
            ++pos;
            if (!ReadNumber(text, pos, 2, value))
                return std::nullopt;
            dt.hour = static_cast<int32>(value);
            if (pos < text.size() && text[pos] == ':')
            {
                ++pos;
                if (!ReadNumber(text, pos, 2, value))
                    return std::nullopt;
                dt.minute = static_cast<int32>(value);
                if (pos < text.size() && text[pos] == ':')
                {
                    ++pos;
                    if (!ReadNumber(text, pos, 2, value))
                        return std::nullopt;
                    dt.second = static_cast<int32>(value);
                    if (pos < text.size() && text[pos] == '.')
                    {
                        ++pos;
                        std::size_t const start = pos;
                        ReadNumber(text, pos, 6, value);
                        for (std::size_t i = pos - start; i < 6; ++i)
                            value *= 10;
                        dt.micro = static_cast<int32>(value);
                    }
                }
            }
        }

        if (dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 || dt.hour > 23 || dt.minute > 59 || dt.second > 59)
            return std::nullopt;
        return dt;
    }

    int64 NowEpoch()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string_view ArgText(sqlite3_value* value)
    {
        char const* text = reinterpret_cast<char const*>(sqlite3_value_text(value));
        return text ? std::string_view(text, sqlite3_value_bytes(value)) : std::string_view();
    }

    std::optional<DateTime> ArgDateTime(sqlite3_value* value)
    {
        switch (sqlite3_value_type(value))
        {
            case SQLITE_NULL:
                return std::nullopt;
            case SQLITE_INTEGER:
            case SQLITE_FLOAT:
            {
                int64 const number = sqlite3_value_int64(value);
                if (number > 10000000000000LL)
                    return ParseDateTime(std::to_string(number / 1000000) + "-" + std::to_string(number / 10000 % 100) + "-" + std::to_string(number / 100 % 100));
                return ParseDateTime(std::to_string(number / 10000) + "-" + std::to_string(number / 100 % 100) + "-" + std::to_string(number % 100));
            }
            default:
                return ParseDateTime(ArgText(value));
        }
    }

    void ResultText(sqlite3_context* ctx, std::string const& text)
    {
        sqlite3_result_text(ctx, text.data(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
    }

    std::string FormatDateTime(DateTime const& dt)
    {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%04lld-%02d-%02d %02d:%02d:%02d", static_cast<long long>(dt.year), dt.month, dt.day, dt.hour, dt.minute, dt.second);
        return buffer;
    }

    std::string MySqlDateFormat(DateTime const& dt, std::string_view format)
    {
        static constexpr char const* MonthNames[] = { "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December" };
        static constexpr char const* DayNames[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

        int64 const days = DaysFromCivil(dt.year, dt.month, dt.day);
        int32 const weekday = static_cast<int32>(((days % 7) + 11) % 7);
        int32 const yearDay = static_cast<int32>(days - DaysFromCivil(dt.year, 1, 1));
        int32 const hour12 = dt.hour % 12 == 0 ? 12 : dt.hour % 12;

        std::string result;
        char buffer[32];
        auto put = [&](char const* fmt, auto value)
        {
            std::snprintf(buffer, sizeof(buffer), fmt, value);
            result += buffer;
        };

        for (std::size_t i = 0; i < format.size(); ++i)
        {
            if (format[i] != '%' || i + 1 >= format.size())
            {
                result += format[i];
                continue;
            }

            char const spec = format[++i];
            switch (spec)
            {
                case 'a': result += std::string_view(DayNames[weekday], 3); break;
                case 'b': result += std::string_view(MonthNames[dt.month - 1], 3); break;
                case 'c': put("%d", dt.month); break;
                case 'D':
                {
                    put("%d", dt.day);
                    int32 const mod100 = dt.day % 100;
                    result += (mod100 >= 11 && mod100 <= 13) ? "th" : dt.day % 10 == 1 ? "st" : dt.day % 10 == 2 ? "nd" : dt.day % 10 == 3 ? "rd" : "th";
                    break;
                }
                case 'd': put("%02d", dt.day); break;
                case 'e': put("%d", dt.day); break;
                case 'f': put("%06d", dt.micro); break;
                case 'H': put("%02d", dt.hour); break;
                case 'h':
                case 'I': put("%02d", hour12); break;
                case 'i': put("%02d", dt.minute); break;
                case 'j': put("%03d", yearDay + 1); break;
                case 'k': put("%d", dt.hour); break;
                case 'l': put("%d", hour12); break;
                case 'M': result += MonthNames[dt.month - 1]; break;
                case 'm': put("%02d", dt.month); break;
                case 'p': result += dt.hour < 12 ? "AM" : "PM"; break;
                case 'r':
                    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d %s", hour12, dt.minute, dt.second, dt.hour < 12 ? "AM" : "PM");
                    result += buffer;
                    break;
                case 'S':
                case 's': put("%02d", dt.second); break;
                case 'T':
                    std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", dt.hour, dt.minute, dt.second);
                    result += buffer;
                    break;
                case 'U': put("%02d", (yearDay + 7 - weekday) / 7); break;
                case 'u': put("%02d", (yearDay + 7 - (weekday + 6) % 7) / 7); break;
                case 'W': result += DayNames[weekday]; break;
                case 'w': put("%d", weekday); break;
                case 'Y': put("%04lld", static_cast<long long>(dt.year)); break;
                case 'y': put("%02lld", static_cast<long long>(dt.year % 100)); break;
                default: result += spec; break;
            }
        }
        return result;
    }

    std::size_t Utf8Length(std::string_view text)
    {
        std::size_t count = 0;
        for (unsigned char c : text)
            if ((c & 0xC0) != 0x80)
                ++count;
        return count;
    }

    // Byte offset of the n-th character (clamped to the end).
    std::size_t Utf8Offset(std::string_view text, int64 chars)
    {
        std::size_t pos = 0;
        while (pos < text.size() && chars > 0)
        {
            ++pos;
            while (pos < text.size() && (static_cast<unsigned char>(text[pos]) & 0xC0) == 0x80)
                ++pos;
            --chars;
        }
        return pos;
    }

    bool AnyNull(int argc, sqlite3_value** argv)
    {
        for (int i = 0; i < argc; ++i)
            if (sqlite3_value_type(argv[i]) == SQLITE_NULL)
                return true;
        return false;
    }

    void UnixTimestamp(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        if (argc == 0)
        {
            sqlite3_result_int64(ctx, NowEpoch());
            return;
        }

        switch (sqlite3_value_type(argv[0]))
        {
            case SQLITE_NULL:
                sqlite3_result_null(ctx);
                return;
            case SQLITE_INTEGER:
                sqlite3_result_int64(ctx, sqlite3_value_int64(argv[0]));
                return;
            case SQLITE_FLOAT:
                sqlite3_result_double(ctx, sqlite3_value_double(argv[0]));
                return;
            default:
            {
                std::optional<DateTime> dt = ParseDateTime(ArgText(argv[0]));
                sqlite3_result_int64(ctx, dt ? std::max<int64>(ToEpoch(*dt), 0) : 0);
                return;
            }
        }
    }

    void Now(sqlite3_context* ctx, int /*argc*/, sqlite3_value** /*argv*/)
    {
        ResultText(ctx, FormatDateTime(CivilFromEpoch(NowEpoch())));
    }

    void CurDate(sqlite3_context* ctx, int /*argc*/, sqlite3_value** /*argv*/)
    {
        ResultText(ctx, FormatDateTime(CivilFromEpoch(NowEpoch())).substr(0, 10));
    }

    void FromUnixTime(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        if (AnyNull(argc, argv))
        {
            sqlite3_result_null(ctx);
            return;
        }

        DateTime const dt = CivilFromEpoch(sqlite3_value_int64(argv[0]));
        ResultText(ctx, argc > 1 ? MySqlDateFormat(dt, ArgText(argv[1])) : FormatDateTime(dt));
    }

    void DateFormat(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        std::optional<DateTime> dt = AnyNull(argc, argv) ? std::nullopt : ArgDateTime(argv[0]);
        if (!dt)
        {
            sqlite3_result_null(ctx);
            return;
        }
        ResultText(ctx, MySqlDateFormat(*dt, ArgText(argv[1])));
    }

    template <int32 Part>
    void DatePart(sqlite3_context* ctx, int /*argc*/, sqlite3_value** argv)
    {
        std::optional<DateTime> dt = ArgDateTime(argv[0]);
        if (!dt)
        {
            sqlite3_result_null(ctx);
            return;
        }

        int64 value = 0;
        switch (Part)
        {
            case 0: value = dt->year; break;
            case 1: value = dt->month; break;
            case 2: value = dt->day; break;
            case 3: value = dt->hour; break;
            case 4: value = dt->minute; break;
            default: value = dt->second; break;
        }
        sqlite3_result_int64(ctx, value);
    }

    void DateDiff(sqlite3_context* ctx, int /*argc*/, sqlite3_value** argv)
    {
        std::optional<DateTime> a = ArgDateTime(argv[0]);
        std::optional<DateTime> b = ArgDateTime(argv[1]);
        if (!a || !b)
        {
            sqlite3_result_null(ctx);
            return;
        }
        sqlite3_result_int64(ctx, DaysFromCivil(a->year, a->month, a->day) - DaysFromCivil(b->year, b->month, b->day));
    }

    void Rand(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        thread_local std::mt19937_64 engine(std::random_device{}());
        if (argc > 0 && sqlite3_value_type(argv[0]) != SQLITE_NULL)
        {
            std::mt19937_64 seeded(static_cast<uint64>(sqlite3_value_int64(argv[0])));
            sqlite3_result_double(ctx, std::uniform_real_distribution<double>(0.0, 1.0)(seeded));
            return;
        }
        sqlite3_result_double(ctx, std::uniform_real_distribution<double>(0.0, 1.0)(engine));
    }

    void FindInSet(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        if (AnyNull(argc, argv))
        {
            sqlite3_result_null(ctx);
            return;
        }

        std::string_view const needle = ArgText(argv[0]);
        std::string_view list = ArgText(argv[1]);
        if (list.empty())
        {
            sqlite3_result_int64(ctx, 0);
            return;
        }

        int64 index = 1;
        while (true)
        {
            std::size_t const comma = list.find(',');
            if (list.substr(0, comma) == needle)
            {
                sqlite3_result_int64(ctx, index);
                return;
            }
            if (comma == std::string_view::npos)
                break;
            list.remove_prefix(comma + 1);
            ++index;
        }
        sqlite3_result_int64(ctx, 0);
    }

    template <bool FromLeft>
    void Substring(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        if (AnyNull(argc, argv))
        {
            sqlite3_result_null(ctx);
            return;
        }

        std::string_view const text = ArgText(argv[0]);
        int64 const count = std::max<int64>(sqlite3_value_int64(argv[1]), 0);
        if (FromLeft)
        {
            std::size_t const end = Utf8Offset(text, count);
            sqlite3_result_text(ctx, text.data(), static_cast<int>(end), SQLITE_TRANSIENT);
        }
        else
        {
            int64 const length = static_cast<int64>(Utf8Length(text));
            std::size_t const start = Utf8Offset(text, std::max<int64>(length - count, 0));
            sqlite3_result_text(ctx, text.data() + start, static_cast<int>(text.size() - start), SQLITE_TRANSIENT);
        }
    }

    void CharLength(sqlite3_context* ctx, int /*argc*/, sqlite3_value** argv)
    {
        if (sqlite3_value_type(argv[0]) == SQLITE_NULL)
        {
            sqlite3_result_null(ctx);
            return;
        }
        sqlite3_result_int64(ctx, static_cast<int64>(Utf8Length(ArgText(argv[0]))));
    }

    template <bool Greatest>
    void Extremum(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        if (AnyNull(argc, argv))
        {
            sqlite3_result_null(ctx);
            return;
        }

        bool numeric = true;
        for (int i = 0; i < argc; ++i)
        {
            int const type = sqlite3_value_numeric_type(argv[i]);
            if (type != SQLITE_INTEGER && type != SQLITE_FLOAT)
                numeric = false;
        }

        int best = 0;
        for (int i = 1; i < argc; ++i)
        {
            int cmp = 0;
            if (numeric)
            {
                if (sqlite3_value_type(argv[i]) == SQLITE_INTEGER && sqlite3_value_type(argv[best]) == SQLITE_INTEGER)
                {
                    int64 const a = sqlite3_value_int64(argv[i]);
                    int64 const b = sqlite3_value_int64(argv[best]);
                    cmp = a < b ? -1 : a > b ? 1 : 0;
                }
                else
                {
                    double const a = sqlite3_value_double(argv[i]);
                    double const b = sqlite3_value_double(argv[best]);
                    cmp = a < b ? -1 : a > b ? 1 : 0;
                }
            }
            else
                cmp = ArgText(argv[i]).compare(ArgText(argv[best]));

            if (Greatest ? cmp > 0 : cmp < 0)
                best = i;
        }
        sqlite3_result_value(ctx, argv[best]);
    }

    void Concat(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        if (AnyNull(argc, argv))
        {
            sqlite3_result_null(ctx);
            return;
        }

        std::string result;
        for (int i = 0; i < argc; ++i)
            result += ArgText(argv[i]);
        ResultText(ctx, result);
    }

    void SubstringIndex(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        if (AnyNull(argc, argv))
        {
            sqlite3_result_null(ctx);
            return;
        }

        std::string_view const text = ArgText(argv[0]);
        std::string_view const delim = ArgText(argv[1]);
        int64 count = sqlite3_value_int64(argv[2]);
        if (delim.empty() || count == 0)
        {
            ResultText(ctx, std::string());
            return;
        }

        if (count > 0)
        {
            std::size_t pos = 0;
            while (count-- > 0)
            {
                pos = text.find(delim, pos);
                if (pos == std::string_view::npos)
                {
                    ResultText(ctx, std::string(text));
                    return;
                }
                if (count > 0)
                    pos += delim.size();
            }
            ResultText(ctx, std::string(text.substr(0, pos)));
        }
        else
        {
            std::size_t pos = text.size();
            while (count++ < 0)
            {
                if (pos < delim.size())
                {
                    ResultText(ctx, std::string(text));
                    return;
                }
                pos = text.rfind(delim, pos - delim.size());
                if (pos == std::string_view::npos)
                {
                    ResultText(ctx, std::string(text));
                    return;
                }
            }
            ResultText(ctx, std::string(text.substr(pos + delim.size())));
        }
    }

    void Locate(sqlite3_context* ctx, int argc, sqlite3_value** argv)
    {
        if (AnyNull(argc, argv))
        {
            sqlite3_result_null(ctx);
            return;
        }

        std::string_view const needle = ArgText(argv[0]);
        std::string_view const text = ArgText(argv[1]);
        int64 const from = argc > 2 ? sqlite3_value_int64(argv[2]) : 1;
        if (from < 1)
        {
            sqlite3_result_int64(ctx, 0);
            return;
        }

        std::size_t const pos = text.find(needle, Utf8Offset(text, from - 1));
        sqlite3_result_int64(ctx, pos == std::string_view::npos ? 0 : static_cast<int64>(Utf8Length(text.substr(0, pos))) + 1);
    }

    struct FunctionDef
    {
        char const* name;
        int argc;
        bool deterministic;
        void (*fn)(sqlite3_context*, int, sqlite3_value**);
    };

    constexpr FunctionDef Functions[] =
    {
        { "UNIX_TIMESTAMP",   0, false, UnixTimestamp },
        { "UNIX_TIMESTAMP",   1, true,  UnixTimestamp },
        { "NOW",              0, false, Now },
        { "SYSDATE",          0, false, Now },
        { "UTC_TIMESTAMP",    0, false, Now },
        { "CURDATE",          0, false, CurDate },
        { "UTC_DATE",         0, false, CurDate },
        { "FROM_UNIXTIME",    1, true,  FromUnixTime },
        { "FROM_UNIXTIME",    2, true,  FromUnixTime },
        { "DATE_FORMAT",      2, true,  DateFormat },
        { "YEAR",             1, true,  DatePart<0> },
        { "MONTH",            1, true,  DatePart<1> },
        { "DAY",              1, true,  DatePart<2> },
        { "DAYOFMONTH",       1, true,  DatePart<2> },
        { "HOUR",             1, true,  DatePart<3> },
        { "MINUTE",           1, true,  DatePart<4> },
        { "SECOND",           1, true,  DatePart<5> },
        { "DATEDIFF",         2, true,  DateDiff },
        { "RAND",             0, false, Rand },
        { "RAND",             1, false, Rand },
        { "FIND_IN_SET",      2, true,  FindInSet },
        { "LEFT",             2, true,  Substring<true> },
        { "RIGHT",            2, true,  Substring<false> },
        { "CHAR_LENGTH",      1, true,  CharLength },
        { "CHARACTER_LENGTH", 1, true,  CharLength },
        { "GREATEST",        -1, true,  Extremum<true> },
        { "LEAST",           -1, true,  Extremum<false> },
        { "CONCAT",          -1, true,  Concat },
        { "SUBSTRING_INDEX",  3, true,  SubstringIndex },
        { "LOCATE",           2, true,  Locate },
        { "LOCATE",           3, true,  Locate },
    };
}

int RegisterMySqlFunctions(sqlite3* db)
{
    for (FunctionDef const& def : Functions)
    {
        int const flags = SQLITE_UTF8 | (def.deterministic ? SQLITE_DETERMINISTIC : 0);
        int const rc = sqlite3_create_function_v2(db, def.name, def.argc, flags, nullptr, def.fn, nullptr, nullptr, nullptr);
        if (rc != SQLITE_OK)
            return rc;
    }
    return SQLITE_OK;
}
