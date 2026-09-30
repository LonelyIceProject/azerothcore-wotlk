# Plugins

A plugin is a module built as a shared library and loaded at start, without rebuilding the core: by
worldserver, and by authserver and dbimport when the plugin is made for them (a database backend, for
example). Plugins need the core built as shared libraries:

```
cmake -DWITH_DYNAMIC_LINKING=ON -DAC_PLUGIN_ABI=<name> ...
```

A core built without shared libraries takes the same plugin sources too, built into the programs (see
[Built into the programs](#built-into-the-programs)).

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
  "server": { "library": "greeter", "apps": [ "worldserver" ] },
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
| `depends` | id → version range, read as npm's semver reads it (`VersionRange.h`): comparators separated by spaces and/or commas, all of which must hold: `1.2.3`, `1.2.x`, `>=1.2 <2`, `>=1.2.0,<2.0.0`, `^1.2`, `~1.2.3`, `*`. A range that cannot be read (`||`, hyphen ranges, pre-release versions, unknown operators) skips the plugin. |
| `conflicts` | ids that must not be installed together with this plugin. |
| `server.library` | Base name of the library; the loader adds `.dll`, `lib*.so` or `lib*.dylib`. |
| `server.apps` | Programs that load the plugin: `worldserver`, `authserver`, `dbimport`. Default `["worldserver"]`. |
| `databases` | Update folders for `auth`, `characters` and `world`. |
| `config` | The plugin's `.conf.dist`. |

Other fields are ignored by the server and may be used by tools (launchers, package managers).

## Library

```cpp
#include "PluginApi.h"

void AddGreeterScripts();   // creates the plugin's ScriptObjects, as a module's Add*Scripts() does

AC_PLUGIN(AddGreeterScripts)
```

A library ends with one entry macro:

| Macro | Entry points |
|---|---|
| `AC_PLUGIN(addScripts)` | Scripts, registered by worldserver together with the modules' scripts. |
| `AC_PLUGIN_ON_LOAD(onLoad)` | Code run right after the library is loaded, in every program that loads it, before configs are read and databases open. |
| `AC_PLUGIN_ENTRY(onLoad, addScripts)` | Both. |

They export `AcorePlugin_Abi`, `AcorePlugin_Platform`, `AcorePlugin_OnLoad` and `AcorePlugin_AddScripts` with
C linkage. A plugin links against the core libraries (`game` and what it pulls in, or only `shared` for a
plugin that also loads in authserver and dbimport) and against the libraries of the plugins it depends on.

A database backend is such a plugin: its `onLoad` calls `RegisterBackendDriver` (`IDbConnectionBackend.h`)
and its manifest lists every program that opens the databases:

```cpp
void AddMyBackend()
{
    DbBackendDriver driver;
    driver.create = &CreateMyBackend;       // IDbConnectionBackend with a CreateScriptTarget for the updater
    driver.caps = { 0, true, true };
    RegisterBackendDriver(DatabaseBackend::MySQL, driver);
}

AC_PLUGIN_ON_LOAD(AddMyBackend)
```

```json
"server": { "library": "mybackend", "apps": [ "worldserver", "authserver", "dbimport" ] }
```

Inside the core's build a plugin is a folder with `plugin.json` and a `CMakeLists.txt`:

```cmake
AddPlugin(greeter SOURCES src/greeter.cpp src/plugin.cpp)
AddPlugin(mybackend SOURCES src/backend.cpp plugin/plugin.cpp CORE shared LINK <client library>
  RUNTIME_FILES <client library runtime files>)
```

Pass the folders with `-DAC_PLUGIN_SOURCE_DIRS=<dir>;<dir>`. `AddPlugin` builds the library and lays the
plugin out under `bin/<config>/plugins/<id>/`. `CORE` is the core library to build against (`game` by
default); `RUNTIME_FILES` are copied next to the library.

### Built into the programs

When the core is built without shared libraries, `AddPlugin` builds the plugin into the programs named in
`server.apps` instead and copies its `RUNTIME_FILES` next to them. The entry macros then register the entry
points under the manifest's id instead of exporting them, and the loader uses them for the plugin's folder in
`PluginsDir`, which `AddPlugin` still lays out (without a library). A built-in plugin whose folder is missing is
loaded anyway, without its configs and SQL.

## Loading

At start worldserver, authserver and dbimport (`PluginsDir` in their configs)

1. read every `plugins/*/plugin.json`, leave out plugins not made for the program (`server.apps`), skip
   unknown formats, duplicate ids, conflicts and plugins whose dependencies are missing or out of range;
2. order the plugins so that dependencies come first;
3. load each library (Windows: `LoadLibraryEx`, searching the plugin's folder for the DLLs it needs;
   Linux and macOS: `dlopen` with `RTLD_NOW | RTLD_GLOBAL`) and check ABI and platform;
4. add the plugin's config: the `.dist` from the plugin is loaded first, then `modules/<name>.conf` (the file
   name of `config` without `.dist`), when it exists, overrides it key by key. The `modules` folder is the one
   beside the main config file the program loaded (`-c`), or `modules` in the default config directory when
   there is no such folder;
5. add the plugin's SQL folders to the database updater (state `MODULE`, same rules as module SQL);
6. run the plugin's `onLoad`;
7. worldserver registers the plugin's scripts after the static modules' scripts.

A plugin that fails any step is skipped with an error in the log, and so are the plugins depending on it.
Plugins are loaded once; there is no unloading or hot reload.

## Databases

Plugin SQL is written in the SQL the updater applies for modules and must not depend on the database
backend the server runs on. Plugin code reaches the databases only through the core's interfaces
(`DatabaseWorkerPool`, prepared statements, transactions, `ModuleDatabasePool` for its own databases).
