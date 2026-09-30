cmake_minimum_required(VERSION 3.20)

project(ShadowMapTool VERSION 1.0.0 LANGUAGES C CXX)

include(FetchContent)
include(CTest)

# CMake 3.20, bundled with Visual Studio 2019, cannot model CXX23 even though
# that compiler provides the C++20 features used by this foundation release.
if(CMAKE_VERSION VERSION_LESS 3.25)
    set(W3SHADOW_CXX_STANDARD 20)
    message(STATUS "CMake ${CMAKE_VERSION}: using the C++20 compatibility dialect")
else()
    set(W3SHADOW_CXX_STANDARD 23)
endif()

option(SHADOWMAPTOOL_FETCH_STORMLIB "Download the pinned StormLib dependency" ON)
option(SHADOWMAPTOOL_FETCH_CASCLIB "Download and bundle the pinned CascLib dependency" ON)

if(SHADOWMAPTOOL_FETCH_STORMLIB)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
    set(STORM_BUILD_TESTS OFF CACHE BOOL "Do not build StormLib tests" FORCE)
    set(STORM_SKIP_INSTALL ON CACHE BOOL "Do not install StormLib" FORCE)
    set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "Use StormLib's bundled dependencies" FORCE)
    if(WIN32)
        set(STORM_UNICODE ON CACHE BOOL "Use Unicode Windows paths in StormLib" FORCE)
    endif()

    FetchContent_Declare(
        StormLib
        GIT_REPOSITORY https://github.com/ladislav-zezula/StormLib.git
        # Immutable commit for the v9.40 release.
        GIT_TAG 6bb1882bd00ddbc3729cac5dac0fda81a61e5514
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(StormLib)
else()
    find_package(StormLib CONFIG REQUIRED)
endif()

if(WIN32 AND SHADOWMAPTOOL_FETCH_CASCLIB)
    set(CASC_UNICODE ON CACHE BOOL "Build Unicode CascLib" FORCE)
    set(CASC_BUILD_SHARED_LIB ON CACHE BOOL "Build CascLib DLL" FORCE)
    set(CASC_BUILD_STATIC_LIB OFF CACHE BOOL "Do not build static CascLib" FORCE)
    set(CASC_BUILD_TESTS OFF CACHE BOOL "Do not build CascLib tests" FORCE)
    FetchContent_Declare(
        CascLib
        GIT_REPOSITORY https://github.com/ladislav-zezula/CascLib.git
        GIT_TAG 3.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(CascLib)
endif()

add_library(w3shadow_core
    src/archive/MapArchive.cpp
    src/assets/AssetProvider.cpp
    src/formats/DOO.cpp
    src/formats/MDX.cpp
    src/geometry/BVH.cpp
    src/objects/ObjectDatabase.cpp
    src/formats/Png.cpp
    src/formats/W3R.cpp
    src/formats/W3E.cpp
    src/shadow/Pattern.cpp
    src/shadow/ShadowGenerator.cpp
    src/shadow/ShadowMap.cpp
    src/util/FileIO.cpp
)

target_include_directories(w3shadow_core PUBLIC src)
target_link_libraries(w3shadow_core PRIVATE StormLib::storm)
if(WIN32)
    target_compile_definitions(w3shadow_core PRIVATE UNICODE _UNICODE)
endif()
set_target_properties(w3shadow_core PROPERTIES
    CXX_STANDARD ${W3SHADOW_CXX_STANDARD}
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

add_executable(w3shadow src/main.cpp src/cli/Cli.cpp)
target_link_libraries(w3shadow PRIVATE w3shadow_core)
if(TARGET casc)
    add_dependencies(w3shadow casc)
    add_custom_command(TARGET w3shadow POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:casc> $<TARGET_FILE_DIR:w3shadow>)
endif()
set_target_properties(w3shadow PROPERTIES
    CXX_STANDARD ${W3SHADOW_CXX_STANDARD}
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)

if(MSVC)
    target_compile_options(w3shadow_core PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_options(w3shadow PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
    target_compile_options(w3shadow_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_options(w3shadow PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endif()

install(TARGETS w3shadow RUNTIME DESTINATION bin)

if(WIN32)
    add_executable(w3shadow-gui WIN32
        resources/w3shadow-gui.rc
        src/gui/App.cpp
        src/gui/WinMain.cpp
    )
    target_link_libraries(
        w3shadow-gui
        PRIVATE w3shadow_core d2d1 dwrite dwmapi ole32 shell32 comdlg32
    )
    if(TARGET casc)
        add_dependencies(w3shadow-gui casc)
        add_custom_command(TARGET w3shadow-gui POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:casc> $<TARGET_FILE_DIR:w3shadow-gui>)
    endif()
    target_compile_definitions(w3shadow-gui PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(w3shadow-gui PROPERTIES
        CXX_STANDARD ${W3SHADOW_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )
    if(MSVC)
        target_compile_options(w3shadow-gui PRIVATE /W4 /permissive- /EHsc /utf-8)
        target_link_options(w3shadow-gui PRIVATE /MANIFEST:NO)
    endif()
    install(TARGETS w3shadow-gui RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
    add_executable(w3shadow_tests tests/TestMain.cpp)
    target_link_libraries(w3shadow_tests PRIVATE w3shadow_core)
    if(TARGET casc)
        add_dependencies(w3shadow_tests casc)
        add_custom_command(TARGET w3shadow_tests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:casc> $<TARGET_FILE_DIR:w3shadow_tests>)
    endif()
    target_compile_definitions(
        w3shadow_tests
        PRIVATE
            W3SHADOW_FIXTURE_MAP="${CMAKE_CURRENT_SOURCE_DIR}/../WC3_Database/WC3_default/DefaultMap.w3m"
            W3SHADOW_DOO_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../DoodadHider/_reference/war3map.doo"
            W3SHADOW_CURRENT_MAP_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../ShadowMap_tests/Epic Quests.w3x"
    )
    set_target_properties(w3shadow_tests PROPERTIES
        CXX_STANDARD ${W3SHADOW_CXX_STANDARD}
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )

    if(MSVC)
        target_compile_options(w3shadow_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    else()
        target_compile_options(w3shadow_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()

    add_test(NAME w3shadow_unit_tests COMMAND w3shadow_tests)
    add_test(
        NAME w3shadow_inspect_default_map
        COMMAND w3shadow inspect "${CMAKE_CURRENT_SOURCE_DIR}/../WC3_Database/WC3_default/DefaultMap.w3m"
    )
    if(WIN32)
        add_test(
            NAME w3shadow_gui_smoke
            COMMAND w3shadow-gui --smoke-test
                    "${CMAKE_CURRENT_SOURCE_DIR}/../WC3_Database/WC3_default/DefaultMap.w3m"
        )
        add_test(
            NAME w3shadow_gui_current_map_smoke
            COMMAND w3shadow-gui --smoke-test
                    "${CMAKE_CURRENT_SOURCE_DIR}/../ShadowMap_tests/Epic Quests.w3x"
        )
    endif()
endif()

