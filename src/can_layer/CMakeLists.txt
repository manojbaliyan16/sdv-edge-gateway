add_library(can_layer STATIC
    can_reader.cpp
    signal_decoder.cpp
)

target_include_directories(can_layer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
