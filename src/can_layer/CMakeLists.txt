add_library(can_layer STATIC
    signal_decoder.cpp
    can_reader.cpp
    can_writer.cpp    # remote-command actuation — mirrors can_reader's socket setup
)

target_include_directories(can_layer
    PUBLIC  ${CMAKE_SOURCE_DIR}/include   # signal_decoder.hpp, types.hpp, dlt_wrapper.hpp
    PRIVATE ${DLT_INCLUDE}                # dlt/dlt.h — real DLT on Linux only, empty var on Mac
)

target_link_libraries(can_layer PRIVATE
    Threads::Threads
)

# Link real DLT on Pi only, and only if it was actually found (root CMakeLists.txt
# sets DLT_LIB via find_library against the real libdlt.so — see note there about
# why find_package(automotive-dlt) was abandoned; its exported target doesn't exist).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND DLT_LIB)
    target_link_libraries(can_layer PRIVATE ${DLT_LIB})
endif()
