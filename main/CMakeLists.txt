idf_component_register(SRCS "sipkip-audio.c"
                       INCLUDE_DIRS "..")

target_compile_options(${COMPONENT_LIB} PRIVATE "-Wno-format" -DOP_FIXED_POINT)
