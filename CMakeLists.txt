cmake_minimum_required(VERSION 3.20)

project(MarkdownDocsMcpServer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build only the lightweight stdio/core part of mcp-cpp.
set(MCP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MCP_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MCP_ENABLE_HTTP OFF CACHE BOOL "" FORCE)
set(MCP_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)

option(MARKDOWN_DOCS_MCP_USE_SYSTEM_DEPS
       "Use system-installed dependencies instead of downloading them" OFF)
set(MCP_USE_SYSTEM_DEPS ${MARKDOWN_DOCS_MCP_USE_SYSTEM_DEPS} CACHE BOOL "" FORCE)

# Normal users do not need a separate mcp-cpp checkout: CMake downloads the
# released SDK. Contributors can point MCP_CPP_SOURCE_DIR at a local checkout.
set(MCP_CPP_SOURCE_DIR "" CACHE PATH "Optional path to a local mcp-cpp checkout")
if(MCP_CPP_SOURCE_DIR)
    if(NOT EXISTS "${MCP_CPP_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Invalid MCP_CPP_SOURCE_DIR: ${MCP_CPP_SOURCE_DIR}")
    endif()
    add_subdirectory("${MCP_CPP_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/mcp-cpp" EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(
        mcp_cpp
        GIT_REPOSITORY https://github.com/Neumann-Labs/mcp-cpp.git
        GIT_TAG v0.1.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(mcp_cpp)
endif()

add_executable(markdown_docs_mcp_server src/docs_server.cpp)
target_link_libraries(markdown_docs_mcp_server PRIVATE mcp::mcp)
target_compile_options(markdown_docs_mcp_server PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >)

install(TARGETS markdown_docs_mcp_server RUNTIME DESTINATION bin)

include(CTest)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
    add_test(
        NAME markdown_docs_mcp_server_smoke
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/docs_server_smoke_test.py"
                "$<TARGET_FILE:markdown_docs_mcp_server>"
                "${CMAKE_CURRENT_SOURCE_DIR}/sample_docs")
endif()
