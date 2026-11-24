# Builds the factory folder from the content folder and runs the content check on its presets.
# The CPU readings are left out here (tests run side by side, so timings say nothing); the release
# check, scripts/content-check.ps1, runs them alone.
#   cmake -DCLI=<zederia-cli> -DOUT=<work folder> -DCONTENT=<content> -P ContentCheck.cmake
file(REMOVE_RECURSE ${OUT})
file(MAKE_DIRECTORY ${OUT})
execute_process(COMMAND ${CLI} content build --in ${CONTENT} --out ${OUT}/factory RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "content build failed (exit code ${rc}):\n${out}${err}")
endif()
execute_process(COMMAND ${CLI} content check --presets ${OUT}/factory/Presets --content ${OUT}/factory --no-cpu --threads 2
                        --report ${OUT}/content-check.json
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "content check failed (exit code ${rc}):\n${out}${err}")
endif()
if(NOT "${out}" MATCHES "preset.s.: [0-9]+ passed, 0 failed")
    message(FATAL_ERROR "content check did not report a full pass:\n${out}")
endif()
message(STATUS "${out}")
