cmake_minimum_required(VERSION 3.20)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SDGO_VERSION LIMIT_COUNT 1)
project(sdgo_mod_framework VERSION ${SDGO_VERSION} LANGUAGES C CXX RC)
set(SDGO_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(SDGO_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(SDGO_VERSION_PATCH ${PROJECT_VERSION_PATCH})

# GOnline.exe is a 32-bit process.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "Configure for 32-bit: cmake -S . -B build -G \"Visual Studio 17 2022\" -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: d3d9.dll is a static import of the game, so it must not depend on a
# VC++ redistributable being installed on the player's machine.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Output is staged as two trees that mirror the install targets:
#   stage/<config>/client/  -> the game folder (next to GOnline.exe): d3d9.dll, sdgo/...
#   stage/<config>/server/  -> each battle server folder (next to BattleServer.exe): winmm.dll, sdgo/...
# tools/install_client.ps1 / install_server.ps1 copy them; tools/package.ps1 zips a release.
set(SDGO_STAGE "${CMAKE_BINARY_DIR}/stage/$<CONFIG>")

add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde32.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)

add_library(sdgo_api INTERFACE)
target_include_directories(sdgo_api INTERFACE include)

function(sdgo_stage target destination)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/${destination}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${target}> "${SDGO_STAGE}/${destination}/"
    )
endfunction()

# Version resource (cmake/version.rc.in) naming the product, so installers can tell this
# project's DLLs from unrelated ones with the same file name.
function(sdgo_version_resource target filename description)
    set(RES_FILENAME "${filename}")
    set(RES_DESCRIPTION "${description}")
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/version_${target}.rc")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
endfunction()

function(sdgo_strict target)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
endfunction()

# ---- loader shims: d3d9.dll next to GOnline.exe, winmm.dll next to BattleServer.exe --
add_library(early_boot STATIC loader/common/early_boot.cpp)
target_link_libraries(early_boot PUBLIC sdgo_api)
sdgo_strict(early_boot)

add_library(d3d9_shim SHARED loader/d3d9/d3d9_shim.cpp loader/d3d9/d3d9.def)
set_target_properties(d3d9_shim PROPERTIES OUTPUT_NAME d3d9)
target_link_libraries(d3d9_shim PRIVATE early_boot)
sdgo_strict(d3d9_shim)
sdgo_version_resource(d3d9_shim d3d9.dll "SDGO mod framework loader (client)")
sdgo_stage(d3d9_shim "client")

add_library(winmm_shim SHARED loader/winmm/winmm_shim.cpp loader/winmm/winmm.def)
set_target_properties(winmm_shim PROPERTIES OUTPUT_NAME winmm)
target_link_libraries(winmm_shim PRIVATE early_boot)
sdgo_strict(winmm_shim)
sdgo_version_resource(winmm_shim winmm.dll "SDGO mod framework loader (battle server)")
sdgo_stage(winmm_shim "server")

# ---- core: sdgo/core.dll ----------------------------------------------------------
add_library(sdgo_core SHARED core/core.cpp core/hooks.cpp core/log.cpp core/symbols.cpp)
set_target_properties(sdgo_core PROPERTIES OUTPUT_NAME core)
target_link_libraries(sdgo_core PRIVATE sdgo_api minhook)
sdgo_strict(sdgo_core)
sdgo_version_resource(sdgo_core core.dll "SDGO mod framework core")
sdgo_stage(sdgo_core "client/sdgo")
sdgo_stage(sdgo_core "server/sdgo")

# ---- mods: sdgo/mods/<id>/<id>.dll + mod.ini + extra files -------------------------
# Client mods go to stage/<config>/client/sdgo/mods/<id>, SERVER mods to the server tree.
function(sdgo_add_mod id)
    cmake_parse_arguments(MOD "SERVER" "DESCRIPTION" "SOURCES;FILES;LIBS" ${ARGN})
    if(MOD_SERVER)
        set(root "server/sdgo")
    else()
        set(root "client/sdgo")
    endif()
    add_library(${id} SHARED ${MOD_SOURCES})
    target_link_libraries(${id} PRIVATE sdgo_api ${MOD_LIBS})
    sdgo_strict(${id})
    sdgo_version_resource(${id} ${id}.dll "SDGO mod: ${MOD_DESCRIPTION}")
    sdgo_stage(${id} "${root}/mods/${id}")
    foreach(file mod.ini ${MOD_FILES})
        add_custom_command(TARGET ${id} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/mods/${id}/${file}" "${SDGO_STAGE}/${root}/mods/${id}/${file}")
    endforeach()
endfunction()

sdgo_add_mod(mission_ai DESCRIPTION "co-op mission wingmen (client)"
             SOURCES mods/mission_ai/mission_ai.cpp FILES mission_ai.lua)
sdgo_add_mod(ime_fix DESCRIPTION "IME commit length fix" SOURCES mods/ime_fix/ime_fix.cpp)
sdgo_add_mod(findwindow_fix DESCRIPTION "FindWindowW fix" SOURCES mods/findwindow_fix/findwindow_fix.cpp LIBS user32)
sdgo_add_mod(startup_crash_fix DESCRIPTION "random crash at game start fix" SOURCES mods/startup_crash_fix/startup_crash_fix.cpp)
sdgo_add_mod(net_probe DESCRIPTION "packet logger" SOURCES mods/net_probe/net_probe.cpp)
sdgo_add_mod(coop_ai SERVER DESCRIPTION "co-op mission wingmen (battle server)" SOURCES mods/coop_ai/coop_ai.cpp)
sdgo_add_mod(match_ai DESCRIPTION "PvP AI players (client)" SOURCES mods/match_ai/match_ai.cpp)
sdgo_add_mod(pvp_ai SERVER DESCRIPTION "PvP AI players (battle server)" SOURCES mods/pvp_ai/pvp_ai.cpp)

# Optional catalog provider, same mod id in the two processes. Owner-generated
# catalog.bin is never staged or bundled by a framework build.
foreach(role client server)
    set(target ai_catalog_${role})
    add_library(${target} SHARED mods/ai_catalog/${role}.cpp)
    set_target_properties(${target} PROPERTIES OUTPUT_NAME ai_catalog
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/catalog_${role}/$<CONFIG>"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/catalog_${role}/$<CONFIG>")
    target_link_libraries(${target} PRIVATE sdgo_api)
    sdgo_strict(${target})
    sdgo_version_resource(${target} ai_catalog.dll "SDGO mod: AI roster (${role})")
    sdgo_stage(${target} "${role}/sdgo/mods/ai_catalog")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/${role}.ini"
            "${SDGO_STAGE}/${role}/sdgo/mods/ai_catalog/mod.ini")
endforeach()
add_custom_target(ai_catalog_data ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/client/sdgo/mods/ai_catalog"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/server/sdgo/mods/ai_catalog"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/client.ini"
        "${SDGO_STAGE}/client/sdgo/mods/ai_catalog/mod.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/server.ini"
        "${SDGO_STAGE}/server/sdgo/mods/ai_catalog/mod.ini")

# ---- data: symbol files and default configuration ---------------------------------
file(GLOB SDGO_SYMBOL_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/symbols/*.ini")
add_custom_target(sdgo_data ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/client/sdgo/symbols"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDGO_SYMBOL_FILES} "${SDGO_STAGE}/client/sdgo/symbols/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/config/sdgo.ini" "${SDGO_STAGE}/client/sdgo/"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/server/sdgo/symbols"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SDGO_SYMBOL_FILES} "${SDGO_STAGE}/server/sdgo/symbols/"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/config/sdgo.ini" "${SDGO_STAGE}/server/sdgo/"
    COMMENT "Staging symbol files and sdgo.ini"
)

# ---- tests: runtime check of the loader's forwarding (tests/loader_test.ps1) -------
add_executable(loader_test tests/loader_test.c)
sdgo_strict(loader_test)

# ---- tests: findwindow_fix against a simulated SDPlus hook (tests/findwindow_fix_test.ps1)
# None of these are staged into the client or server package.
add_executable(findwindow_fix_test tests/findwindow_fix_test.cpp)
target_link_libraries(findwindow_fix_test PRIVATE sdgo_api minhook user32)
sdgo_strict(findwindow_fix_test)
add_library(export_hook_test_mod SHARED tests/export_hook_test_mod.cpp)
target_link_libraries(export_hook_test_mod PRIVATE sdgo_api user32)
sdgo_strict(export_hook_test_mod)
foreach(fixture SDPlus generic_input_method)
    add_library(findwindow_fixture_${fixture} SHARED tests/findwindow_caller_fixture.cpp)
    set_target_properties(findwindow_fixture_${fixture} PROPERTIES OUTPUT_NAME ${fixture})
    target_link_libraries(findwindow_fixture_${fixture} PRIVATE user32)
    sdgo_strict(findwindow_fixture_${fixture})
endforeach()

# Offline AI contracts. The fixtures call production code with synthetic rooms,
# packets, HUDs and native callbacks; no game executable is loaded.
include(CTest)
if(BUILD_TESTING)
    add_library(catalog_conflict_mod SHARED tests/catalog_conflict_mod.cpp)
    target_link_libraries(catalog_conflict_mod PRIVATE sdgo_api)
    sdgo_strict(catalog_conflict_mod)
    add_executable(ai_catalog_test tests/ai_catalog_test.cpp)
    target_link_libraries(ai_catalog_test PRIVATE sdgo_api)
    sdgo_strict(ai_catalog_test)
    # Keep native fixture calls as hookable calls in optimized builds as well.
    target_compile_options(ai_catalog_test PRIVATE /Ob0)
    add_executable(ai_catalog_native_test tests/ai_catalog_native_test.cpp)
    target_link_libraries(ai_catalog_native_test PRIVATE sdgo_api minhook)
    sdgo_strict(ai_catalog_native_test)
    add_executable(ai_catalog_client_data_test tests/ai_catalog_client_data_test.cpp)
    target_link_libraries(ai_catalog_client_data_test PRIVATE sdgo_api)
    sdgo_strict(ai_catalog_client_data_test)
    foreach(mod pvp_ai match_ai coop_ai mission_ai)
        add_executable(${mod}_test "tests/${mod}_test.cpp")
        target_link_libraries(${mod}_test PRIVATE sdgo_api)
        sdgo_strict(${mod}_test)
        add_test(NAME ${mod}_offline COMMAND ${mod}_test)
        set_tests_properties(${mod}_offline PROPERTIES TIMEOUT 30)
    endforeach()

    find_program(SDGO_POWERSHELL NAMES powershell pwsh)
    if(SDGO_POWERSHELL)
        add_test(NAME ai_catalog_integration COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_catalog_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(ai_catalog_integration PROPERTIES TIMEOUT 60)
        foreach(check loader findwindow_fix)
            add_test(NAME ${check}_offline COMMAND "${SDGO_POWERSHELL}"
                -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/${check}_test.ps1"
                -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
            set_tests_properties(${check}_offline PROPERTIES TIMEOUT 60)
        endforeach()
        add_test(NAME installer_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_test.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(installer_offline PROPERTIES TIMEOUT 60)
        add_test(NAME mod_staging_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/staging_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(mod_staging_offline PROPERTIES TIMEOUT 90)
    endif()

    # Optional official Lua 5.0.3 source; never linked into a shipped mod.
    set(SDGO_LUA50_SOURCE "" CACHE PATH "Extracted official Lua 5.0.3 source for offline tests")
    if(SDGO_LUA50_SOURCE)
        if(NOT EXISTS "${SDGO_LUA50_SOURCE}/include/lua.h" OR NOT EXISTS "${SDGO_LUA50_SOURCE}/src/lapi.c")
            message(FATAL_ERROR "SDGO_LUA50_SOURCE must point to extracted official Lua 5.0.3 sources")
        endif()
        file(GLOB SDGO_LUA50_CORE CONFIGURE_DEPENDS "${SDGO_LUA50_SOURCE}/src/*.c")
        add_library(ai_test_lua50 STATIC ${SDGO_LUA50_CORE}
            "${SDGO_LUA50_SOURCE}/src/lib/lauxlib.c"
            "${SDGO_LUA50_SOURCE}/src/lib/lbaselib.c"
            "${SDGO_LUA50_SOURCE}/src/lib/lstrlib.c")
        target_include_directories(ai_test_lua50 PUBLIC "${SDGO_LUA50_SOURCE}/include")
        target_include_directories(ai_test_lua50 PRIVATE "${SDGO_LUA50_SOURCE}/src")
        target_compile_definitions(ai_test_lua50 PRIVATE _CRT_SECURE_NO_WARNINGS)
        add_executable(mission_ai_lua_test tools/lua50_host.c)
        target_link_libraries(mission_ai_lua_test PRIVATE ai_test_lua50)
        add_test(NAME mission_ai_lua_offline COMMAND mission_ai_lua_test mods/mission_ai/test/sim_mission.lua)
        set_tests_properties(mission_ai_lua_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    else()
        message(STATUS "Lua simulation disabled; set SDGO_LUA50_SOURCE to include it")
    endif()
endif()
