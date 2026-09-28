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

CollectSourceFiles(
  ${CMAKE_CURRENT_SOURCE_DIR}
  PRIVATE_SOURCES)

GroupSources(${CMAKE_CURRENT_SOURCE_DIR})

add_library(dbdialect STATIC
  ${PRIVATE_SOURCES})

target_include_directories(dbdialect
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${AC_SOURCE_DIR}/src/common
    ${AC_SOURCE_DIR}/src/server/database/Database)

target_link_libraries(dbdialect
  PRIVATE
    acore-core-interface
  PUBLIC
    sqlite3
    fmt)

set_target_properties(dbdialect
  PROPERTIES
    FOLDER
      "server")
