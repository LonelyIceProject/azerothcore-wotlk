# Plugins

A plugin is a module built as a shared library and loaded by worldserver at start, without rebuilding the
core. Plugins need the core built as shared libraries:

```
cmake -DWITH_DYNAMIC_LINKING=ON -DAC_PLUGIN_ABI=<name> ...
```

`AC_PLUGIN_ABI` names the binary interface of the build. Plugins are built against the same core headers and
libraries and with the same value; the loader refuses a library whose name or platform differs.

## Folder layout

Plugins live in `PluginsDir` (worldserver.conf, default `plugins` in the working directory), one folder each:

```
plugins/
  example.greeter/
    plugin.json
    server/
      windows-x64/greeter.dll
      linux-x64/libgreeter.so
      macos-arm64/libgreeter.dylib
    sql/world/2026_01_01_00_greeter.sql
    conf/greeter.conf.dist
```

A folder may carry builds for several platforms; the loader uses `server/<platform>/` of the running system
(`windows-x64`, `linux-x64`, `linux-arm64`, `macos-x64`, `macos-arm64`).

## plugin.json

```json
{
  "format": 1,
  "id": "example.greeter",
  "version": "1.0.0",
  "name": "Greeter",
  "core": { "abi": "azerothcore-dev" },
  "depends": { "example.base": ">=1.2.0" },
  "conflicts": [ "example.old-greeter" ],
  "server": { "library": "greeter" },
  "databases": { "world": "sql/world", "characters": "sql/characters" },
  "config": "conf/greeter.conf.dist"
}
```

| Field | Meaning |
|---|---|
| `format` | Manifest format, currently 1. Unknown formats are skipped. |
| `id` | Unique id, lowercase `[a-z0-9.-]`. |
| `version` | Semantic version. |
| `name` | Display name: a string, or an object of locale → string with an `en` entry. |
| `core.abi` | `AC_PLUGIN_ABI` the library was built with. Required with `server`. |
| `depends` | id → version range: `1.2.3`, `1.2.x`, `>=1.2 <2`, `^1.2`, `~1.2.3`, `*`. |
| `conflicts` | ids that must not be installed together with this plugin. |
| `server.library` | Base name of the library; the loader adds `.dll`, `lib*.so` or `lib*.dylib`. |
| `databases` | Update folders for `auth`, `characters` and `world`. |
| `config` | The plugin's `.conf.dist`. |

Other fields are ignored by the server and may be used by tools (launchers, package managers).

## Library

```cpp
#include "PluginApi.h"

void AddGreeterScripts();   // creates the plugin's ScriptObjects, as a module's Add*Scripts() does

AC_PLUGIN(AddGreeterScripts)
```

`AC_PLUGIN` exports `AcorePlugin_Abi`, `AcorePlugin_Platform` and `AcorePlugin_AddScripts` with C linkage.
A plugin links against the core libraries (`game` and what it pulls in) and against the libraries of the
plugins it depends on.

Inside the core's build a plugin is a folder with `plugin.json` and a `CMakeLists.txt`:

```cmake
AddPlugin(greeter SOURCES src/greeter.cpp src/plugin.cpp)
```

Pass the folders with `-DAC_PLUGIN_SOURCE_DIRS=<dir>;<dir>`. `AddPlugin` builds the library and lays the
plugin out under `bin/<config>/plugins/<id>/`.

The same sources can still be built as a static module: keep the `AC_PLUGIN` line in a file outside the
folders the module build collects (for example `plugin/plugin.cpp`).

## Loading

At start worldserver

1. reads every `plugins/*/plugin.json`, skips unknown formats, duplicate ids, conflicts and plugins whose
   dependencies are missing or out of range;
2. orders the plugins so that dependencies come first;
3. loads each library (Windows: `LoadLibraryEx`, searching the plugin's folder for the DLLs it needs;
   Linux and macOS: `dlopen` with `RTLD_NOW | RTLD_GLOBAL`) and checks ABI and platform;
4. adds the plugin's config: `configs/modules/<name>.conf` when it exists, else the `.dist` from the plugin;
5. adds the plugin's SQL folders to the database updater (state `MODULE`, same rules as module SQL);
6. registers the plugin's scripts after the static modules' scripts.

A plugin that fails any step is skipped with an error in the log, and so are the plugins depending on it.
Plugins are loaded once; there is no unloading or hot reload.

## Databases

Plugin SQL is written in the SQL the updater applies for modules and must not depend on the database
backend the server runs on. Plugin code reaches the databases only through the core's interfaces
(`DatabaseWorkerPool`, prepared statements, transactions, `ModuleDatabasePool` for its own databases).
