# zederia_core: engine, parameter registry, patch model, wavetable toolkit, offline renderer. No
# interface code and no plug-in framework: it builds and tests on its own and is shared by the
# plug-in, the command-line tool and the agent server.
configure_file(include/zederia/Version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/zederia/Version.h @ONLY)

add_library(zederia_core STATIC
    src/params/ParamDef.cpp
    src/params/Registry.cpp
    src/params/ParamDocs.cpp
    src/patch/Patch.cpp
    src/patch/PatchIO.cpp
    src/patch/Shape.cpp
    src/tuning/Tuning.cpp
    src/rt/AudioThreadGuard.cpp
    src/dsp/Fft.cpp
    src/dsp/HalfBand.cpp
    src/dsp/Simd.cpp
    src/audio/WavFile.cpp
    src/table/Wavetable.cpp
    src/table/TableData.cpp
    src/table/Toolkit.cpp
    src/table/Formula.cpp
    src/table/Import.cpp
    src/table/Resynth.cpp
    src/util/Image.cpp
    src/content/Recipe.cpp
    src/content/Library.cpp
    src/engine/VoiceAllocator.cpp
    src/engine/SampleData.cpp
    src/engine/OscKernelScalar.cpp
    src/engine/OscKernelSse2.cpp
    src/engine/OscKernelAvx2.cpp
    src/engine/Filter.cpp
    src/engine/Lfo.cpp
    src/engine/Engine.cpp
    src/fx/Effect.cpp
    src/fx/Drive.cpp
    src/fx/Dynamics.cpp
    src/fx/Eq.cpp
    src/fx/ModFx.cpp
    src/fx/Delay.cpp
    src/fx/Reverb.cpp
    src/fx/Stereo.cpp
    src/play/Player.cpp
    src/midi/MidiFile.cpp
    src/render/OfflineRenderer.cpp
    src/bench/Bench.cpp
)
target_include_directories(zederia_core PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_link_libraries(zederia_core PUBLIC nlohmann_json::nlohmann_json PRIVATE zederia_warnings)

# The eight-lane oscillator kernel is the only code compiled for AVX2; it runs only after a
# run-time check (dsp/Simd.h). The kernels must not fuse multiplies and adds, or the scalar,
# SSE2 and AVX2 paths would stop being bit-identical.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x64")
    if(MSVC)
        set_source_files_properties(src/engine/OscKernelAvx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
    else()
        set_source_files_properties(src/engine/OscKernelAvx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
    endif()
endif()
if(NOT MSVC)
    set_source_files_properties(src/engine/OscKernelScalar.cpp src/engine/OscKernelSse2.cpp src/engine/OscKernelAvx2.cpp
        PROPERTIES COMPILE_FLAGS "-ffp-contract=off")
endif()
