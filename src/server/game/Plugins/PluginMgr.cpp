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

#include "PluginMgr.h"
#include "Config.h"
#include "PluginApi.h"
#include "Log.h"
#include "UpdateFetcher.h"
#include <algorithm>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <fkYAML/node.hpp>

#if AC_PLATFORM == AC_PLATFORM_WINDOWS
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <Windows.h>
#else
#  include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace
{
    constexpr int ManifestFormat = 1;

    std::string Text(fkyaml::node const& n)
    {
        if (n.is_string())
            return n.get_value<std::string>();
        if (n.is_mapping() && n.contains("en") && n["en"].is_string())
            return n["en"].get_value<std::string>();
        return {};
    }

    std::string Str(fkyaml::node const& root, char const* key)
    {
        return root.contains(key) && root[key].is_string() ? root[key].get_value<std::string>() : std::string();
    }

    std::vector<int> ParseVersion(std::string const& v)
    {
        std::vector<int> parts;
        std::stringstream ss(v);
        std::string part;
        while (std::getline(ss, part, '.'))
            parts.push_back(part.empty() || part == "x" || part == "*" ? -1 : std::atoi(part.c_str()));
        while (parts.size() < 3)
            parts.push_back(-1);
        return parts;
    }

    int Compare(std::vector<int> const& a, std::vector<int> const& b)
    {
        for (std::size_t i = 0; i < 3; ++i)
        {
            int x = std::max(a[i], 0), y = std::max(b[i], 0);
            if (x != y)
                return x < y ? -1 : 1;
        }
        return 0;
    }

    std::string LibraryFile(std::string const& base)
    {
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
        return base + ".dll";
#elif AC_PLATFORM == AC_PLATFORM_APPLE
        return "lib" + base + ".dylib";
#else
        return "lib" + base + ".so";
#endif
    }

    void* Symbol(void* handle, char const* name)
    {
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
        return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
        return dlsym(handle, name);
#endif
    }
}

PluginMgr* PluginMgr::instance()
{
    static PluginMgr instance;
    return &instance;
}

bool PluginMgr::Satisfies(std::string const& version, std::string const& range)
{
    std::vector<int> v = ParseVersion(version);
    std::stringstream ss(range);
    std::string term;
    while (ss >> term)
    {
        if (term == "*")
            continue;
        std::string op;
        while (!term.empty() && std::string("<>=^~").find(term[0]) != std::string::npos)
        {
            op += term[0];
            term.erase(0, 1);
        }
        std::vector<int> r = ParseVersion(term);
        int c = Compare(v, r);
        bool ok = true;
        if (op == ">=")
            ok = c >= 0;
        else if (op == ">")
            ok = c > 0;
        else if (op == "<=")
            ok = c <= 0;
        else if (op == "<")
            ok = c < 0;
        else if (op == "^")
            ok = c >= 0 && v[0] == std::max(r[0], 0) && (r[0] > 0 || v[1] == std::max(r[1], 0));
        else if (op == "~")
            ok = c >= 0 && v[0] == std::max(r[0], 0) && v[1] == std::max(r[1], 0);
        else
        {
            // "1.2.3" exact, "1.2" / "1.2.x" any patch of 1.2
            for (std::size_t i = 0; i < 3 && ok; ++i)
                if (r[i] >= 0 && v[i] != r[i])
                    ok = false;
        }
        if (!ok)
            return false;
    }
    return true;
}

bool PluginMgr::ReadManifest(fs::path const& dir, PluginInfo& info)
{
    std::ifstream in(dir / "plugin.json", std::ios::binary);
    if (!in)
        return false;

    info.dir = dir;
    try
    {
        fkyaml::node root = fkyaml::node::deserialize(in);
        int format = root.contains("format") ? root["format"].get_value<int>() : 0;
        if (format != ManifestFormat)
        {
            info.error = "unsupported manifest format " + std::to_string(format);
            return true;
        }

        info.id = Str(root, "id");
        info.version = Str(root, "version");
        info.name = root.contains("name") ? Text(root["name"]) : info.id;
        if (info.id.empty() || info.version.empty())
        {
            info.error = "manifest needs id and version";
            return true;
        }

        if (root.contains("depends") && root["depends"].is_mapping())
            for (auto const& [key, value] : root["depends"].as_map())
                info.depends[key.get_value<std::string>()] = value.is_string() ? value.get_value<std::string>() : "*";
        if (root.contains("conflicts") && root["conflicts"].is_sequence())
            for (auto const& c : root["conflicts"].as_seq())
                info.conflicts.push_back(c.get_value<std::string>());

        if (root.contains("server") && root["server"].is_mapping())
        {
            fkyaml::node const& server = root["server"];
            std::string library = Str(server, "library");
            if (!library.empty())
                info.library = dir / "server" / AC_PLUGIN_PLATFORM / LibraryFile(library);

            std::string abi = root.contains("core") && root["core"].is_mapping() ? Str(root["core"], "abi") : std::string();
            if (!info.library.empty() && abi != AC_PLUGIN_ABI)
            {
                info.error = "built for core " + (abi.empty() ? std::string("?") : abi) + ", this server is " AC_PLUGIN_ABI;
                return true;
            }
        }

        if (root.contains("databases") && root["databases"].is_mapping())
        {
            for (auto const& [key, value] : root["databases"].as_map())
            {
                std::string db = key.get_value<std::string>();
                // Plugin-owned databases (objects) are opened by the plugin itself.
                if (value.is_string() && (db == "auth" || db == "characters" || db == "world"))
                    info.databases.emplace_back(db, dir / fs::u8path(value.get_value<std::string>()));
            }
        }

        std::string config = Str(root, "config");
        if (!config.empty())
        {
            info.configDist = dir / fs::u8path(config);
            std::string file = info.configDist.filename().string();
            if (file.size() > 5 && file.ends_with(".dist"))
                file.resize(file.size() - 5);
            info.configFile = file;
        }
    }
    catch (std::exception const& e)
    {
        info.error = std::string("bad plugin.json: ") + e.what();
    }
    return true;
}

bool PluginMgr::OpenLibrary(PluginInfo& info)
{
    if (info.library.empty())
        return true;
    std::error_code ec;
    if (!fs::exists(info.library, ec))
    {
        info.error = "no build for " AC_PLUGIN_PLATFORM " (" + info.library.generic_string() + ")";
        return false;
    }

#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    // The plugin's own folder is searched for the DLLs it depends on.
    HMODULE handle = LoadLibraryExW(info.library.wstring().c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!handle)
    {
        info.error = "LoadLibrary failed, error " + std::to_string(GetLastError());
        return false;
    }
#else
    void* handle = dlopen(info.library.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!handle)
    {
        char const* err = dlerror();
        info.error = std::string("dlopen failed: ") + (err ? err : "?");
        return false;
    }
#endif
    info.handle = handle;

    auto abi = reinterpret_cast<char const* (*)()>(Symbol(handle, "AcorePlugin_Abi"));
    auto platform = reinterpret_cast<char const* (*)()>(Symbol(handle, "AcorePlugin_Platform"));
    info.addScripts = reinterpret_cast<void (*)()>(Symbol(handle, "AcorePlugin_AddScripts"));
    if (!abi || !platform || !info.addScripts)
    {
        info.error = "not a plugin library (missing exports)";
        return false;
    }
    if (std::string(abi()) != AC_PLUGIN_ABI || std::string(platform()) != AC_PLUGIN_PLATFORM)
    {
        info.error = std::string("library built for ") + abi() + " " + platform() + ", this server is " AC_PLUGIN_ABI " " AC_PLUGIN_PLATFORM;
        info.addScripts = nullptr;
        return false;
    }
    return true;
}

void PluginMgr::Load(fs::path const& dir)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return;

    std::map<std::string, PluginInfo> found;
    std::vector<fs::path> folders;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_directory(ec))
            folders.push_back(it->path());
    std::sort(folders.begin(), folders.end());

    for (fs::path const& folder : folders)
    {
        PluginInfo info;
        if (!ReadManifest(folder, info))
            continue;
        if (!info.error.empty())
        {
            LOG_ERROR("server.loading", "Plugin {}: {}", folder.filename().generic_string(), info.error);
            continue;
        }
        if (found.count(info.id))
        {
            LOG_ERROR("server.loading", "Plugin {}: id {} is already used by {}", folder.filename().generic_string(), info.id,
                found[info.id].dir.filename().generic_string());
            continue;
        }
        found.emplace(info.id, std::move(info));
    }

    // Drop plugins whose dependencies are missing or out of range, or that conflict, until nothing changes.
    for (bool changed = true; changed;)
    {
        changed = false;
        for (auto it = found.begin(); it != found.end();)
        {
            std::string why;
            for (auto const& [dep, range] : it->second.depends)
            {
                auto d = found.find(dep);
                if (d == found.end())
                    why = "needs " + dep + " " + range;
                else if (!Satisfies(d->second.version, range))
                    why = "needs " + dep + " " + range + ", installed " + d->second.version;
                if (!why.empty())
                    break;
            }
            for (std::string const& c : it->second.conflicts)
                if (why.empty() && found.count(c))
                    why = "conflicts with " + c;
            if (!why.empty())
            {
                LOG_ERROR("server.loading", "Plugin {} {} skipped: {}", it->first, it->second.version, why);
                it = found.erase(it);
                changed = true;
            }
            else
                ++it;
        }
    }

    // Dependencies first, ties by id.
    std::set<std::string> placed;
    std::function<void(std::string const&)> place = [&](std::string const& id)
    {
        if (placed.count(id))
            return;
        placed.insert(id);
        for (auto const& [dep, range] : found[id].depends)
            place(dep);
        _plugins.push_back(std::move(found[id]));
    };
    for (auto const& [id, info] : found)
        place(id);

    std::set<std::string> failed;
    for (PluginInfo& info : _plugins)
    {
        bool depFailed = std::any_of(info.depends.begin(), info.depends.end(), [&](auto const& d) { return failed.count(d.first) > 0; });
        if (depFailed)
            info.error = "a dependency failed to load";
        else if (OpenLibrary(info))
            info.loaded = true;

        if (!info.loaded)
        {
            failed.insert(info.id);
            LOG_ERROR("server.loading", "Plugin {} {} not loaded: {}", info.id, info.version, info.error);
            continue;
        }

        if (!info.configFile.empty())
            sConfigMgr->AddPluginConfig(info.configFile, info.configDist.generic_string());
        for (auto const& [db, path] : info.databases)
            UpdateFetcher::AddPluginDirectory(db, path);
        LOG_INFO("server.loading", "Plugin {} {} ({})", info.id, info.version, info.name);
    }
}

void PluginMgr::AddScripts()
{
    for (PluginInfo const& info : _plugins)
        if (info.loaded && info.addScripts)
            info.addScripts();
}

PluginInfo const* PluginMgr::Find(std::string const& id) const
{
    for (PluginInfo const& info : _plugins)
        if (info.id == id)
            return &info;
    return nullptr;
}

bool PluginMgr::IsLoaded(std::string const& id) const
{
    PluginInfo const* info = Find(id);
    return info && info->loaded;
}
