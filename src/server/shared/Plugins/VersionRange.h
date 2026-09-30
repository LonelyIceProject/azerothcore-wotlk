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

#ifndef ACORE_VERSIONRANGE_H
#define ACORE_VERSIONRANGE_H

#include "Define.h"
#include <string>

// Version ranges of plugin dependencies (doc/Plugins.md), read the way npm's semver reads them: comparators
// separated by spaces and/or commas, all of which must hold ("^1.2", ">=1.0.0 <2.0.0", ">=1.0.0,<2.0.0", "1.2.x").
namespace Acore::VersionRange
{
    // Whether the range can be read. When it cannot, badTerm (if given) receives the part that could not.
    AC_SHARED_API bool IsValid(std::string const& range, std::string* badTerm = nullptr);

    // Whether version lies in range; false for a range that cannot be read. The version is read as the leading
    // digits of its first three dot-separated parts, missing parts counting as 0.
    AC_SHARED_API bool Satisfies(std::string const& version, std::string const& range);
}

#endif
