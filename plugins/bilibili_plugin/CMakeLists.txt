add_library(bilibili_plugin SHARED
    bilibili_plugin.cpp
)

target_include_directories(bilibili_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(bilibili_plugin PRIVATE
    ${CMAKE_DL_LIBS}
    stdc++fs
    ssl
    crypto
)

target_compile_definitions(bilibili_plugin PRIVATE
    CPPHTTPLIB_OPENSSL_SUPPORT
)

set_target_properties(bilibili_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)
