add_library(file_plugin SHARED
    file_plugin.cpp
)

target_include_directories(file_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(file_plugin PRIVATE
    ${CMAKE_DL_LIBS}
    stdc++fs
)

set_target_properties(file_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
