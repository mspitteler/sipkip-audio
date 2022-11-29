idf_component_register(SRCS "sipkip-audio.c" "spp-task.c" "vfs-acceptor.c" "xmodem.c"
                       INCLUDE_DIRS "..")

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format" -DOP_FIXED_POINT)
