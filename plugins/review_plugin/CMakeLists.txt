add_library(review_plugin SHARED
    review_plugin.cpp
)

target_include_directories(review_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(review_plugin PRIVATE
    ${CMAKE_DL_LIBS}
    stdc++fs
)

set_target_properties(review_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
