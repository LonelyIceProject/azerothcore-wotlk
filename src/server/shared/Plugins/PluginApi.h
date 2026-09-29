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

// Included by plugin libraries, which end with one entry macro (doc/Plugins.md):
//   AC_PLUGIN(AddMyScripts)                 scripts, registered by worldserver with the modules' scripts
//   AC_PLUGIN_ON_LOAD(OnLoad)               code run right after the library loads, in every program that loads it
//   AC_PLUGIN_ENTRY(OnLoad, AddMyScripts)   both

#include "Define.h"

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

using PluginFunction = void (*)();

// A plugin built into the programs (AddPlugin in a core built without shared libraries) instead of loaded from its
// library; PluginMgr uses these entry points for the plugin folder of the same id.
AC_SHARED_API void RegisterStaticPlugin(char const* id, PluginFunction onLoad, PluginFunction addScripts);

struct StaticPluginRegistrar
{
    StaticPluginRegistrar(char const* id, PluginFunction onLoad, PluginFunction addScripts)
    {
        RegisterStaticPlugin(id, onLoad, addScripts);
    }
};

#ifdef AC_PLUGIN_STATIC
// AddPlugin defines AC_PLUGIN_STATIC and AC_PLUGIN_ID (the manifest's id) for plugins built into the programs.
#  define AC_PLUGIN_ENTRY(onLoad, addScripts) \
    namespace { StaticPluginRegistrar const acorePluginRegistrar(AC_PLUGIN_ID, onLoad, addScripts); }
#else
#  define AC_PLUGIN_ENTRY(onLoad, addScripts) \
    AC_PLUGIN_EXPORT char const* AcorePlugin_Abi() { return AC_PLUGIN_ABI; } \
    AC_PLUGIN_EXPORT char const* AcorePlugin_Platform() { return AC_PLUGIN_PLATFORM; } \
    AC_PLUGIN_EXPORT void AcorePlugin_OnLoad() { if (PluginFunction const fn = onLoad) fn(); } \
    AC_PLUGIN_EXPORT void AcorePlugin_AddScripts() { if (PluginFunction const fn = addScripts) fn(); }
#endif

#define AC_PLUGIN(addScripts) AC_PLUGIN_ENTRY(nullptr, addScripts)
#define AC_PLUGIN_ON_LOAD(onLoad) AC_PLUGIN_ENTRY(onLoad, nullptr)

#endif
