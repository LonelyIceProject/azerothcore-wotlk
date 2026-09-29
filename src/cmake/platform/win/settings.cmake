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

# check the CMake preload parameters (commented out by default)

# overload CMAKE_INSTALL_PREFIX if not being set properly
#if( WIN32 )
#  if( NOT CYGWIN )
#    if( NOT CMAKE_INSTALL_PREFIX )
#      set(CMAKE_INSTALL_PREFIX "${AC_BINARY_DIR}/bin")
#    endif()
#  endif()
#endif()

if ( MSVC )
  include(${AC_SOURCE_DIR}/src/cmake/compiler/msvc/settings.cmake)
elseif ( MINGW )
  include(${AC_SOURCE_DIR}/src/cmake/compiler/mingw/settings.cmake)
endif()
