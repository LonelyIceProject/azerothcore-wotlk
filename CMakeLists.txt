#
# This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
#

# Require version Cmake
cmake_minimum_required(VERSION 3.16...3.22)

message(STATUS "CMake version: ${CMAKE_VERSION}")

# CMake policies (can not be handled elsewhere)
cmake_policy(SET CMP0005 NEW)

if (POLICY CMP0153)
  cmake_policy(SET CMP0153 OLD)
endif()

# Set projectname (must be done AFTER setting configurationtypes)
project(AzerothCore VERSION 3.0.0 LANGUAGES CXX C)

# Root of the AzerothCore sources and build tree. They differ from CMAKE_SOURCE_DIR / CMAKE_BINARY_DIR when another
# project adds AzerothCore with add_subdirectory() to build its own applications or plugins against it.
set(AC_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(AC_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")

# add this options before PROJECT keyword
set(CMAKE_DISABLE_SOURCE_CHANGES ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)

# Set RPATH-handing (CMake parameters)
set(CMAKE_SKIP_BUILD_RPATH 0)
set(CMAKE_BUILD_WITH_INSTALL_RPATH 0)
set(CMAKE_INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH 1)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(AC_PATH_ROOT "${AC_SOURCE_DIR}")

# set macro-directory
list(APPEND CMAKE_MODULE_PATH
        "${AC_SOURCE_DIR}/src/cmake/macros")

include(CheckCXXSourceRuns)
include(CheckIncludeFiles)
include(ConfigureScripts)
include(ConfigureModules)
include(ConfigureApplications)
include(ConfigureTools)

# some utils for cmake
include(deps/acore/cmake-utils/utils.cmake)

include(src/cmake/ac_macros.cmake)

# set default buildoptions and print them
include(conf/dist/config.cmake)

# load custom configurations for cmake if exists
if(EXISTS "${AC_SOURCE_DIR}/conf/config.cmake")
    include(conf/config.cmake)
endif()

#
# Loading dyn modules
#

# add modules and dependencies
CU_SUBDIRLIST(sub_DIRS  "${AC_SOURCE_DIR}/modules" FALSE FALSE)
FOREACH(subdir ${sub_DIRS})

    get_filename_component(MODULENAME ${subdir} NAME)

    if (";${DISABLED_AC_MODULES};" MATCHES ";${MODULENAME};")
        continue()
    endif()

    STRING(REPLACE "${AC_SOURCE_DIR}/" "" subdir_rel ${subdir})
    if(EXISTS "${subdir}/CMakeLists.txt")
        add_subdirectory("${subdir_rel}")
    endif()
ENDFOREACH()

CU_RUN_HOOK("AFTER_LOAD_CONF")

# build in Release-mode by default if not explicitly set
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo")
endif()

# turn off PCH totally if enabled (hidden setting, mainly for devs)
if( NOPCH )
    set(USE_COREPCH 0)
    set(USE_SCRIPTPCH 0)
endif()

include(ConfigureBaseTargets)
include(CheckPlatform)
include(GroupSources)
include(AutoCollect)
include(ConfigInstall)
include(AcoreExport)
include(AcorePlugin)

CU_RUN_HOOK("AFTER_LOAD_CMAKE_MODULES")

# Other database backends (MySQL, ...) come from plugins, see doc/Plugins.md
option(WITH_SQLITE "Build the SQLite database backend" 1)

find_package(PCHSupport)

if(UNIX AND WITH_PERFTOOLS)
    find_package(Gperftools)
endif()

if(NOT WITHOUT_GIT)
    find_package(Git)
endif()

# Find revision ID and hash of the sourcetree
include(src/cmake/genrev.cmake)

# print out the results before continuing
include(src/cmake/showoptions.cmake)

#
# Loading framework
#

add_subdirectory(deps)
add_subdirectory(src/common)

#
# Loading application sources
#

CU_RUN_HOOK("BEFORE_SRC_LOAD")

# add core sources
add_subdirectory(src)

# The hooks mod-playerbots needs in the core, also when the module is loaded as a plugin instead of being
# built from modules/.
option(WITH_PLAYERBOTS_HOOKS "Build the core with the hooks mod-playerbots needs" OFF)
if (WITH_PLAYERBOTS_HOOKS AND BUILD_APPLICATION_WORLDSERVER)
  target_compile_options(database PRIVATE -DMOD_PLAYERBOTS)
  target_compile_options(game-interface INTERFACE -DMOD_PLAYERBOTS)
endif()

if (BUILD_APPLICATION_WORLDSERVER)
  # add modules sources
  add_subdirectory(modules)
endif()

# Plugins built together with the core: each folder holds a plugin.json and a CMakeLists.txt calling AddPlugin().
# Without shared libraries they are built into the programs instead (doc/Plugins.md).
set(AC_PLUGIN_SOURCE_DIRS "" CACHE STRING "Plugin source folders built with the core (;-separated)")
if (BUILD_APPLICATION_WORLDSERVER OR BUILD_APPLICATION_AUTHSERVER)
  foreach(plugin_dir ${AC_PLUGIN_SOURCE_DIRS})
    get_filename_component(plugin_name "${plugin_dir}" NAME)
    add_subdirectory("${plugin_dir}" "${AC_BINARY_DIR}/plugins/${plugin_name}")
  endforeach()
endif()

CU_RUN_HOOK("AFTER_SRC_LOAD")

if (BUILD_TESTING AND BUILD_APPLICATION_WORLDSERVER)
    # we use these flags to get code coverage
    set(UNIT_TEST_CXX_FLAGS "-fprofile-arcs -ftest-coverage -fno-inline")

    # enable additional flags for GCC.
    if ( CMAKE_CXX_COMPILER_ID MATCHES GNU )
        set(UNIT_TEST_CXX_FLAGS "${UNIT_TEST_CXX_FLAGS} -fno-inline-small-functions -fno-default-inline")
    endif()

    message("Unit tests code coverage: enabling ${UNIT_TEST_CXX_FLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${UNIT_TEST_CXX_FLAGS}")

    include(src/cmake/googletest.cmake)
    fetch_googletest(
            ${PROJECT_SOURCE_DIR}/src/cmake
            ${PROJECT_BINARY_DIR}/googletest
    )

    enable_testing()
    add_subdirectory(src/test)

    add_custom_target(coverage DEPENDS coverage_command)

    add_custom_command(OUTPUT coverage_command
        # Run unit tests.
        COMMAND ctest
        # Run the graphical front-end for code coverage.
        COMMAND lcov    --directory src --capture --output-file coverage.info
        COMMAND lcov    --remove coverage.info '/usr/*' '${AC_BINARY_DIR}/googletest/*' '${CMAKE_CURRENT_SOURCE_DIR}/src/test/*' --output-file coverage.info
        COMMAND genhtml -o ${CMAKE_CURRENT_SOURCE_DIR}/coverage-report coverage.info
        WORKING_DIRECTORY "${AC_BINARY_DIR}"
        )
endif()
