# SPDX-License-Identifier: MIT
# MultiplayerEvolved build
#
# Two targets:
#   MultiplayerEvolved   the mod itself, loaded into the game process
#   version        a tiny proxy DLL the game loads automatically at startup
#
# The proxy exists because HaloCampaignEvolved.exe statically imports VERSION.dll.
# Dropping our own version.dll next to the executable therefore gets us loaded
# with no injector, no launcher and no administrator rights. That is what makes
# installation a single file copy, which matters more for adoption than any
# feature in the mod.
#
# Dependencies:
#   nlohmann/json   fetched automatically
#   Steamworks SDK  supplied by the builder, see MPE_STEAMWORKS_SDK below
#
# Configure and build:
#   cmake -B build -S . -DMPE_STEAMWORKS_SDK=C:/sdk/steamworks -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.24)

project(MultiplayerEvolved
        VERSION 0.1.0
        DESCRIPTION "Multiplayer and Forge for Halo: Campaign Evolved"
        LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "MultiplayerEvolved targets Windows x64 only, matching the game.")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "A 32 bit build cannot load into the 64 bit game process.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------
# Options
# --------------------------------------------------------------------------

set(MPE_STEAMWORKS_SDK "" CACHE PATH
    "Root of the Steamworks SDK, the directory containing public/steam")

option(MPE_BUILD_TESTS "Build unit tests" OFF)

# Copies build output straight into the game so iteration is build then launch.
set(MPE_GAME_DIR "" CACHE PATH
    "Game directory containing Meteorite/Binaries/Win64, for the install target")

# --------------------------------------------------------------------------
# Dependencies
# --------------------------------------------------------------------------

include(FetchContent)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

# The Steamworks SDK cannot be fetched: Valve's licence requires each developer
# to download it themselves. Fail with an actionable message rather than a
# missing header error 200 lines into the build.
if(NOT MPE_STEAMWORKS_SDK)
    message(FATAL_ERROR
        "MPE_STEAMWORKS_SDK is not set.\n"
        "Download the Steamworks SDK from https://partner.steamgames.com/downloads/list\n"
        "then configure with -DMPE_STEAMWORKS_SDK=<path to the sdk folder>")
endif()

set(MPE_STEAM_INCLUDE "${MPE_STEAMWORKS_SDK}/public")
set(MPE_STEAM_LIB     "${MPE_STEAMWORKS_SDK}/redistributable_bin/win64/steam_api64.lib")

if(NOT EXISTS "${MPE_STEAM_INCLUDE}/steam/steam_api.h")
    message(FATAL_ERROR
        "steam_api.h was not found under ${MPE_STEAM_INCLUDE}.\n"
        "MPE_STEAMWORKS_SDK should point at the folder containing 'public/steam'.")
endif()
if(NOT EXISTS "${MPE_STEAM_LIB}")
    message(FATAL_ERROR "steam_api64.lib was not found at ${MPE_STEAM_LIB}")
endif()

add_library(SteamworksSDK INTERFACE)
target_include_directories(SteamworksSDK INTERFACE "${MPE_STEAM_INCLUDE}")
target_link_libraries(SteamworksSDK INTERFACE "${MPE_STEAM_LIB}")

# --------------------------------------------------------------------------
# The mod
# --------------------------------------------------------------------------

add_library(MultiplayerEvolved SHARED
    src/ModMain.cpp

    src/Core/Result.cpp
    src/Core/Log.cpp
    src/Core/Hash.cpp
    src/Core/GameBuild.cpp

    src/Blam/ModuleImage.cpp
    src/Blam/PatternScanner.cpp
    src/Blam/SymbolRegistry.cpp

    src/Engine/IEngineControl.cpp

    src/Net/IPeerTransport.cpp
    src/Net/PacketProtocol.cpp
    src/Net/SteamSocketsTransport.cpp

    src/Lobby/SteamMatchmakingHooks.cpp
    src/Lobby/LobbyManager.cpp

    src/Map/MapVariant.cpp
    src/Map/MapVariantParser.cpp
    src/Map/MapVariantInjector.cpp
)

target_include_directories(MultiplayerEvolved PRIVATE src)

target_link_libraries(MultiplayerEvolved PRIVATE
    nlohmann_json::nlohmann_json
    SteamworksSDK
)

target_compile_definitions(MultiplayerEvolved PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX                  # Windows min/max macros break std::min and std::max
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
    MPE_VERSION_STRING="${PROJECT_VERSION}"
)

if(MSVC)
    target_compile_options(MultiplayerEvolved PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus           # report the real __cplusplus value
        /Zc:preprocessor          # conforming preprocessor, needed by __VA_ARGS__ use
        /utf-8
        /EHsc                     # exceptions contained at the json boundary only
        /MP
        $<$<CONFIG:Release>:/O2>
        $<$<CONFIG:Release>:/Zi>  # ship a PDB: a crash report without symbols is noise
    )
    # Treat warnings as errors in CI only, so a contributor is never blocked by a
    # warning in a dependency on their machine.
    if(DEFINED ENV{CI})
        target_compile_options(MultiplayerEvolved PRIVATE /WX)
    endif()

    target_link_options(MultiplayerEvolved PRIVATE
        $<$<CONFIG:Release>:/DEBUG>
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

set_target_properties(MultiplayerEvolved PROPERTIES
    OUTPUT_NAME "MultiplayerEvolved"
    PREFIX ""
)

# --------------------------------------------------------------------------
# The loader proxy
# --------------------------------------------------------------------------

add_library(version SHARED
    loader/version_proxy.cpp
)
target_include_directories(version PRIVATE src)
target_compile_definitions(version PRIVATE WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
set_target_properties(version PROPERTIES
    OUTPUT_NAME "version"
    PREFIX ""
)
if(MSVC)
    target_compile_options(version PRIVATE /W4 /permissive- /utf-8)
endif()

# --------------------------------------------------------------------------
# Install into the game
# --------------------------------------------------------------------------

if(MPE_GAME_DIR)
    set(MPE_INSTALL_DIR "${MPE_GAME_DIR}/Meteorite/Binaries/Win64")

    add_custom_target(install_to_game
        COMMAND ${CMAKE_COMMAND} -E make_directory "${MPE_INSTALL_DIR}/MultiplayerEvolved"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:MultiplayerEvolved> "${MPE_INSTALL_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:version> "${MPE_INSTALL_DIR}/"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/data" "${MPE_INSTALL_DIR}/MultiplayerEvolved"
        DEPENDS MultiplayerEvolved version
        COMMENT "Installing MultiplayerEvolved into ${MPE_INSTALL_DIR}"
        VERBATIM
    )
endif()

# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------

if(MPE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
