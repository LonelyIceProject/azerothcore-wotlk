# Plugins: shared libraries loaded by PluginMgr at run time (doc/Plugins.md).
#
#   AddPlugin(<target> SOURCES <files...> [CORE <library>] [LINK <libraries...>] [INCLUDES <dirs...>]
#             [RUNTIME_FILES <files...>] [EXPORT_ALL])
#
# called from a plugin's CMakeLists.txt next to its plugin.json. Builds <target> as a shared library named
# after server.library in the manifest and lays the plugin out under AC_PLUGINS_OUTPUT_DIR/<id>/ the way the
# loader expects: plugin.json, server/<platform>/<library>, and the data, sql, conf, lua and client folders.
# CORE is the core library the plugin builds against: game (default), or shared for a plugin that also loads
# in authserver and dbimport (server.apps) and must not pull the game in there. RUNTIME_FILES are libraries
# the plugin library needs at run time, copied next to it. INCLUDES are public include folders for plugins
# that link against this one; EXPORT_ALL exports every symbol of the library for them (Windows needs it,
# other platforms export by default).
#
# In a core built without shared libraries the plugin is built into the programs its manifest names in
# server.apps (worldserver when missing) instead, its RUNTIME_FILES are copied next to them, and the folder
# is laid out without the library.

if (WIN32)
  set(AC_PLUGIN_PLATFORM "windows-x64")
elseif (APPLE)
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(AC_PLUGIN_PLATFORM "macos-arm64")
  else()
    set(AC_PLUGIN_PLATFORM "macos-x64")
  endif()
else()
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(AC_PLUGIN_PLATFORM "linux-arm64")
  else()
    set(AC_PLUGIN_PLATFORM "linux-x64")
  endif()
endif()

set(AC_PLUGIN_ABI "azerothcore-dev" CACHE STRING "Name of the plugin binary interface of this build; plugins must be built with the same value")

if (CMAKE_CONFIGURATION_TYPES)
  set(AC_PLUGINS_DEFAULT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>/plugins")
else()
  set(AC_PLUGINS_DEFAULT_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/plugins")
endif()
set(AC_PLUGINS_OUTPUT_DIR "${AC_PLUGINS_DEFAULT_DIR}" CACHE STRING "Where built plugins are laid out (next to worldserver by default)")

function(AddPlugin target)
  cmake_parse_arguments(P "EXPORT_ALL" "CORE" "SOURCES;LINK;INCLUDES;RUNTIME_FILES" ${ARGN})
  if (NOT P_CORE)
    set(P_CORE game)
  endif()
  if (NOT TARGET ${P_CORE})
    message(STATUS "Plugin ${target} skipped: this build has no ${P_CORE} library")
    return()
  endif()

  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" manifest)
  string(JSON id GET "${manifest}" id)
  string(JSON library GET "${manifest}" server library)
  set(apps "")
  string(JSON app_count ERROR_VARIABLE no_apps LENGTH "${manifest}" server apps)
  if (NOT no_apps AND app_count GREATER 0)
    math(EXPR last "${app_count} - 1")
    foreach(i RANGE ${last})
      string(JSON app GET "${manifest}" server apps ${i})
      list(APPEND apps ${app})
    endforeach()
  else()
    set(apps worldserver)
  endif()

  if (BUILD_SHARED_LIBS)
    add_library(${target} SHARED ${P_SOURCES})
  else()
    # Built into the programs: PluginApi.h registers the entry points under the manifest's id instead of exporting them.
    add_library(${target} OBJECT ${P_SOURCES})
    target_compile_definitions(${target} PRIVATE AC_PLUGIN_STATIC "AC_PLUGIN_ID=\"${id}\"")
  endif()
  target_link_libraries(${target}
    PRIVATE
      acore-core-interface
      ${P_CORE}
      ${P_LINK})
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  # Lets plugin headers choose dllexport / dllimport for data shared between plugin libraries.
  target_compile_definitions(${target} PRIVATE AC_PLUGIN_BUILD)
  if (P_INCLUDES)
    target_include_directories(${target} PUBLIC ${P_INCLUDES})
  endif()
  if (P_EXPORT_ALL AND BUILD_SHARED_LIBS)
    set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
  if (MSVC)
    target_compile_options(${target} PRIVATE /bigobj)
  endif()

  set(dir "${AC_PLUGINS_OUTPUT_DIR}/${id}")
  set_target_properties(${target} PROPERTIES FOLDER "plugins")
  if (BUILD_SHARED_LIBS)
    set_target_properties(${target} PROPERTIES
      OUTPUT_NAME "${library}"
      RUNTIME_OUTPUT_DIRECTORY "${dir}/server/${AC_PLUGIN_PLATFORM}"
      LIBRARY_OUTPUT_DIRECTORY "${dir}/server/${AC_PLUGIN_PLATFORM}")
    if (P_RUNTIME_FILES)
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${P_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    endif()
  else()
    if (P_RUNTIME_FILES)
      if (CMAKE_CONFIGURATION_TYPES)
        set(bin "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>")
      else()
        set(bin "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
      endif()
      add_custom_target(${target}-runtime ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${bin}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${P_RUNTIME_FILES} "${bin}"
        VERBATIM)
      set_target_properties(${target}-runtime PROPERTIES FOLDER "plugins")
    endif()
    foreach(app ${apps})
      if (TARGET ${app})
        target_link_libraries(${app} PRIVATE ${target})
        if (P_RUNTIME_FILES)
          add_dependencies(${app} ${target}-runtime)
        endif()
      endif()
    endforeach()
  endif()

  # The plugin's other files are copied on every build, so changes to them alone reach the output as well;
  # the folders are replaced, so files removed from the sources disappear from the output too.
  set(copy COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${dir}/plugin.json")
  foreach(sub data sql conf lua client)
    list(APPEND copy COMMAND ${CMAKE_COMMAND} -E rm -rf "${dir}/${sub}")
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${sub}")
      list(APPEND copy COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/${sub}" "${dir}/${sub}")
    endif()
  endforeach()
  foreach(extra settings.json icon.png LICENSE README.md)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${extra}")
      list(APPEND copy COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${extra}" "${dir}/${extra}")
    endif()
  endforeach()
  add_custom_target(${target}-files ALL ${copy} VERBATIM)
  set_target_properties(${target}-files PROPERTIES FOLDER "plugins")
  add_dependencies(${target} ${target}-files)
endfunction()
