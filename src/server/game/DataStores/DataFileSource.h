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

#ifndef _DATAFILESOURCE_H
#define _DATAFILESOURCE_H

#include "Define.h"
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Files the server reads from DataDir while it runs: terrain ("maps/0000001.map") and cinematic cameras
// ("Cameras/FlybyDwarf.m2"). Paths are relative to DataDir with '/' separators. A module can serve them from
// somewhere else (built on demand, from an archive, ...); without one they are read from DataDir.
class AC_GAME_API DataFileSource
{
public:
    virtual ~DataFileSource() = default;

    // std::nullopt when there is no such file.
    virtual std::optional<std::vector<char>> Read(std::string const& path) = 0;
    virtual bool Exists(std::string const& path) { return Read(path).has_value(); }
};

namespace DataFiles
{
    // nullptr: files in DataDir again. Set before the world loads; reads may come from several threads.
    AC_GAME_API void SetSource(std::shared_ptr<DataFileSource> source);

    AC_GAME_API std::optional<std::vector<char>> Read(std::string const& path);
    AC_GAME_API bool Exists(std::string const& path);
}

#endif
