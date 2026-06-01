add_library(command_plugin SHARED
    command_plugin.cpp
)

target_include_directories(command_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(command_plugin PRIVATE ${CMAKE_DL_LIBS} stdc++fs)

set_target_properties(command_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
