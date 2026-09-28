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

#ifndef _PLUGIN_API_H
#define _PLUGIN_API_H

// Included by plugin libraries, which end with AC_PLUGIN(AddMyScripts). See doc/Plugins.md.

// Identifies the binary interface of this core build; set with -DAC_PLUGIN_ABI=<name> when configuring.
#ifndef AC_PLUGIN_ABI
#  define AC_PLUGIN_ABI "azerothcore-dev"
#endif

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#  define AC_PLUGIN_PLATFORM "windows-x64"
#elif defined(__linux__) && defined(__x86_64__)
#  define AC_PLUGIN_PLATFORM "linux-x64"
#elif defined(__linux__) && defined(__aarch64__)
#  define AC_PLUGIN_PLATFORM "linux-arm64"
#elif defined(__APPLE__) && defined(__aarch64__)
#  define AC_PLUGIN_PLATFORM "macos-arm64"
#elif defined(__APPLE__) && defined(__x86_64__)
#  define AC_PLUGIN_PLATFORM "macos-x64"
#else
#  define AC_PLUGIN_PLATFORM "unknown"
#endif

#if defined(_WIN32)
#  define AC_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define AC_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#define AC_PLUGIN(addScripts) \
    AC_PLUGIN_EXPORT char const* AcorePlugin_Abi() { return AC_PLUGIN_ABI; } \
    AC_PLUGIN_EXPORT char const* AcorePlugin_Platform() { return AC_PLUGIN_PLATFORM; } \
    AC_PLUGIN_EXPORT void AcorePlugin_AddScripts() { addScripts(); }

#endif
