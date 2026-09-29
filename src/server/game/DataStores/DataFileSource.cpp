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

#include "DataFileSource.h"
#include "World.h"
#include <filesystem>
#include <fstream>
#include <mutex>

namespace
{
    class DataDirSource final : public DataFileSource
    {
    public:
        std::optional<std::vector<char>> Read(std::string const& path) override
        {
            std::ifstream file(sWorld->GetDataPath() + path, std::ios::binary | std::ios::ate);
            if (!file)
                return std::nullopt;

            std::streamoff const size = file.tellg();
            if (size < 0)
                return std::nullopt;

            std::vector<char> data(static_cast<std::size_t>(size));
            file.seekg(0);
            if (!file.read(data.data(), size))
                return std::nullopt;
            return data;
        }

        bool Exists(std::string const& path) override
        {
            std::error_code ec;
            return std::filesystem::is_regular_file(sWorld->GetDataPath() + path, ec);
        }
    };

    std::mutex _lock;
    std::shared_ptr<DataFileSource> _source;

    std::shared_ptr<DataFileSource> GetSource()
    {
        {
            std::lock_guard guard(_lock);
            if (_source)
                return _source;
        }

        static std::shared_ptr<DataFileSource> const dataDir = std::make_shared<DataDirSource>();
        return dataDir;
    }
}

void DataFiles::SetSource(std::shared_ptr<DataFileSource> source)
{
    std::lock_guard guard(_lock);
    _source = std::move(source);
}

std::optional<std::vector<char>> DataFiles::Read(std::string const& path)
{
    return GetSource()->Read(path);
}

bool DataFiles::Exists(std::string const& path)
{
    return GetSource()->Exists(path);
}
