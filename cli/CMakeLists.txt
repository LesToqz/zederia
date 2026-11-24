add_executable(zederia-cli main.cpp TableCommands.cpp ContentCommands.cpp ProcessMemory.cpp UiSnapshot.cpp BrandCommands.cpp CliUtil.h ProcessMemory.h)
target_link_libraries(zederia-cli PRIVATE zederia_core zederia_warnings)
if(WIN32)
    target_link_libraries(zederia-cli PRIVATE psapi)
endif()
# ui-snapshot draws the editor, so it needs the plug-in's code when the plug-in is built.
if(TARGET Zederia)
    target_compile_definitions(zederia-cli PRIVATE ZED_HAVE_UI=1 $<TARGET_PROPERTY:Zederia,COMPILE_DEFINITIONS>)
    target_include_directories(zederia-cli PRIVATE ${CMAKE_SOURCE_DIR}/plugin $<TARGET_PROPERTY:Zederia,INCLUDE_DIRECTORIES>)
    target_link_libraries(zederia-cli PRIVATE Zederia)
endif()

# Its icon and its version in the file.
zed_add_version_resource(zederia-cli "Zederia command-line tool" "zederia-cli.exe" app)
