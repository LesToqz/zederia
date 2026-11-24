# Windows resources. The icon is plugin/brand/zederia.ico, made from the logo's SVG by
# scripts/make-icons.ps1 (zederia-cli brand). Programs made here carry their version: the
# numeric one in the version fields, ZED_VERSION_FULL ("1.0.0-rc1") in the text.
set(ZED_ICON "${CMAKE_SOURCE_DIR}/plugin/brand/zederia.ico")

# The icon alone, for the plug-in targets (the framework writes their version resource).
function(zed_add_icon_resource target)
    if(NOT WIN32 OR NOT TARGET ${target})
        return()
    endif()
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_icon.rc")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/Icon.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
    set_property(SOURCE "${rc}" APPEND PROPERTY OBJECT_DEPENDS "${ZED_ICON}")
endfunction()

# The icon and a version resource, for the command-line tool and the agent server.
function(zed_add_version_resource target description file type)
    if(NOT WIN32)
        return()
    endif()
    set(ZED_RC_DESCRIPTION "${description}")
    set(ZED_RC_FILE "${file}")
    get_filename_component(ZED_RC_NAME "${file}" NAME_WE)
    if(type STREQUAL "dll")
        set(ZED_RC_TYPE "VFT_DLL")
    else()
        set(ZED_RC_TYPE "VFT_APP")
    endif()
    if(ZED_VERSION_SUFFIX)
        set(ZED_RC_FLAGS "VS_FF_PRERELEASE")
    else()
        set(ZED_RC_FLAGS "0x0L")
    endif()
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/VersionInfo.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
    set_property(SOURCE "${rc}" APPEND PROPERTY OBJECT_DEPENDS "${ZED_ICON}")
endfunction()
