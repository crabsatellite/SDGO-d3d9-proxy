cmake_minimum_required(VERSION 3.20)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SDGO_VERSION LIMIT_COUNT 1)
project(sdgo_mod_framework VERSION ${SDGO_VERSION} LANGUAGES C CXX RC)
find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
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
#   stage/<config>/server/  -> Battle/Agent/World host folders: winmm.dll, sdgo/...
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
sdgo_version_resource(winmm_shim winmm.dll "SDGO mod framework loader (server)")
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
    # Copy on every target build: this refreshes changed source data and also
    # repairs a stale/corrupted staged file whose timestamp is newer than source.
    set(mod_data_commands COMMAND "${CMAKE_COMMAND}" -E make_directory "${SDGO_STAGE}/${root}/mods/${id}")
    foreach(file mod.ini ${MOD_FILES})
        list(APPEND mod_data_commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/mods/${id}/${file}" "${SDGO_STAGE}/${root}/mods/${id}/${file}")
    endforeach()
    add_custom_target(${id}_data ${mod_data_commands} VERBATIM)
    add_dependencies(${id} ${id}_data)
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
sdgo_add_mod(battle_rewards SERVER DESCRIPTION "native contribution rewards and owner multipliers"
             SOURCES mods/battle_rewards/battle_rewards.cpp LIBS bcrypt FILES config.example.ini)
sdgo_add_mod(room_random SERVER DESCRIPTION "per-round random owned human loadouts"
             SOURCES mods/room_random/room_random.cpp mods/room_random/inventory.cpp
             LIBS odbc32 bcrypt FILES config.example.ini)

sdgo_add_mod(net_route DESCRIPTION "configured network endpoint routes" SOURCES mods/net_route/net_route.cpp LIBS ws2_32 shell32 FILES config.example.ini)
sdgo_add_mod(agent_udp_pool SERVER DESCRIPTION "native Agent UDP port pool" SOURCES mods/agent_udp_pool/agent_udp_pool.cpp LIBS ws2_32 FILES config.example.ini)
sdgo_add_mod(agent_relay_log_fix SERVER DESCRIPTION "native Agent relay printf fix" SOURCES mods/agent_relay_log_fix/agent_relay_log_fix.cpp)
sdgo_add_mod(rank_progression_fix SERVER DESCRIPTION "native World battle rank progression fix"
             SOURCES mods/rank_progression_fix/rank_progression_fix.cpp LIBS ole32 oleaut32 bcrypt uuid)

sdgo_add_mod(new_account_profile SERVER DESCRIPTION "new-account starter currency and rank" SOURCES mods/new_account_profile/new_account_profile.cpp LIBS ole32 oleaut32 uuid FILES config.example.ini)

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

# The same manifests drive package selection and runtime category/feature gates.
add_custom_target(sdgo_catalog ALL
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/mod_catalog.py"
        --root "${CMAKE_CURRENT_SOURCE_DIR}"
        --output "${SDGO_STAGE}/client/sdgo/catalog.json"
        --output "${SDGO_STAGE}/server/sdgo/catalog.json"
    COMMENT "Validating and staging the categorized mod catalog"
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

# Real-core transport tests, never staged into release packages.
add_executable(net_route_test tests/net_route_test.cpp)
target_link_libraries(net_route_test PRIVATE sdgo_api ws2_32)
sdgo_strict(net_route_test)
add_executable(net_login_test tests/net_login_test.cpp)
target_link_libraries(net_login_test PRIVATE sdgo_api ws2_32 shell32)
sdgo_strict(net_login_test)
add_library(net_route_conflict_mod SHARED tests/net_route_conflict_mod.cpp)
target_link_libraries(net_route_conflict_mod PRIVATE sdgo_api ws2_32)
sdgo_strict(net_route_conflict_mod)
add_executable(agent_udp_pool_test tests/agent_udp_pool_test.cpp)
target_link_libraries(agent_udp_pool_test PRIVATE sdgo_api ws2_32)
sdgo_strict(agent_udp_pool_test)

# Offline AI contracts. The fixtures call production code with synthetic rooms,
# packets, HUDs and native callbacks; no game executable is loaded.
include(CTest)
if(BUILD_TESTING)
    # Explicit optional acceptance probe requiring a locally supplied Agent EXE.
    # It is never staged or run by CI against game files.
    add_library(native_agent_cash_probe_mod SHARED tests/native_agent_cash_probe_mod.cpp)
    target_link_libraries(native_agent_cash_probe_mod PRIVATE sdgo_api)
    sdgo_strict(native_agent_cash_probe_mod)
    # The daily component can be supplied by a later integrated target. Until
    # then compile its exact committed implementation as a non-staged fixture.
    if(TARGET daily_rewards)
        set(SDGO_REWARD_DAILY_TEST_TARGET daily_rewards)
    else()
        add_library(daily_rewards_coexist_fixture SHARED tests/fixtures/daily_rewards_coexist/daily_rewards.cpp)
        target_link_libraries(daily_rewards_coexist_fixture PRIVATE sdgo_api)
        set_target_properties(daily_rewards_coexist_fixture PROPERTIES OUTPUT_NAME daily_rewards
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/reward_daily_fixture/$<CONFIG>")
        sdgo_strict(daily_rewards_coexist_fixture)
        set(SDGO_REWARD_DAILY_TEST_TARGET daily_rewards_coexist_fixture)
    endif()
    add_executable(battle_rewards_runtime_test tests/battle_rewards_runtime_test.cpp)
    target_link_libraries(battle_rewards_runtime_test PRIVATE sdgo_api)
    target_include_directories(battle_rewards_runtime_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_rewards_runtime_test)
    add_dependencies(battle_rewards_runtime_test battle_rewards sdgo_core ${SDGO_REWARD_DAILY_TEST_TARGET})
    add_test(NAME battle_reward_runtime_integration COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/battle_rewards_runtime_test.ps1" -Config "$<CONFIG>"
        -BuildDir "${CMAKE_BINARY_DIR}" -DailyRewardsDll "$<TARGET_FILE:${SDGO_REWARD_DAILY_TEST_TARGET}>")
    set_tests_properties(battle_reward_runtime_integration PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120 RUN_SERIAL TRUE)
    add_executable(battle_reward_mission_test tests/battle_reward_mission_test.cpp)
    target_link_libraries(battle_reward_mission_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_mission_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_mission_test)
    add_test(NAME battle_reward_mission_offline COMMAND battle_reward_mission_test)
    set_tests_properties(battle_reward_mission_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_stats_test tests/battle_reward_stats_test.cpp)
    target_link_libraries(battle_reward_stats_test PRIVATE sdgo_api)
    sdgo_strict(battle_reward_stats_test)
    add_test(NAME battle_reward_stats_offline COMMAND battle_reward_stats_test)
    set_tests_properties(battle_reward_stats_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_native_damage_test tests/battle_reward_native_damage_test.cpp)
    target_link_libraries(battle_reward_native_damage_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_native_damage_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_native_damage_test)
    add_test(NAME battle_reward_native_damage_offline COMMAND battle_reward_native_damage_test)
    set_tests_properties(battle_reward_native_damage_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_jobs_test tests/battle_reward_jobs_test.cpp)
    target_link_libraries(battle_reward_jobs_test PRIVATE sdgo_api bcrypt)
    target_include_directories(battle_reward_jobs_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_jobs_test)
    add_test(NAME battle_reward_jobs_offline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_reward_job_interop.py" "$<TARGET_FILE:battle_reward_jobs_test>")
    set_tests_properties(battle_reward_jobs_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_executable(battle_reward_cash_notify_test tests/battle_reward_cash_notify_test.cpp)
    target_link_libraries(battle_reward_cash_notify_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_cash_notify_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_cash_notify_test)
    add_test(NAME battle_reward_cash_notify_offline COMMAND battle_reward_cash_notify_test)
    set_tests_properties(battle_reward_cash_notify_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_boss_test tests/battle_reward_boss_test.cpp)
    target_link_libraries(battle_reward_boss_test PRIVATE sdgo_api bcrypt)
    target_include_directories(battle_reward_boss_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_boss_test)
    add_test(NAME battle_reward_boss_offline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/reward_boss_native_probe.py" "$<TARGET_FILE:battle_reward_boss_test>")
    set_tests_properties(battle_reward_boss_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_executable(battle_reward_combat_test tests/battle_reward_combat_test.cpp)
    target_link_libraries(battle_reward_combat_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_combat_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_combat_test)
    add_test(NAME battle_reward_combat_offline COMMAND battle_reward_combat_test)
    set_tests_properties(battle_reward_combat_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_runtime_state_test tests/battle_reward_runtime_state_test.cpp)
    target_link_libraries(battle_reward_runtime_state_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_runtime_state_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_runtime_state_test)
    add_test(NAME battle_reward_runtime_state_offline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_state_probe.py" "$<TARGET_FILE:battle_reward_runtime_state_test>")
    set_tests_properties(battle_reward_runtime_state_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_executable(battle_reward_plan_test tests/battle_reward_plan_test.cpp)
    target_link_libraries(battle_reward_plan_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_plan_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_plan_test)
    add_test(NAME battle_reward_plan_offline COMMAND battle_reward_plan_test)
    set_tests_properties(battle_reward_plan_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_bridge_test tests/battle_reward_bridge_test.cpp)
    target_link_libraries(battle_reward_bridge_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_bridge_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_bridge_test)
    add_test(NAME battle_reward_bridge_offline COMMAND battle_reward_bridge_test)
    set_tests_properties(battle_reward_bridge_offline PROPERTIES TIMEOUT 30)
    add_executable(battle_reward_policy_test tests/battle_reward_policy_test.cpp)
    target_link_libraries(battle_reward_policy_test PRIVATE sdgo_api)
    target_include_directories(battle_reward_policy_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_policy_test)
    add_test(NAME battle_reward_policy_offline COMMAND battle_reward_policy_test)
    set_tests_properties(battle_reward_policy_offline PROPERTIES TIMEOUT 30)
    add_test(NAME battle_reward_cpp_parity COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/reward_cpp_parity.py" --no-build --exe "$<TARGET_FILE:battle_reward_policy_test>")
    set_tests_properties(battle_reward_cpp_parity PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME battle_reward_owner_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_battle_reward*.py" -v)
    set_tests_properties(battle_reward_owner_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME battle_reward_delivery_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_reward*.py" -v)
    set_tests_properties(battle_reward_delivery_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120)

    add_executable(rank_progression_fix_test tests/rank_progression_fix_test.cpp)
    target_link_libraries(rank_progression_fix_test PRIVATE sdgo_api ole32 oleaut32 bcrypt uuid)
    sdgo_strict(rank_progression_fix_test)
    add_test(NAME rank_progression_fix_offline COMMAND rank_progression_fix_test)
    set_tests_properties(rank_progression_fix_offline PROPERTIES TIMEOUT 30)
    add_executable(rank_progression_db_test tests/rank_progression_db_test.cpp)
    target_link_libraries(rank_progression_db_test PRIVATE ole32 oleaut32 bcrypt uuid)
    sdgo_strict(rank_progression_db_test)
    add_library(ai_catalog_provider_fixture SHARED tests/ai_catalog_provider_fixture.cpp)
    set_target_properties(ai_catalog_provider_fixture PROPERTIES OUTPUT_NAME ai_catalog
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/provider_fixture/$<CONFIG>/ai_catalog")
    sdgo_strict(ai_catalog_provider_fixture)
    add_executable(ai_catalog_provider_test tests/ai_catalog_provider_test.cpp)
    target_link_libraries(ai_catalog_provider_test PRIVATE sdgo_api)
    sdgo_strict(ai_catalog_provider_test)
    add_dependencies(ai_catalog_provider_test ai_catalog_provider_fixture)
    add_test(NAME ai_catalog_provider_offline COMMAND ai_catalog_provider_test $<TARGET_FILE:ai_catalog_provider_fixture>)
    set_tests_properties(ai_catalog_provider_offline PROPERTIES TIMEOUT 30)

    add_executable(room_commands_test tests/room_commands_test.cpp)
    target_link_libraries(room_commands_test PRIVATE sdgo_api)
    sdgo_strict(room_commands_test)
    add_test(NAME room_commands_offline COMMAND room_commands_test)
    set_tests_properties(room_commands_offline PROPERTIES TIMEOUT 30)

    add_executable(room_random_test tests/room_random_test.cpp)
    target_link_libraries(room_random_test PRIVATE sdgo_api bcrypt)
    sdgo_strict(room_random_test)
    add_test(NAME room_random_offline COMMAND room_random_test)
    set_tests_properties(room_random_offline PROPERTIES TIMEOUT 30)

    # Explicitly invoked read-only host probe; not a CTest or staged executable.
    add_executable(room_random_inventory_probe tests/room_random_inventory_probe.cpp mods/room_random/inventory.cpp)
    target_link_libraries(room_random_inventory_probe PRIVATE sdgo_api odbc32)
    sdgo_strict(room_random_inventory_probe)

    add_library(catalog_gate_probe SHARED tests/catalog_gate_probe.cpp)
    target_link_libraries(catalog_gate_probe PRIVATE sdgo_api)
    sdgo_strict(catalog_gate_probe)
    add_test(NAME mod_catalog_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_mod_catalog.py" -v)
    set_tests_properties(mod_catalog_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    foreach(mod pvp_ai match_ai coop_ai mission_ai)
        add_executable(${mod}_test "tests/${mod}_test.cpp")
        target_link_libraries(${mod}_test PRIVATE sdgo_api)
        sdgo_strict(${mod}_test)
        add_test(NAME ${mod}_offline COMMAND ${mod}_test)
        set_tests_properties(${mod}_offline PROPERTIES TIMEOUT 30)
    endforeach()

    find_program(SDGO_POWERSHELL NAMES powershell pwsh)
    if(SDGO_POWERSHELL)
        add_test(NAME catalog_gates_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/catalog_gate_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(catalog_gates_offline PROPERTIES TIMEOUT 60)
        add_test(NAME package_catalog_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_catalog_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(package_catalog_offline PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
        foreach(check loader findwindow_fix)
            add_test(NAME ${check}_offline COMMAND "${SDGO_POWERSHELL}"
                -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/${check}_test.ps1"
                -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
            set_tests_properties(${check}_offline PROPERTIES TIMEOUT 60)
        endforeach()
        add_test(NAME installer_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_test.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}")
        # Exercises repeated client/Battle/World updates in child PowerShell processes.
        set_tests_properties(installer_offline PROPERTIES TIMEOUT 180)
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

if(BUILD_TESTING)
    add_executable(new_account_policy_test tests/new_account_policy_test.cpp)
    sdgo_strict(new_account_policy_test)
    add_test(NAME new_account_policy COMMAND new_account_policy_test)
    set_tests_properties(new_account_policy PROPERTIES TIMEOUT 30)
    # Opt-in SQL fixture: needs an isolated container and local credentials.
    add_executable(new_account_sql_test tests/new_account_sql_test.cpp)
    target_link_libraries(new_account_sql_test PRIVATE sdgo_api ole32 oleaut32 uuid)
    sdgo_strict(new_account_sql_test)
endif()
