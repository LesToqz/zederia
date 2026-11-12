add_executable(zederia-cli main.cpp TableCommands.cpp CliUtil.h)
target_link_libraries(zederia-cli PRIVATE zederia_core zederia_warnings)
