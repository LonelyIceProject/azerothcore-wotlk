# Plugins: shared libraries loaded by PluginMgr at run time (doc/Plugins.md).
#
#   AddPlugin(<target> SOURCES <files...> [LINK <libraries...>])
#
# called from a plugin's CMakeLists.txt next to its plugin.json. Builds <target> as a shared library named
# after server.library in the manifest and lays the plugin out under AC_PLUGINS_OUTPUT_DIR/<id>/ the way the
# loader expects: plugin.json, server/<platform>/<library>, and the sql, conf, lua and client folders.

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

set(AC_PLUGINS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/plugins" CACHE STRING "Where built plugins are laid out")

function(AddPlugin target)
  cmake_parse_arguments(P "" "" "SOURCES;LINK" ${ARGN})

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

  set(dir "${AC_PLUGINS_OUTPUT_DIR}/${id}")
  set_target_properties(${target} PROPERTIES
    OUTPUT_NAME "${library}"
    RUNTIME_OUTPUT_DIRECTORY "${dir}/server/${AC_PLUGIN_PLATFORM}"
    LIBRARY_OUTPUT_DIRECTORY "${dir}/server/${AC_PLUGIN_PLATFORM}"
    FOLDER "plugins")

  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugin.json" "${dir}/plugin.json")
  foreach(sub sql conf lua client)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${sub}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/${sub}" "${dir}/${sub}")
    endif()
  endforeach()
  foreach(extra settings.json icon.png LICENSE README.md)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${extra}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/${extra}" "${dir}/${extra}")
    endif()
  endforeach()
endfunction()
