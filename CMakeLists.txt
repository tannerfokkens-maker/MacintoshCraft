cmake_minimum_required(VERSION 3.9)
project(bareiron C)

# Gather source files (exclude ot_glue.c - not needed with static OT linking)
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
    USE_OPENTRANSPORT
)

# Compiler flags for 68k (enable function sections for dead code elimination)
target_compile_options(bareiron PRIVATE
    -O2
    -fno-exceptions
    -ffunction-sections
)

# Link flags for dead code elimination
set_target_properties(bareiron PROPERTIES
    LINK_FLAGS "-Wl,-gc-sections"
)

# Link Open Transport libraries (order matters - dependencies after dependents)
target_link_libraries(bareiron OpenTransportApp OpenTransport OpenTptInet retrocrt Interface)
