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

add_library(sqlite3 STATIC
  sqlite3.c
  sqlite3.h
  sqlite3ext.h)

target_compile_definitions(sqlite3
  PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_ENABLE_COLUMN_METADATA
    SQLITE_ENABLE_MATH_FUNCTIONS
    SQLITE_OMIT_DEPRECATED
    SQLITE_DEFAULT_FOREIGN_KEYS=1
    SQLITE_MAX_VARIABLE_NUMBER=32766)

target_include_directories(sqlite3
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(sqlite3
  PRIVATE
    acore-dependency-interface
  PUBLIC
    threads
    ${CMAKE_DL_LIBS})

set_target_properties(sqlite3
  PROPERTIES
    FOLDER
      "deps")
