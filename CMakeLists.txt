cmake_minimum_required(VERSION 3.29)

project(RandomTreasureDrop VERSION 0.1.40 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(RANDOM_TREASURE_DROP_LOCAL_SDK_DIR "" CACHE PATH
    "Optional local D2RLoader PluginSDK include tree for offline builds")
if(RANDOM_TREASURE_DROP_LOCAL_SDK_DIR)
    add_library(D2RLPlugin INTERFACE)
    add_library(D2RLPlugin::D2RLPlugin ALIAS D2RLPlugin)
    target_include_directories(D2RLPlugin INTERFACE
        "${RANDOM_TREASURE_DROP_LOCAL_SDK_DIR}/include")
else()
    include(FetchContent)
    set(D2RLPLUGIN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(D2RLPLUGIN_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        D2RLoaderPluginSDK
        GIT_REPOSITORY https://github.com/D2RLoader/PluginSDK.git
        GIT_TAG 6eb8f8b6192868214706bd6d528c5294f2f551b7
        GIT_PROGRESS TRUE
    )
    FetchContent_MakeAvailable(D2RLoaderPluginSDK)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/config/random-treasure-drop.toml"
    RANDOM_TREASURE_DROP_DEFAULT_CONFIG)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(src/default_config.hpp.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/default_config.hpp"
    @ONLY NEWLINE_STYLE LF)

add_library(random_treasure_drop SHARED
    src/plugin.cpp
    src/plugin.rc
)
target_link_libraries(random_treasure_drop PRIVATE D2RLPlugin::D2RLPlugin)
target_include_directories(random_treasure_drop PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(random_treasure_drop PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
)
set_target_properties(random_treasure_drop PROPERTIES
    OUTPUT_NAME "d2rl-random-treasure-drop"
    PREFIX ""
)

if(MSVC)
    target_compile_options(random_treasure_drop PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_link_options(random_treasure_drop PRIVATE /INCREMENTAL:NO /Brepro)
endif()

