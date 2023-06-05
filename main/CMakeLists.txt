idf_component_register(SRCS "sipkip-audio.c" "spp-task.c" "vfs-acceptor.c" "xmodem.c" "muxed-gpio.c" "commands.c"
                       INCLUDE_DIRS "..")

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format" "-Wno-error=unused-const-variable" -DOP_FIXED_POINT)
include("audio_identifiers.cmake")
