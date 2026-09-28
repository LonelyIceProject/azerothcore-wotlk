# Core libraries built as shared libraries (WITH_DYNAMIC_LINKING) export their symbols so the applications and
# plugin libraries can link against them. Classes and data marked AC_<NAME>_API are exported explicitly (data needs
# dllimport on the consumer side); everything else is exported by CMake's generated .def file.
function(AcoreExportLibrary target name)
  target_compile_definitions(${target} PRIVATE ACORE_API_EXPORT_${name})
  if (BUILD_SHARED_LIBS)
    set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
  endif()
endfunction()
