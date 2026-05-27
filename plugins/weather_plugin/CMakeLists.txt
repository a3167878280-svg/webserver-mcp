add_library(weather_plugin SHARED
    weather_plugin.cpp
)

target_include_directories(weather_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(weather_plugin PRIVATE
    ${CMAKE_DL_LIBS}
    stdc++fs
)

set_target_properties(weather_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
