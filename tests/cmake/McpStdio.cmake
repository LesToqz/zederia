# The real zederia-mcp program over its standard streams: one scripted session per protocol
# generation (2025-06-18 and 2025-11-25 with initialize; 2026-07-28 with per-request metadata).
#   cmake -DMCP=<zederia-mcp> -DOUT=<folder> -P McpStdio.cmake
file(REMOVE_RECURSE "${OUT}")
file(MAKE_DIRECTORY "${OUT}/user" "${OUT}/factory" "${OUT}/settings" "${OUT}/local")
set(ENV{ZEDERIA_SETTINGS_DIR} "${OUT}/settings")
set(ENV{ZEDERIA_LOCAL_DIR} "${OUT}/local")
set(meta [=[{"io.modelcontextprotocol/protocolVersion":"2026-07-28","io.modelcontextprotocol/clientCapabilities":{},"io.modelcontextprotocol/clientInfo":{"name":"ctest","version":"1"}}]=])

function(run_session name input)
    file(WRITE "${OUT}/${name}.in" "${input}")
    execute_process(COMMAND "${MCP}" --stdio --user-dir "${OUT}/user" --factory-dir "${OUT}/factory" --no-live
                    INPUT_FILE "${OUT}/${name}.in" OUTPUT_VARIABLE out ERROR_VARIABLE err RESULT_VARIABLE rc TIMEOUT 120)
    file(WRITE "${OUT}/${name}.out" "${out}")
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${name}: zederia-mcp exited with ${rc}: ${err}")
    endif()
    set(session_out "${out}" PARENT_SCOPE)
endfunction()

function(expect name text)
    string(FIND "${session_out}" "${text}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "${name}: the output lacks ${text}\n--- output ---\n${session_out}")
    endif()
endfunction()

foreach(version 2025-06-18 2025-11-25)
    run_session(legacy_${version}
"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"${version}\",\"capabilities\":{},\"clientInfo\":{\"name\":\"ctest\",\"version\":\"1\"}}}
{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}
{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}
{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"open_session\",\"arguments\":{}}}
{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/read\",\"params\":{\"uri\":\"zederia://guide/agent\"}}
{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"prompts/list\"}
")
    expect(legacy_${version} "\"protocolVersion\":\"${version}\"")
    expect(legacy_${version} "\"name\":\"export_pack\"")
    expect(legacy_${version} "\"target\":\"session:")
    expect(legacy_${version} "Working with Zederia")
    expect(legacy_${version} "\"name\":\"design_sound\"")
endforeach()

run_session(modern
"{\"jsonrpc\":\"2.0\",\"id\":\"probe\",\"method\":\"server/discover\",\"params\":{\"_meta\":${meta}}}
{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\",\"params\":{\"_meta\":${meta}}}
{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"theory\",\"arguments\":{\"action\":\"chord\",\"symbols\":[\"Fm7\"]},\"_meta\":${meta}}}
{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"initialize-not\",\"params\":{\"_meta\":${meta}}}
")
expect(modern "\"supportedVersions\":[\"2026-07-28\",\"2025-11-25\",\"2025-06-18\"]")
expect(modern "\"resultType\":\"complete\"")
expect(modern "\"cacheScope\":\"public\"")
expect(modern "\"symbol\":\"Fm7\"")
expect(modern "\"code\":-32601")
message(STATUS "zederia-mcp answered both generations over stdio")
