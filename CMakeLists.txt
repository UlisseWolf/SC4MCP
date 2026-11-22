# SC4MCP.dll - alternative to SC4MCP.sln. Build with Visual Studio 2022, 32-bit (SimCity 4 is a 32-bit game):
#   cmake -S . -B build -A Win32
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
project(SC4MCP LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "SimCity 4 is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(GZCOM ${CMAKE_SOURCE_DIR}/vendor/gzcom-dll/gzcom-dll)

add_library(SC4MCP SHARED
  src/HttpServer.cpp
  src/MainThreadQueue.cpp
  src/McpDllDirector.cpp
  src/McpLog.cpp
  src/McpServer.cpp
  src/TickService.cpp
  src/Tools.cpp
  src/ToolsCommon.cpp
  src/ToolsControl.cpp
  src/ToolsRead.cpp
  ${GZCOM}/src/cRZBaseString.cpp
  ${GZCOM}/src/cRZCellMap.cpp
  ${GZCOM}/src/StringResourceManager.cpp
  ${GZCOM}/src/cRZCOMDllDirector.cpp
)
target_include_directories(SC4MCP PRIVATE src vendor/nlohmann ${GZCOM}/include)
target_link_libraries(SC4MCP PRIVATE ws2_32)
target_compile_definitions(SC4MCP PRIVATE WIN32 _WINDOWS _USRDLL NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(SC4MCP PRIVATE /W4 /permissive- /utf-8)
# Static runtime: players do not need to install the Visual C++ redistributable.
set_property(TARGET SC4MCP PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
