# Plugins: shared libraries loaded by PluginMgr at run time (doc/Plugins.md).
#
#   AddPlugin(<target> SOURCES <files...> [LINK <libraries...>] [INCLUDES <dirs...>] [EXPORT_ALL])
#
# called from a plugin's CMakeLists.txt next to its plugin.json. Builds <target> as a shared library named
# after server.library in the manifest and lays the plugin out under AC_PLUGINS_OUTPUT_DIR/<id>/ the way the
# loader expects: plugin.json, server/<platform>/<library>, and the data, sql, conf, lua and client folders.
# INCLUDES are public include folders for plugins that link against this one; EXPORT_ALL exports every
# symbol of the library for them (Windows needs it, other platforms export by default).

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
  cmake_parse_arguments(P "EXPORT_ALL" "" "SOURCES;LINK;INCLUDES" ${ARGN})

  if (NOT BUILD_SHARED_LIBS)
    message(FATAL_ERROR "Plugin ${target} needs the core built as shared libraries (-DWITH_DYNAMIC_LINKING=ON)")
  endif()

  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" manifest)
  string(JSON id GET "${manifest}" id)
  string(JSON library GET "${manifest}" server library)

  add_library(${target} SHARED ${P_SOURCES})
  target_link_libraries(${target}
    PRIVATE
      acore-core-interface
      game
      ${P_LINK})
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  # Lets plugin headers choose dllexport / dllimport for data shared between plugin libraries.
  target_compile_definitions(${target} PRIVATE AC_PLUGIN_BUILD)
  if (P_INCLUDES)
    target_include_directories(${target} PUBLIC ${P_INCLUDES})
  endif()
  if (P_EXPORT_ALL)
    set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
  if (MSVC)
    target_compile_options(${target} PRIVATE /bigobj)
  endif()

  set(dir "${AC_PLUGINS_OUTPUT_DIR}/${id}")
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME "${library}"
    RUNTIME_OUTPUT_DIRECTORY "${dir}/server/${AC_PLUGIN_PLATFORM}"
    LIBRARY_OUTPUT_DIRECTORY "${dir}/server/${AC_PLUGIN_PLATFORM}"
    FOLDER "plugins")

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
