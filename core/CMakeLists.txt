# zederia_core: engine, parameter registry, patch model, offline renderer. No interface code and
# no plug-in framework: it builds and tests on its own and is shared by the plug-in, the
# command-line tool and the agent server.
configure_file(include/zederia/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/zederia/Version.h @ONLY)

add_library(zederia_core STATIC
    src/params/ParamDef.cpp
    src/params/Registry.cpp
    src/params/ParamDocs.cpp
    src/patch/Patch.cpp
    src/patch/PatchIO.cpp
    src/rt/AudioThreadGuard.cpp
    src/engine/VoiceAllocator.cpp
    src/engine/Engine.cpp
    src/render/OfflineRenderer.cpp
    src/bench/Bench.cpp
)
target_include_directories(zederia_core PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(zederia_core PUBLIC nlohmann_json::nlohmann_json PRIVATE zederia_warnings)
