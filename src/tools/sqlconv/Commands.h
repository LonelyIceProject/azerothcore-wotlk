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

#ifndef _SQLCONV_COMMANDS_H
#define _SQLCONV_COMMANDS_H

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;

using CommandArgs = std::vector<std::string_view>;

int LintCommand(CommandArgs const& args);
int LoadCommand(CommandArgs const& args);
int SchemaDiffCommand(CommandArgs const& args);
int VerifyCommand(CommandArgs const& args);
int SchemaSelfTestCommand(CommandArgs const& args);

// Opens a database for bulk work with the MySQL function shims; journal and sync off unless keepJournal.
sqlite3* OpenScratchDatabase(std::filesystem::path const& path, bool create, bool keepJournal, std::string& error);

#endif
