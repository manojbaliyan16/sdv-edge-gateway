add_library(can_layer STATIC
    signal_decoder.cpp
    can_reader.cpp
)

target_include_directories(can_layer
    PUBLIC  ${CMAKE_SOURCE_DIR}/include   # signal_decoder.hpp, types.hpp, dlt_wrapper.hpp
)

target_link_libraries(can_layer PRIVATE
    Threads::Threads
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(can_layer PRIVATE automotive-dlt)
endif()
