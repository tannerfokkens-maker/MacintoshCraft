cmake_minimum_required(VERSION 3.9)
project(bareiron C)

# Gather source files (exclude ot_glue.c - not needed for MacTCP)
file(GLOB SOURCES "src/*.c")
list(FILTER SOURCES EXCLUDE REGEX ".*ot_glue\\.c$")

# Add the executable
add_application(bareiron ${SOURCES})

# Include directories
target_include_directories(bareiron PRIVATE
    ${CMAKE_SOURCE_DIR}/include
)

# Compile definitions for 68k Mac
target_compile_definitions(bareiron PRIVATE
    MAC68K_PLATFORM
    USE_MACTCP
)

# Compiler flags for 68k
target_compile_options(bareiron PRIVATE
    -O2
    -fno-exceptions
)

# Link MacTCPHelper (installed in toolchain)
target_link_libraries(bareiron MacTCPHelper)
