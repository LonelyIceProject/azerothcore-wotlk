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

#include "DatabaseConnectionInfo.h"
#include "Log.h"
#include "Tokenize.h"
#include "Util.h"
#include <filesystem>

namespace
{
    bool ParseServerTokens(std::string_view infoString, DatabaseConnectionInfo& info)
    {
        std::vector<std::string_view> tokens = Acore::Tokenize(infoString, ';', true);

        if (tokens.size() != 5 && tokens.size() != 6)
            return false;

        info.host.assign(tokens.at(0));
        info.port_or_socket.assign(tokens.at(1));
        info.user.assign(tokens.at(2));
        info.password.assign(tokens.at(3));
        info.database.assign(tokens.at(4));

        if (tokens.size() == 6)
            info.ssl.assign(tokens.at(5));

        return true;
    }

    void ParseSQLite(std::string_view infoString, DatabaseConnectionInfo& info)
    {
        std::vector<std::string_view> tokens = Acore::Tokenize(infoString, ';', false);
        if (tokens.empty())
            return;

        info.path.assign(tokens.front());

        for (std::size_t i = 1; i < tokens.size(); ++i)
        {
            std::string_view option = tokens[i];
            std::size_t eq = option.find('=');
            std::string_view key = option.substr(0, eq);
            std::string_view value = eq == std::string_view::npos ? std::string_view() : option.substr(eq + 1);

            if (StringEqualI(key, "name") && !value.empty())
                info.database.assign(value);
            else if (StringEqualI(key, "attach"))
            {
                std::size_t aliasEnd = value.find('=');
                if (aliasEnd == std::string_view::npos || aliasEnd == 0 || aliasEnd + 1 == value.size())
                {
                    LOG_ERROR("sql.driver", "Invalid SQLite attach option '{}', expected attach=<alias>=<path>", option);
                    info.path.clear();
                    return;
                }

                info.attach.emplace_back(std::string(value.substr(0, aliasEnd)), std::string(value.substr(aliasEnd + 1)));
            }
            else
            {
                LOG_ERROR("sql.driver", "Unknown SQLite connection option '{}'", option);
                info.path.clear();
                return;
            }
        }

        if (info.database.empty())
            info.database = std::filesystem::path(info.path).stem().string();
    }
}

bool DatabaseConnectionInfo::IsValid() const
{
    if (backend == DatabaseBackend::SQLite)
        return !path.empty() && !database.empty();

    return !host.empty() && !database.empty();
}

DatabaseConnectionInfo DatabaseConnectionInfo::Parse(std::string_view infoString)
{
    DatabaseConnectionInfo info;

    if (StringStartsWithI(infoString, "sqlite:"))
    {
        info.backend = DatabaseBackend::SQLite;
        ParseSQLite(infoString.substr(7), info);
    }
    else if (StringStartsWithI(infoString, "pgsql:"))
    {
        info.backend = DatabaseBackend::PostgreSQL;
        ParseServerTokens(infoString.substr(6), info);
    }
    else if (StringStartsWithI(infoString, "mysql:"))
        ParseServerTokens(infoString.substr(6), info);
    else
        ParseServerTokens(infoString, info);

    return info;
}
