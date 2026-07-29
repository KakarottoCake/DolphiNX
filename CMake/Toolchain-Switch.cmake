if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO must point to the devkitPro installation")
endif()

set(DOLPHIN_SWITCH ON CACHE BOOL "Build the standalone Nintendo Switch frontend" FORCE)
include("$ENV{DEVKITPRO}/cmake/Switch.cmake")

add_compile_options(
  "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
  "-fmacro-prefix-map=${CMAKE_SOURCE_DIR}=."
  # Horizon currently runs Dolphin on the Switch's Cortex-A57 CPU cluster. Keep
  # the upstream ARMv8-A feature selection, but schedule generated code for the
  # actual host CPU instead of a generic ARMv8 core.
  -mtune=cortex-a57
  -fno-ident
)
