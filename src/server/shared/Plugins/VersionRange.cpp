/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "VersionRange.h"
#include <algorithm>
#include <array>
#include <string_view>
#include <vector>

namespace
{
    using Version = std::array<uint64, 3>;

    enum class Op { Less, LessEqual, Greater, GreaterEqual };

    struct Comparator
    {
        Op op;
        Version version;
    };

    constexpr uint64 MaxPart = 999999999;

    bool IsDigit(char c)
    {
        return c >= '0' && c <= '9';
    }

    bool IsSeparator(char c)
    {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ',';
    }

    // The version being checked: leading digits of the first three parts, as plugins have always been compared.
    Version ReadVersion(std::string const& s)
    {
        Version v{ 0, 0, 0 };
        std::size_t pos = 0;
        for (std::size_t part = 0; part < 3; ++part)
        {
            uint64 n = 0;
            for (; pos < s.size() && IsDigit(s[pos]); ++pos)
                n = std::min<uint64>(n * 10 + uint64(s[pos] - '0'), MaxPart);
            v[part] = n;
            pos = s.find('.', pos);
            if (pos == std::string::npos)
                break;
            ++pos;
        }
        return v;
    }

    // A version in a range: "1", "1.2" or "1.2.3", "x", "X" or "*" for a part and every part after it, an optional
    // leading "v" and "+build" metadata that is ignored. given: the number of parts that are numbers.
    bool ReadPartial(std::string s, Version& v, int& given)
    {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
            s.erase(0, 1);
        if (std::size_t const plus = s.find('+'); plus != std::string::npos)
            s.resize(plus);
        if (s.empty())
            return false;

        v = { 0, 0, 0 };
        given = 0;
        bool wildcard = false;
        std::size_t pos = 0;
        for (int part = 0;; ++part)
        {
            if (part == 3)
                return false;
            std::size_t const dot = s.find('.', pos);
            std::string const p = s.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
            if (p == "x" || p == "X" || p == "*")
                wildcard = true;
            else
            {
                if (wildcard || p.empty() || p.size() > 9 || !std::all_of(p.begin(), p.end(), IsDigit))
                    return false;
                v[part] = std::stoull(p);
                given = part + 1;
            }
            if (dot == std::string::npos)
                return true;
            pos = dot + 1;
        }
    }

    Version Bump(Version v, int part)
    {
        v[part] = std::min(v[part] + 1, MaxPart + 1);
        for (int i = part + 1; i < 3; ++i)
            v[i] = 0;
        return v;
    }

    // One term (operator and version) as plain comparators, with the meaning npm's semver gives it.
    bool AddTerm(std::string const& op, std::string const& version, std::vector<Comparator>& out)
    {
        Version v;
        int given;
        if (!ReadPartial(version, v, given))
            return false;

        Version const zero{ 0, 0, 0 };
        auto add = [&](Op o, Version const& x) { out.push_back({ o, x }); };
        auto nothing = [&]() { add(Op::Less, zero); };
        auto exact = [&]()
        {
            if (given == 3)
            {
                add(Op::GreaterEqual, v);
                add(Op::LessEqual, v);
            }
            else if (given > 0)
            {
                add(Op::GreaterEqual, v);
                add(Op::Less, Bump(v, given - 1));
            }
        };

        if (op.empty() || op == "=")
            exact();
        else if (op == ">")
        {
            if (given == 0)
                nothing();
            else if (given == 3)
                add(Op::Greater, v);
            else
                add(Op::GreaterEqual, Bump(v, given - 1));
        }
        else if (op == ">=")
        {
            if (given > 0)
                add(Op::GreaterEqual, v);
        }
        else if (op == "<")
        {
            if (given == 0)
                nothing();
            else
                add(Op::Less, v);
        }
        else if (op == "<=")
        {
            if (given == 3)
                add(Op::LessEqual, v);
            else if (given > 0)
                add(Op::Less, Bump(v, given - 1));
        }
        else if (op == "~" || op == "~>")
        {
            if (given > 0)
            {
                add(Op::GreaterEqual, v);
                add(Op::Less, Bump(v, given == 1 ? 0 : 1));
            }
        }
        else if (op == "^")
        {
            if (given > 0)
            {
                add(Op::GreaterEqual, v);
                // the first part that is not zero may not change; with fewer parts given, the last one given
                int keep = 0;
                while (keep < given - 1 && v[keep] == 0)
                    ++keep;
                add(Op::Less, Bump(v, keep));
            }
        }
        else
            return false;
        return true;
    }

    bool Parse(std::string const& range, std::vector<Comparator>& out, std::string* badTerm)
    {
        auto bad = [&](std::string const& term)
        {
            if (badTerm)
                *badTerm = term;
            return false;
        };

        std::vector<std::string> tokens;
        for (std::size_t pos = 0; pos < range.size();)
        {
            if (IsSeparator(range[pos]))
            {
                ++pos;
                continue;
            }
            std::size_t end = pos;
            while (end < range.size() && !IsSeparator(range[end]))
                ++end;
            tokens.push_back(range.substr(pos, end - pos));
            pos = end;
        }

        for (std::size_t i = 0; i < tokens.size(); ++i)
        {
            std::string term = tokens[i];
            // alternatives and hyphen ranges are not supported
            if (term.find("||") != std::string::npos || term == "-")
                return bad(term);
            std::size_t opEnd = 0;
            while (opEnd < term.size() && std::string_view("<>=^~").find(term[opEnd]) != std::string_view::npos)
                ++opEnd;
            std::string const op = term.substr(0, opEnd);
            std::string version = term.substr(opEnd);
            // ">= 1.2.0": an operator on its own takes the next token
            if (version.empty() && !op.empty() && i + 1 < tokens.size())
            {
                version = tokens[++i];
                term += " " + version;
            }
            if (!AddTerm(op, version, out))
                return bad(term);
        }
        return true;
    }

    int Compare(Version const& a, Version const& b)
    {
        for (std::size_t i = 0; i < 3; ++i)
            if (a[i] != b[i])
                return a[i] < b[i] ? -1 : 1;
        return 0;
    }
}

bool Acore::VersionRange::IsValid(std::string const& range, std::string* badTerm)
{
    std::vector<Comparator> comparators;
    return Parse(range, comparators, badTerm);
}

bool Acore::VersionRange::Satisfies(std::string const& version, std::string const& range)
{
    std::vector<Comparator> comparators;
    if (!Parse(range, comparators, nullptr))
        return false;
    Version const v = ReadVersion(version);
    return std::all_of(comparators.begin(), comparators.end(), [&](Comparator const& c)
    {
        int const r = Compare(v, c.version);
        switch (c.op)
        {
            case Op::Less:         return r < 0;
            case Op::LessEqual:    return r <= 0;
            case Op::Greater:      return r > 0;
            case Op::GreaterEqual: return r >= 0;
        }
        return false;
    });
}
