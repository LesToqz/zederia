# zederia-mcp: the agent bridge (plan 4). The protocol layer, the tools, resources and prompts are
# a static library so the conformance tests can drive them in-process; the program adds main().
set(ZED_GUIDE_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/GuideData.cpp")
file(READ ${CMAKE_SOURCE_DIR}/docs/AGENT_GUIDE.md guide_text)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/docs/AGENT_GUIDE.md)
# Compilers limit the length of one string literal, so the guide is kept in pieces.
string(LENGTH "${guide_text}" guide_length)
set(guide_cpp "// Generated from docs/AGENT_GUIDE.md by mcp/CMakeLists.txt. Do not edit.\n#include <string>\nnamespace zmcp {\nconst std::string& agentGuide() {\n    static const std::string text = std::string()")
set(offset 0)
while(offset LESS guide_length)
    string(SUBSTRING "${guide_text}" ${offset} 6000 piece)
    string(APPEND guide_cpp "\n        + R\"zedguide(${piece})zedguide\"")
    math(EXPR offset "${offset} + 6000")
endwhile()
string(APPEND guide_cpp ";\n    return text;\n}\n} // namespace zmcp\n")
file(CONFIGURE OUTPUT ${ZED_GUIDE_SOURCE} CONTENT "${guide_cpp}" @ONLY)

add_library(zederia_mcp_lib STATIC
    src/Schema.cpp src/Schema.h
    src/Bridge.cpp src/Bridge.h
    src/Tools.h src/ToolsCore.cpp src/ToolsLibrary.cpp src/ToolsAudio.cpp src/ToolsMusic.cpp
    src/Protocol.cpp src/Protocol.h
    src/Transport.h src/Stdio.cpp src/Http.cpp
    ${ZED_GUIDE_SOURCE})
target_include_directories(zederia_mcp_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(zederia_mcp_lib PUBLIC zederia_core zederia_link zed_httplib PRIVATE zederia_warnings)
if(MSVC)
    # cpp-httplib on Windows needs a recent Windows API level.
    target_compile_definitions(zederia_mcp_lib PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(zederia_mcp_lib PRIVATE /bigobj)
endif()

add_executable(zederia-mcp main.cpp)
target_link_libraries(zederia-mcp PRIVATE zederia_mcp_lib zederia_warnings)
if(MSVC)
    target_compile_definitions(zederia-mcp PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Its icon and its version in the file.
zed_add_version_resource(zederia-mcp "Zederia agent server (MCP)" "zederia-mcp.exe" app)
