# Finds the development libraries required by NexusStream60. Supports vcpkg's
# unofficial targets, pkg-config, FFMPEG_ROOT, and ordinary CMake search paths.
find_package(unofficial-ffmpeg CONFIG QUIET)
if(TARGET unofficial::ffmpeg::avformat)
  add_library(FFmpeg::avformat ALIAS unofficial::ffmpeg::avformat)
  add_library(FFmpeg::avcodec ALIAS unofficial::ffmpeg::avcodec)
  add_library(FFmpeg::avutil ALIAS unofficial::ffmpeg::avutil)
  set(FFmpeg_FOUND TRUE)
  return()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_AVFORMAT QUIET IMPORTED_TARGET libavformat)
  pkg_check_modules(PC_AVCODEC QUIET IMPORTED_TARGET libavcodec)
  pkg_check_modules(PC_AVUTIL QUIET IMPORTED_TARGET libavutil)
  if(TARGET PkgConfig::PC_AVFORMAT AND TARGET PkgConfig::PC_AVCODEC AND TARGET PkgConfig::PC_AVUTIL)
    add_library(FFmpeg::avformat ALIAS PkgConfig::PC_AVFORMAT)
    add_library(FFmpeg::avcodec ALIAS PkgConfig::PC_AVCODEC)
    add_library(FFmpeg::avutil ALIAS PkgConfig::PC_AVUTIL)
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

