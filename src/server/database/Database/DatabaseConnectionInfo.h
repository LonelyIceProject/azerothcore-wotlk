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

#ifndef _DATABASECONNECTIONINFO_H
#define _DATABASECONNECTIONINFO_H

#include "DatabaseBackend.h"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// "sqlite:db/playerbots.sqlite;attach=characters=db/characters.sqlite[;name=playerbots]"
// "pgsql:host;port;user;password;database[;ssl]"
// "host;port_or_socket;user;password;database[;ssl]" (legacy MySQL, also accepted with a "mysql:" prefix)
struct AC_DATABASE_API DatabaseConnectionInfo
{
    DatabaseConnectionInfo() = default;
    explicit DatabaseConnectionInfo(std::string_view infoString) : DatabaseConnectionInfo(Parse(infoString)) { }

    DatabaseBackend backend = DatabaseBackend::MySQL;
    std::string database;           // logical name: MySQL schema, SQLite file stem or ;name=
    std::string host;
    std::string port_or_socket;
    std::string user;
    std::string password;
    std::string ssl;
    std::string path;               // SQLite: file path as configured, relative paths resolve against the server cwd
    std::vector<std::pair<std::string, std::string>> attach;   // alias -> path of another logical database

    [[nodiscard]] bool IsValid() const;

    static DatabaseConnectionInfo Parse(std::string_view infoString);
};

#endif
