# Warnings are errors for Zederia's own code. Dependencies are included through angle brackets
# (and SYSTEM include directories), so they stay outside this policy.
set(ZED_WARNING_FLAGS "")
if(MSVC)
    set(ZED_WARNING_FLAGS /W4 /WX /permissive- /external:anglebrackets /external:W0)
else()
    set(ZED_WARNING_FLAGS -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(zederia_warnings INTERFACE)
target_compile_options(zederia_warnings INTERFACE ${ZED_WARNING_FLAGS})

# For targets that also compile third-party sources (the plug-in compiles the framework's modules):
# apply the flags to our own files only.
function(zed_strict_sources)
    set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "${ZED_WARNING_FLAGS}")
endfunction()
