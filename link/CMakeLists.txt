# zederia_link: the local pipe protocol between Zederia instances and zederia-mcp (plan 4.2, 4.8):
# the user-only pipe, discovery files with random tokens, and the agent permissions file. Shared by
# the plug-in and the server; no plug-in framework and no engine code.
add_library(zederia_link STATIC src/Link.cpp include/zederia/link/Link.h)
target_include_directories(zederia_link PUBLIC include)
target_link_libraries(zederia_link PUBLIC nlohmann_json::nlohmann_json PRIVATE zederia_warnings)
if(WIN32)
    target_link_libraries(zederia_link PRIVATE bcrypt advapi32)
endif()
