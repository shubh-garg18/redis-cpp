cmake_minimum_required(VERSION 3.16)

project(CacheMeIfYouCan VERSION 0.1.0 LANGUAGES CXX)

# ============================
# Global Configuration
# ============================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(Threads REQUIRED)

# ============================
# Compiler Warnings
# ============================
set(WARNING_FLAGS
    -Wall
    -Wextra
    -Wshadow
    -Wconversion
    -Wpedantic
)

# ============================
# Module Libraries
# ============================
add_library(protocol
    src/protocol/RESPParser.cpp
)
target_include_directories(protocol PUBLIC include)
target_compile_options(protocol PRIVATE ${WARNING_FLAGS})

add_library(storage
    src/storage/Database.cpp
    src/storage/StringStore.cpp
    src/storage/ListStore.cpp
    src/storage/SortedSetStore.cpp
    src/storage/StreamStore.cpp
)
target_include_directories(storage PUBLIC include)
target_link_libraries(storage PUBLIC Threads::Threads)
target_compile_options(storage PRIVATE ${WARNING_FLAGS})

add_library(command
    src/command/CommandDispatcher.cpp
    src/command/BasicCommands.cpp
    src/command/ListCommands.cpp
    src/command/SortedSetCommands.cpp
    src/command/StreamCommands.cpp
    src/command/GeoCommands.cpp
)
target_include_directories(command PUBLIC include)
target_link_libraries(command PUBLIC protocol storage)
target_compile_options(command PRIVATE ${WARNING_FLAGS})

add_library(server
    src/server/ClientSession.cpp
    src/server/TCPServer.cpp
    src/server/TransactionManager.cpp
)
target_include_directories(server PUBLIC include)
target_link_libraries(server PUBLIC command protocol Threads::Threads)
target_compile_options(server PRIVATE ${WARNING_FLAGS})

# ============================
# Main Server Executable
# ============================
add_executable(redis_server
    src/main.cpp
)

target_include_directories(redis_server PRIVATE include)
target_compile_options(redis_server PRIVATE ${WARNING_FLAGS})
target_link_libraries(redis_server PRIVATE protocol storage command server Threads::Threads)

# ============================
# Build Type Flags
# ============================
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_options(redis_server PRIVATE -g)
elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
    target_compile_options(redis_server PRIVATE -O3 -march=native)
endif()

# ============================
# Build Info
# ============================
message(STATUS "")
message(STATUS "========== Build Configuration ==========")
message(STATUS "Project:       ${PROJECT_NAME}")
message(STATUS "Build Type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard:  ${CMAKE_CXX_STANDARD}")
message(STATUS "=========================================")
message(STATUS "")
