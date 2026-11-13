# User application sources and include directories.
#
# This is the only build file you are expected to edit. Add your own runner
# sources and header search paths here; the SDK assembles the rest of the
# build graph.

set(USER_APP_EXTRA_SRCS
  runner/main.cc
  runner/webos_pointer_seats.cc
)

# webos_pointer_seats.cc overrides these libwayland-client functions for the
# embedder, which only works if the runner exports them.
string(APPEND CMAKE_EXE_LINKER_FLAGS
  " -Wl,--export-dynamic-symbol=wl_proxy_add_listener"
  " -Wl,--export-dynamic-symbol=wl_proxy_marshal_flags")

set(USER_APP_EXTRA_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/runner
)
