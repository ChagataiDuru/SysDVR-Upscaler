# Finds the development libraries required by NexusStream60. Supports vcpkg's
# unofficial targets, pkg-config, FFMPEG_ROOT, and ordinary CMake search paths.
# FFmpeg::* are INTERFACE IMPORTED targets rather than ALIAS targets because
# vcpkg's FFmpeg wrapper appends Apple framework link flags to them afterwards.
find_package(unofficial-ffmpeg CONFIG QUIET)
if(TARGET unofficial::ffmpeg::avformat)
  foreach(component avformat avcodec avutil)
    add_library(FFmpeg::${component} INTERFACE IMPORTED)
    target_link_libraries(FFmpeg::${component} INTERFACE unofficial::ffmpeg::${component})
  endforeach()
  set(FFmpeg_FOUND TRUE)
  return()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_AVFORMAT QUIET IMPORTED_TARGET libavformat)
  pkg_check_modules(PC_AVCODEC QUIET IMPORTED_TARGET libavcodec)
  pkg_check_modules(PC_AVUTIL QUIET IMPORTED_TARGET libavutil)
  if(TARGET PkgConfig::PC_AVFORMAT AND TARGET PkgConfig::PC_AVCODEC AND TARGET PkgConfig::PC_AVUTIL)
    foreach(component avformat avcodec avutil)
      string(TOUPPER "${component}" pkg_component)
      add_library(FFmpeg::${component} INTERFACE IMPORTED)
      target_link_libraries(FFmpeg::${component} INTERFACE PkgConfig::PC_${pkg_component})
    endforeach()
    set(FFmpeg_FOUND TRUE)
    return()
  endif()
endif()

find_path(FFmpeg_INCLUDE_DIR libavformat/avformat.h HINTS "${FFMPEG_ROOT}" "$ENV{FFMPEG_ROOT}" PATH_SUFFIXES include)
foreach(component avformat avcodec avutil)
  find_library(FFmpeg_${component}_LIBRARY NAMES ${component} HINTS "${FFMPEG_ROOT}" "$ENV{FFMPEG_ROOT}" PATH_SUFFIXES lib bin)
endforeach()
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS FFmpeg_INCLUDE_DIR FFmpeg_avformat_LIBRARY FFmpeg_avcodec_LIBRARY FFmpeg_avutil_LIBRARY
  FAIL_MESSAGE "FFmpeg development files were not found. The ffmpeg.exe command is not enough. Install vcpkg's ffmpeg package via the supplied manifest, or set FFMPEG_ROOT to a directory containing include/libavformat/avformat.h and the avformat, avcodec, and avutil import libraries.")
if(FFmpeg_FOUND)
  foreach(component avformat avcodec avutil)
    add_library(FFmpeg::${component} UNKNOWN IMPORTED)
    set_target_properties(FFmpeg::${component} PROPERTIES
      IMPORTED_LOCATION "${FFmpeg_${component}_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${FFmpeg_INCLUDE_DIR}")
  endforeach()
endif()

