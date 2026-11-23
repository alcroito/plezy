# Host build of the player core against a fake StarfishMediaAPIs, for checking what is fed to LG's
# pipeline without a TV. FFMPEG_DIR: an x86_64 build from scripts/webos/build_native_player_ffmpeg.sh.
cmake_minimum_required(VERSION 3.10)
project(wnp_host_test LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)

set(FFMPEG_DIR "" CACHE PATH "FFmpeg install prefix (include/, lib/)")
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBASS REQUIRED libass)

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/../webos")
add_executable(wnp_host_test
  main.cc
  fake_starfish.cc
  "${SRC}/player.cc"
  "${SRC}/hevc_sei.cc"
  "${SRC}/av1_obu.cc"
  "${SRC}/subtitles.cc"
  "${SRC}/audio_transcode.cc"
)
target_include_directories(wnp_host_test PRIVATE "${SRC}" "${FFMPEG_DIR}/include" ${LIBASS_INCLUDE_DIRS})
target_link_directories(wnp_host_test PRIVATE "${FFMPEG_DIR}/lib" ${LIBASS_LIBRARY_DIRS})
target_link_libraries(wnp_host_test PRIVATE avformat avcodec avfilter swresample avutil ${LIBASS_LIBRARIES} pthread)
set_target_properties(wnp_host_test PROPERTIES BUILD_RPATH "${FFMPEG_DIR}/lib")
