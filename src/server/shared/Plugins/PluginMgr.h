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

#ifndef ACORE_PLUGINMGR_H
#define ACORE_PLUGINMGR_H

#include "Define.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// Plugins loaded at run time from PluginsDir (doc/Plugins.md): server libraries, their configs and their SQL.
struct PluginInfo
{
    std::string id;
    std::string version;
    std::string name;
    std::filesystem::path dir;
    std::filesystem::path library;                       // empty: no server code
    std::string abi;                                     // core.abi the library was built for
    std::vector<std::string> apps;                       // programs that load the plugin (server.apps)
    std::string configFile;                              // file name looked up in configs/modules
    std::filesystem::path configDist;                    // fallback inside the plugin folder
    std::vector<std::pair<std::string, std::filesystem::path>> databases;   // core database -> update folder
    std::map<std::string, std::string> depends;          // id -> version range
    std::vector<std::string> conflicts;
    bool loaded = false;
    std::string error;
    void* handle = nullptr;
    void (*onLoad)() = nullptr;
    void (*addScripts)() = nullptr;
};

class AC_SHARED_API PluginMgr
{
public:
    static PluginMgr* instance();

    // Reads the manifests in dir, orders plugins by dependencies, loads the libraries of the plugins made for one of
    // apps ("worldserver", "authserver", "dbimport"; server.apps in the manifest, worldserver when missing), runs
    // their load entry points and registers their configs and SQL folders. Call after the main config and the log
    // are loaded, before ConfigMgr::LoadModulesConfigs and the databases.
    void Load(std::filesystem::path const& dir, std::vector<std::string> const& apps = { "worldserver" });

    // Registers the scripts of every loaded plugin, in load order. Call from the modules script loader.
    void AddScripts();

    std::vector<PluginInfo> const& GetPlugins() const { return _plugins; }
    PluginInfo const* Find(std::string const& id) const;
    bool IsLoaded(std::string const& id) const;

    // Semantic version check: "1.2.3" against ">=1.2 <2", "^1.2", "~1.2.3", "1.2.x", "*".
    static bool Satisfies(std::string const& version, std::string const& range);

private:
    PluginMgr() = default;
    bool ReadManifest(std::filesystem::path const& dir, PluginInfo& info);
    bool OpenLibrary(PluginInfo& info);

    std::vector<PluginInfo> _plugins;   // load order
};

#define sPluginMgr PluginMgr::instance()

#endif
