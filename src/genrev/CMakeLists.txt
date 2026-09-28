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

# Need to pass old ${AC_BINARY_DIR} as param because its different at build stage
add_custom_target(revision.h ALL
  COMMAND "${CMAKE_COMMAND}" -DBUILDDIR="${AC_BINARY_DIR}" -DAC_SOURCE_DIR="${AC_SOURCE_DIR}" -DAC_BINARY_DIR="${AC_BINARY_DIR}" -P "${AC_SOURCE_DIR}/src/cmake/genrev.cmake" "${AC_BINARY_DIR}"
  WORKING_DIRECTORY "${AC_SOURCE_DIR}")

set_target_properties(revision.h
  PROPERTIES
    FOLDER
      "server")
