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
    foreach(file mod.ini manager.json ${MOD_FILES})
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
sdgo_add_mod(shop_labels DESCRIPTION "shop rental and Unicode labels" SOURCES mods/shop_labels/shop_labels.cpp FILES display.example.ini)
add_library(shop_catalog_engine STATIC mods/shop_catalog/catalog_runtime.cpp)
target_include_directories(shop_catalog_engine PUBLIC mods/shop_catalog)
target_link_libraries(shop_catalog_engine PUBLIC odbc32)
sdgo_strict(shop_catalog_engine)
sdgo_add_mod(shop_catalog SERVER DESCRIPTION "host-managed native shop catalog" SOURCES mods/shop_catalog/shop_catalog.cpp FILES catalog.example.ini LIBS shop_catalog_engine)
add_executable(shop_catalog_apply tools/shop_catalog_apply.cpp)
target_link_libraries(shop_catalog_apply PRIVATE shop_catalog_engine)
sdgo_strict(shop_catalog_apply)
sdgo_stage(shop_catalog_apply "server/sdgo/mods/shop_catalog")
sdgo_add_mod(coop_ai SERVER DESCRIPTION "co-op mission wingmen (battle server)" SOURCES mods/coop_ai/coop_ai.cpp)
sdgo_add_mod(match_ai DESCRIPTION "PvP AI players (client)" SOURCES mods/match_ai/match_ai.cpp)
sdgo_add_mod(pvp_ai SERVER DESCRIPTION "PvP AI players (battle server)" SOURCES mods/pvp_ai/pvp_ai.cpp)
sdgo_add_mod(battle_ai_server SERVER DESCRIPTION "player resource rules for PvP bots" SOURCES mods/battle_ai_server/battle_ai_server.cpp)
sdgo_add_mod(battle_ai DESCRIPTION "battle AI decisions and stuck recovery" SOURCES mods/battle_ai/battle_ai.cpp FILES battle_ai.lua universal_ai.lua config.example.ini)
sdgo_add_mod(hangar_manager DESCRIPTION "native hangar sorting and duplicate exchange"
             SOURCES mods/hangar_manager/hangar_manager.cpp mods/hangar_manager/native_ui.cpp FILES units.tsv LIBS user32 ole32)
sdgo_add_mod(storage_claim DESCRIPTION "native storage claim-all button"
             SOURCES mods/storage_claim/storage_claim.cpp mods/storage_claim/native_ui.cpp LIBS user32)
sdgo_add_mod(operator_catalog DESCRIPTION "operator names, portraits and item registration"
             SOURCES mods/operator_catalog/operator_catalog.cpp FILES display.example.ini)
sdgo_add_mod(battle_rewards SERVER DESCRIPTION "native contribution rewards and owner multipliers"
             SOURCES mods/battle_rewards/battle_rewards.cpp LIBS bcrypt FILES config.example.ini)
sdgo_add_mod(room_random_client DESCRIPTION "native random loadout client synchronization"
             SOURCES mods/room_random_client/room_random_client.cpp)
sdgo_add_mod(room_random SERVER DESCRIPTION "per-round random owned human loadouts"
             SOURCES mods/room_random/room_random.cpp mods/room_random/inventory.cpp
             LIBS odbc32 bcrypt FILES config.example.ini)

sdgo_add_mod(net_route DESCRIPTION "configured network endpoint routes" SOURCES mods/net_route/net_route.cpp LIBS ws2_32 shell32 FILES config.example.ini)
sdgo_add_mod(agent_udp_pool SERVER DESCRIPTION "native Agent UDP port pool" SOURCES mods/agent_udp_pool/agent_udp_pool.cpp LIBS ws2_32 FILES config.example.ini)
sdgo_add_mod(agent_relay_log_fix SERVER DESCRIPTION "native Agent relay printf fix" SOURCES mods/agent_relay_log_fix/agent_relay_log_fix.cpp)
sdgo_add_mod(game_test_client DESCRIPTION "isolated native GameTest client" SOURCES mods/game_test_client/game_test_client.cpp LIBS user32 ws2_32)
sdgo_add_mod(game_test_server SERVER DESCRIPTION "isolated native GameTest battle server" SOURCES mods/game_test_server/game_test_server.cpp)

# Bounded developer recorder, outside the installed Mod/runtime trees.
add_executable(game_test_audio tools/game_test_audio.cpp)
target_link_libraries(game_test_audio PRIVATE sdgo_api mmdevapi ole32)
sdgo_strict(game_test_audio)
sdgo_version_resource(game_test_audio game_test_audio.exe "SDGO GameTest process audio capture")

sdgo_add_mod(daily_rewards SERVER DESCRIPTION "daily double battle rewards (Beta)"
             SOURCES mods/daily_rewards/daily_rewards.cpp FILES config.example.ini)
sdgo_add_mod(rank_progression_fix SERVER DESCRIPTION "native World battle rank progression fix"
             SOURCES mods/rank_progression_fix/rank_progression_fix.cpp LIBS ole32 oleaut32 bcrypt uuid)


# Private feature libraries; no room metadata or gameplay state enters the core.
add_library(room_metadata_common STATIC mods/room_metadata/common/room_metadata_common.cpp)
target_include_directories(room_metadata_common PUBLIC mods/room_metadata/common)
sdgo_strict(room_metadata_common)
add_library(room_titles_client_state STATIC mods/room_titles/client_state.cpp)
target_include_directories(room_titles_client_state PUBLIC mods/room_titles)
target_link_libraries(room_titles_client_state PUBLIC room_metadata_common)
sdgo_strict(room_titles_client_state)
add_library(room_titles_native_transport STATIC mods/room_titles/native_transport.cpp)
target_link_libraries(room_titles_native_transport PUBLIC sdgo_api room_metadata_common)
sdgo_strict(room_titles_native_transport)

sdgo_add_mod(room_titles_agent SERVER DESCRIPTION "room title transport (Agent)"
             SOURCES mods/room_titles_agent/room_titles_agent.cpp LIBS room_metadata_common)
sdgo_add_mod(room_titles DESCRIPTION "extended room titles (client)"
             SOURCES mods/room_titles/room_titles.cpp mods/room_titles/native_ui.cpp
             LIBS room_titles_native_transport room_titles_client_state user32)
sdgo_add_mod(room_titles_server SERVER DESCRIPTION "extended room titles (Battle)"
             SOURCES mods/room_titles_server/room_titles_server.cpp mods/room_titles_server/room_titles_server_logic.cpp
             LIBS room_metadata_common bcrypt)

sdgo_add_mod(new_account_profile SERVER DESCRIPTION "new-account starter currency and rank" SOURCES mods/new_account_profile/new_account_profile.cpp LIBS ole32 oleaut32 uuid FILES config.example.ini)
sdgo_add_mod(hangar_capacity SERVER DESCRIPTION "automatic native 1200-slot hangar"
             SOURCES mods/hangar_capacity/hangar_capacity.cpp LIBS ole32 oleaut32 uuid)

sdgo_add_mod(collection_capsule SERVER DESCRIPTION "never-acquired base-form capsule machine"
             SOURCES mods/collection_capsule/collection_capsule.cpp LIBS ole32 oleaut32 uuid FILES config.example.ini)

# Shared capsule resource provider, distributed inside the owner toolkit. The
# generated player bundle uses one capsule_catalog instance for all machines.
add_library(capsule_registry_support SHARED
    support/capsule_registry/capsule_catalog.cpp support/capsule_registry/capsule_runtime.cpp)
set_target_properties(capsule_registry_support PROPERTIES OUTPUT_NAME capsule_catalog)
target_link_libraries(capsule_registry_support PRIVATE sdgo_api)
sdgo_strict(capsule_registry_support)
sdgo_version_resource(capsule_registry_support capsule_catalog.dll "SDGO shared capsule resource registry")
sdgo_stage(capsule_registry_support "support/capsule-registry")

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
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/manager.json"
            "${SDGO_STAGE}/${role}/sdgo/mods/ai_catalog/manager.json")
endforeach()
add_custom_target(ai_catalog_data ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/client/sdgo/mods/ai_catalog"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/server/sdgo/mods/ai_catalog"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/client.ini"
        "${SDGO_STAGE}/client/sdgo/mods/ai_catalog/mod.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/mods/ai_catalog/server.ini"
        "${SDGO_STAGE}/server/sdgo/mods/ai_catalog/mod.ini")

sdgo_add_mod(capsule_catalog DESCRIPTION "native capsule catalog overlay"
             SOURCES mods/capsule_catalog/capsule_catalog.cpp mods/capsule_catalog/capsule_runtime.cpp)
sdgo_add_mod(capsule_currency_fix DESCRIPTION "activity capsule currency display fix"
             SOURCES mods/capsule_currency_fix/capsule_currency_fix.cpp mods/capsule_currency_fix/capsule_currency_logic.cpp)
sdgo_add_mod(capsule_price_probe DESCRIPTION "read-only capsule price UI diagnostics"
             SOURCES mods/capsule_price_probe/capsule_price_probe.cpp)

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

# One self-contained user entry. Individual mods only contribute contracts/panels.
option(SDGO_BUILD_MANAGER "Build the unified Mod Manager EXE" ON)
if(SDGO_BUILD_MANAGER)
    find_program(SDGO_MANAGER_POWERSHELL NAMES powershell pwsh REQUIRED)
    file(GLOB_RECURSE SDGO_MANAGER_INPUTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.html" "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.css"
        "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.json" "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.ps1" "${CMAKE_CURRENT_SOURCE_DIR}/manager/*.sql"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/shop/*.cs"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/collection_capsule/*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/collection_capsule/sql/*.sql"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/*collection_capsule*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/support/capsule_registry/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/mods/collection_capsule/assets/*"
        "${CMAKE_CURRENT_SOURCE_DIR}/mods/*/manager.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/rewards/*.py" "${CMAKE_CURRENT_SOURCE_DIR}/owner/rewards/*.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/owner/rewards/*.sql" "${CMAKE_CURRENT_SOURCE_DIR}/backend/*.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/backend/*.ps1")
    set(SDGO_MANAGER_EXE "${CMAKE_BINARY_DIR}/manager/SDGO Mod Manager.exe")
    add_custom_command(OUTPUT "${SDGO_MANAGER_EXE}"
        COMMAND "${SDGO_MANAGER_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_mod_manager.ps1"
            -BuildDir "${CMAKE_BINARY_DIR}" -Python "${Python3_EXECUTABLE}"
        DEPENDS ${SDGO_MANAGER_INPUTS} "${CMAKE_CURRENT_SOURCE_DIR}/tools/mod_manager.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_mod_manager.ps1"
            "${CMAKE_CURRENT_SOURCE_DIR}/manager/SDGOModManager.spec"
        VERBATIM)
    add_custom_target(mod_manager ALL DEPENDS "${SDGO_MANAGER_EXE}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SDGO_STAGE}/manager"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDGO_MANAGER_EXE}" "${SDGO_STAGE}/manager/")
endif()

# ---- tests: runtime check of the loader's forwarding (tests/loader_test.ps1) -------
add_executable(loader_test tests/loader_test.c)
sdgo_strict(loader_test)
add_executable(launcher_handshake_test tests/launcher_handshake_test.cpp)
sdgo_strict(launcher_handshake_test)

add_executable(capsule_client_runtime_harness tests/capsule_client_runtime_harness.cpp
               mods/capsule_catalog/capsule_runtime.cpp)
target_include_directories(capsule_client_runtime_harness PRIVATE mods/capsule_catalog)
sdgo_strict(capsule_client_runtime_harness)
add_executable(capsule_client_native_stream_test tests/capsule_client_native_stream_test.cpp
               mods/capsule_catalog/capsule_runtime.cpp)
target_include_directories(capsule_client_native_stream_test PRIVATE mods/capsule_catalog)
sdgo_strict(capsule_client_native_stream_test)
add_executable(capsule_currency_fix_test mods/capsule_currency_fix/capsule_currency_fix_test.cpp
               mods/capsule_currency_fix/capsule_currency_logic.cpp)
target_include_directories(capsule_currency_fix_test PRIVATE mods/capsule_currency_fix)
sdgo_strict(capsule_currency_fix_test)
add_executable(capsule_currency_dll_test tests/capsule_currency_dll_test.cpp)
target_link_libraries(capsule_currency_dll_test PRIVATE sdgo_api)
sdgo_strict(capsule_currency_dll_test)
add_dependencies(capsule_currency_dll_test capsule_currency_fix)
add_executable(capsule_price_probe_test mods/capsule_price_probe/capsule_price_probe_test.cpp)
sdgo_strict(capsule_price_probe_test)

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

add_executable(daily_rewards_policy_test tests/daily_rewards_policy_test.cpp)
target_link_libraries(daily_rewards_policy_test PRIVATE sdgo_api)
sdgo_strict(daily_rewards_policy_test)
add_executable(daily_rewards_runtime_test tests/daily_rewards_runtime_test.cpp)
target_link_libraries(daily_rewards_runtime_test PRIVATE sdgo_api)
sdgo_strict(daily_rewards_runtime_test)

# Offline AI contracts. The fixtures call production code with synthetic rooms,
# packets, HUDs and native callbacks; no game executable is loaded.
include(CTest)
if(BUILD_TESTING)
    add_executable(core_ini_test tests/core_ini_test.cpp)
    sdgo_strict(core_ini_test)
    add_test(NAME core_ini_offline COMMAND core_ini_test)
    add_executable(game_test_quiet_sockets_test tests/game_test_quiet_sockets_test.cpp)
    target_link_libraries(game_test_quiet_sockets_test PRIVATE sdgo_api ws2_32)
    sdgo_strict(game_test_quiet_sockets_test)
    add_test(NAME game_test_quiet_sockets_offline COMMAND game_test_quiet_sockets_test)
    add_executable(game_test_quiet_audio_test tests/game_test_quiet_audio_test.cpp)
    target_link_libraries(game_test_quiet_audio_test PRIVATE sdgo_api)
    sdgo_strict(game_test_quiet_audio_test)
    add_test(NAME game_test_quiet_audio_offline COMMAND game_test_quiet_audio_test)
    add_executable(game_test_quiet_graphics_test tests/game_test_quiet_graphics_test.cpp)
    target_link_libraries(game_test_quiet_graphics_test PRIVATE sdgo_api user32)
    sdgo_strict(game_test_quiet_graphics_test)
    add_test(NAME game_test_quiet_graphics_offline COMMAND game_test_quiet_graphics_test)
    add_test(NAME game_test_windows_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p test_game_test_windows.py)
    set_tests_properties(game_test_windows_offline PROPERTIES TIMEOUT 30)
    add_test(NAME launcher_handshake_offline COMMAND launcher_handshake_test)
    add_test(NAME capsule_currency_actual_dll COMMAND capsule_currency_dll_test "$<TARGET_FILE:capsule_currency_fix>")
    set_tests_properties(capsule_currency_actual_dll PROPERTIES TIMEOUT 30)
endif()
if(BUILD_TESTING)
    foreach(test capsule_currency_fix capsule_price_probe)
        add_test(NAME ${test}_offline COMMAND ${test}_test)
        set_tests_properties(${test}_offline PROPERTIES TIMEOUT 30)
    endforeach()
    add_test(NAME capsule_configuration COMMAND "${CMAKE_COMMAND}" -E env
        "CAPSULE_RUNTIME_HARNESS=$<TARGET_FILE:capsule_client_runtime_harness>"
        "${Python3_EXECUTABLE}" -B -X utf8 -m unittest discover -s tests -p "test_capsule*.py" -q)
    set_tests_properties(capsule_configuration PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 180)
endif()

if(BUILD_TESTING)
    add_executable(game_test_framework_host tests/game_test_framework_host.cpp)
    target_link_libraries(game_test_framework_host PRIVATE sdgo_api)
    sdgo_strict(game_test_framework_host)
    add_test(NAME game_test_framework_offline COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/game_test_framework_test.py"
        --host "$<TARGET_FILE:game_test_framework_host>")
    set_tests_properties(game_test_framework_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_executable(game_test_onboarding_gate_test tests/game_test_onboarding_gate_test.cpp)
    add_executable(game_test_native_credentials_test tests/game_test_native_credentials_test.cpp)
    target_link_libraries(game_test_native_credentials_test PRIVATE sdgo_api)
    sdgo_strict(game_test_native_credentials_test)
    add_test(NAME game_test_native_credentials_offline COMMAND game_test_native_credentials_test)
    set_tests_properties(game_test_native_credentials_offline PROPERTIES TIMEOUT 30)
    sdgo_strict(game_test_onboarding_gate_test)
    add_test(NAME game_test_onboarding_gate_offline COMMAND game_test_onboarding_gate_test)
    set_tests_properties(game_test_onboarding_gate_offline PROPERTIES TIMEOUT 30)
    add_executable(game_test_room_actions_test tests/game_test_room_actions_test.cpp)
    target_link_libraries(game_test_room_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_room_actions_test)
    add_test(NAME game_test_room_actions_offline COMMAND game_test_room_actions_test)
    set_tests_properties(game_test_room_actions_offline PROPERTIES TIMEOUT 30)
    add_executable(game_test_channel_actions_test tests/game_test_channel_actions_test.cpp)
    add_executable(game_test_hangar_navigation_test tests/game_test_hangar_navigation_test.cpp)
    add_executable(game_test_page_test_actions_test tests/game_test_page_test_actions_test.cpp)
    target_link_libraries(game_test_page_test_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_page_test_actions_test)
    add_test(NAME game_test_page_test_actions_offline COMMAND game_test_page_test_actions_test)
    add_executable(game_test_storage_test_actions_test tests/game_test_storage_test_actions_test.cpp)
    target_link_libraries(game_test_storage_test_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_storage_test_actions_test)
    add_test(NAME game_test_storage_test_actions_offline COMMAND game_test_storage_test_actions_test)
    add_executable(game_test_operator_catalog_actions_test tests/game_test_operator_catalog_actions_test.cpp)
    target_link_libraries(game_test_operator_catalog_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_operator_catalog_actions_test)
    add_test(NAME game_test_operator_catalog_actions_offline COMMAND game_test_operator_catalog_actions_test)
    target_link_libraries(game_test_hangar_navigation_test PRIVATE sdgo_api)
    sdgo_strict(game_test_hangar_navigation_test)
    add_test(NAME game_test_hangar_navigation_offline COMMAND game_test_hangar_navigation_test)
    foreach(action battleship battleship_deck gift)
        add_executable(game_test_${action}_actions_test tests/game_test_${action}_actions_test.cpp)
        target_link_libraries(game_test_${action}_actions_test PRIVATE sdgo_api)
        sdgo_strict(game_test_${action}_actions_test)
        add_test(NAME game_test_${action}_actions_offline COMMAND game_test_${action}_actions_test)
        set_tests_properties(game_test_${action}_actions_offline PROPERTIES TIMEOUT 30)
    endforeach()
    target_link_libraries(game_test_channel_actions_test PRIVATE sdgo_api)
    sdgo_strict(game_test_channel_actions_test)
    add_test(NAME game_test_channel_actions_offline COMMAND game_test_channel_actions_test)
    set_tests_properties(game_test_channel_actions_offline PROPERTIES TIMEOUT 30)
    add_executable(game_test_server_test tests/game_test_server_test.cpp)
    target_link_libraries(game_test_server_test PRIVATE sdgo_api)
    sdgo_strict(game_test_server_test)
    add_test(NAME game_test_server_offline COMMAND game_test_server_test)
    set_tests_properties(game_test_server_offline PROPERTIES TIMEOUT 30)

    if(SDGO_BUILD_MANAGER)
        add_test(NAME shop_owner_domain_service COMMAND "${CMAKE_BINARY_DIR}/manager-services/ShopUiTests.exe")
        set_tests_properties(shop_owner_domain_service PROPERTIES TIMEOUT 60)
    endif()
    if(SDGO_BUILD_MANAGER)
        add_test(NAME mod_manager_frozen COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/mod_manager_frozen_test.py" --exe "${SDGO_MANAGER_EXE}")
        set_tests_properties(mod_manager_frozen PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
    endif()
    add_test(NAME mod_manager_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_mod_manager*.py" -v)
    set_tests_properties(mod_manager_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 120)
    if(SDGO_BUILD_MANAGER)
        set_tests_properties(mod_manager_offline PROPERTIES
            ENVIRONMENT "SDGO_MANAGER_SERVICES=${CMAKE_BINARY_DIR}/manager-services/SdgoShopServices.dll")
    endif()
    add_test(NAME pr_ai_disclosure_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_pr_ai_disclosure.py" -v)
    set_tests_properties(pr_ai_disclosure_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_executable(hangar_model_test tests/hangar_model_test.cpp)
    target_link_libraries(hangar_model_test PRIVATE sdgo_api)
    sdgo_strict(hangar_model_test)
    target_compile_options(hangar_model_test PRIVATE /UNDEBUG)
    add_test(NAME hangar_model_offline COMMAND hangar_model_test)
    add_executable(hangar_native_test tests/hangar_native_test.cpp mods/hangar_manager/native_ui.cpp)
    target_link_libraries(hangar_native_test PRIVATE sdgo_api user32 ole32)
    sdgo_strict(hangar_native_test)
    target_compile_options(hangar_native_test PRIVATE /UNDEBUG)
    add_test(NAME hangar_native_offline COMMAND hangar_native_test)
    add_executable(hangar_page_ui_test tests/hangar_page_ui_test.cpp mods/hangar_manager/native_ui.cpp)
    target_link_libraries(hangar_page_ui_test PRIVATE sdgo_api user32)
    sdgo_strict(hangar_page_ui_test)
    target_compile_options(hangar_page_ui_test PRIVATE /UNDEBUG)
    add_test(NAME hangar_page_ui_offline COMMAND hangar_page_ui_test)
    add_executable(storage_claim_model_test tests/storage_claim_model_test.cpp)
    target_link_libraries(storage_claim_model_test PRIVATE sdgo_api)
    sdgo_strict(storage_claim_model_test)
    target_compile_options(storage_claim_model_test PRIVATE /UNDEBUG)
    add_test(NAME storage_claim_model_offline COMMAND storage_claim_model_test)
    add_executable(storage_claim_native_test tests/storage_claim_native_test.cpp mods/storage_claim/native_ui.cpp)
    target_link_libraries(storage_claim_native_test PRIVATE sdgo_api user32)
    sdgo_strict(storage_claim_native_test)
    target_compile_options(storage_claim_native_test PRIVATE /UNDEBUG)
    add_test(NAME storage_claim_native_offline COMMAND storage_claim_native_test)
    # Optional execution of two hash-pinned native UI snippets. The game image
    # is read only; no entry point, imports, rendering or networking is started.
    set(SDGO_HANGAR_NATIVE_HOST "" CACHE FILEPATH "Local supported GOnline.exe for hangar mouse replay")
    if(SDGO_HANGAR_NATIVE_HOST)
        add_executable(hangar_native_input_test tests/hangar_native_input_test.cpp mods/hangar_manager/native_ui.cpp)
        target_link_libraries(hangar_native_input_test PRIVATE sdgo_api user32 bcrypt)
        sdgo_strict(hangar_native_input_test)
        target_compile_options(hangar_native_input_test PRIVATE /UNDEBUG)
        add_test(NAME hangar_native_input_replay COMMAND hangar_native_input_test "${SDGO_HANGAR_NATIVE_HOST}")
        set_tests_properties(hangar_native_input_replay PROPERTIES TIMEOUT 60)
        add_executable(storage_claim_replay_test tests/storage_claim_replay_test.cpp mods/storage_claim/native_ui.cpp)
        target_link_libraries(storage_claim_replay_test PRIVATE sdgo_api user32 bcrypt)
        sdgo_strict(storage_claim_replay_test)
        target_compile_options(storage_claim_replay_test PRIVATE /UNDEBUG)
        add_test(NAME storage_claim_native_replay COMMAND storage_claim_replay_test "${SDGO_HANGAR_NATIVE_HOST}")
        set_tests_properties(storage_claim_native_replay PROPERTIES TIMEOUT 60)
    endif()
    add_executable(shop_labels_test tests/shop_labels_test.cpp)
    target_link_libraries(shop_labels_test PRIVATE sdgo_api)
    sdgo_strict(shop_labels_test)
    add_dependencies(shop_labels_test shop_labels)
    add_test(NAME shop_labels_dll_contract COMMAND shop_labels_test "$<TARGET_FILE:shop_labels>" "${CMAKE_BINARY_DIR}/shop_labels_test_data")
    add_executable(shop_catalog_mod_test tests/shop_catalog_mod_test.cpp)
    target_link_libraries(shop_catalog_mod_test PRIVATE sdgo_api)
    sdgo_strict(shop_catalog_mod_test)
    add_dependencies(shop_catalog_mod_test shop_catalog)
    if(SDGO_MANAGER_POWERSHELL)
        add_test(NAME shop_manager_ownership COMMAND "${SDGO_MANAGER_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/shop_manager_ownership_test.ps1" -BuildDir "${CMAKE_BINARY_DIR}" -Config "$<CONFIG>")
        set_tests_properties(shop_manager_ownership PROPERTIES TIMEOUT 60)
    endif()
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME shop_catalog_configuration COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover -s tests -p test_shop_catalog.py -v)
        set_tests_properties(shop_catalog_configuration PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
        add_test(NAME shop_package_installer COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/shop_install_test.py" --build "${CMAKE_BINARY_DIR}")
        set_tests_properties(shop_package_installer PROPERTIES TIMEOUT 360)
    endif()
    add_executable(operator_catalog_test tests/operator_catalog_test.cpp)
    target_link_libraries(operator_catalog_test PRIVATE sdgo_api)
    sdgo_strict(operator_catalog_test)
    foreach(case RANGE 0 3)
        add_test(NAME operator_catalog_${case} COMMAND operator_catalog_test "$<TARGET_FILE:operator_catalog>"
                 "${CMAKE_BINARY_DIR}/operator-case-${case}" "${case}")
    endforeach()
    add_test(NAME operator_assistant_offline COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
             -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_operator_assistant.py")
    set_tests_properties(operator_assistant_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_test(NAME daily_rewards_policy COMMAND daily_rewards_policy_test)
    set_tests_properties(daily_rewards_policy PROPERTIES TIMEOUT 30)
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
    add_executable(ai_catalog_client_map_test EXCLUDE_FROM_ALL tests/ai_catalog_client_map_test.cpp)
    target_link_libraries(ai_catalog_client_map_test PRIVATE sdgo_api kernel32 shell32)
    sdgo_strict(ai_catalog_client_map_test)
    target_compile_options(ai_catalog_client_map_test PRIVATE /Gy /GS- /GR-)
    target_link_options(ai_catalog_client_map_test PRIVATE /BASE:0x400000 /DYNAMICBASE:NO /NODEFAULTLIB /ENTRY:Entry /IGNORE:4078)
    # Explicit optional acceptance probe requiring a locally supplied Agent EXE.
    # It is never staged or run by CI against game files.
    add_library(native_agent_cash_probe_mod SHARED tests/native_agent_cash_probe_mod.cpp)
    target_link_libraries(native_agent_cash_probe_mod PRIVATE sdgo_api)
    sdgo_strict(native_agent_cash_probe_mod)
    # Explicit local image probe; no game bytes are bundled or required in CI.
    add_executable(battle_reward_native_result_probe EXCLUDE_FROM_ALL tests/battle_reward_native_result_probe.cpp)
    target_link_libraries(battle_reward_native_result_probe PRIVATE sdgo_api bcrypt)
    target_include_directories(battle_reward_native_result_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_reward_native_result_probe)
    # Exercise the shipped daily reward Mod in contribution-reward coexistence tests.
    add_executable(battle_rewards_runtime_test tests/battle_rewards_runtime_test.cpp)
    target_link_libraries(battle_rewards_runtime_test PRIVATE sdgo_api)
    target_include_directories(battle_rewards_runtime_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    sdgo_strict(battle_rewards_runtime_test)
    add_dependencies(battle_rewards_runtime_test battle_rewards sdgo_core daily_rewards)
    add_test(NAME battle_reward_runtime_integration COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/battle_rewards_runtime_test.ps1" -Config "$<CONFIG>"
        -BuildDir "${CMAKE_BINARY_DIR}" -DailyRewardsDll "$<TARGET_FILE:daily_rewards>")
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
    add_library(room_random_head_probe SHARED tests/room_random_head_probe.cpp)
    add_executable(room_random_client_policy_test tests/room_random_client_policy_test.cpp)
    sdgo_strict(room_random_client_policy_test)
    add_test(NAME room_random_client_policy COMMAND room_random_client_policy_test)
    target_link_libraries(room_random_head_probe PRIVATE sdgo_api)
    sdgo_strict(room_random_head_probe)
    add_executable(room_random_client_runtime_test tests/room_random_client_runtime_test.cpp)
    target_link_libraries(room_random_client_runtime_test PRIVATE sdgo_api)
    sdgo_strict(room_random_client_runtime_test)
    add_dependencies(room_random_client_runtime_test sdgo_core room_random_client room_random_head_probe)
    add_test(NAME room_random_client_runtime COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/room_random_client_runtime_test.ps1"
        -BuildDir "${CMAKE_BINARY_DIR}" -Config "$<CONFIG>")
    set_tests_properties(room_random_client_runtime PROPERTIES TIMEOUT 120)

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
    add_test(NAME game_test_runner_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_game_test.py" -v)
    set_tests_properties(game_test_runner_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_test(NAME game_test_server_report_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_game_test_server_report.py" -v)
    set_tests_properties(game_test_server_report_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
    add_library(battle_ai_server_bridge_fixture SHARED tests/battle_ai_server_bridge_fixture.cpp)
    sdgo_strict(battle_ai_server_bridge_fixture)
    foreach(mod pvp_ai match_ai coop_ai mission_ai)
        add_executable(${mod}_test "tests/${mod}_test.cpp")
        target_link_libraries(${mod}_test PRIVATE sdgo_api)
        sdgo_strict(${mod}_test)
        if(mod STREQUAL "pvp_ai")
            add_test(NAME ${mod}_offline
                COMMAND ${mod}_test "$<TARGET_FILE:battle_ai_server_bridge_fixture>")
        else()
            add_test(NAME ${mod}_offline COMMAND ${mod}_test)
        endif()
        set_tests_properties(${mod}_offline PROPERTIES TIMEOUT 30)
    endforeach()
    add_dependencies(pvp_ai_test battle_ai_server_bridge_fixture)
    target_link_libraries(match_ai_test PRIVATE minhook)
    add_executable(mission_ai_observer_test tests/mission_ai_observer_test.cpp)
    target_link_libraries(mission_ai_observer_test PRIVATE sdgo_api)
    sdgo_strict(mission_ai_observer_test)
    add_test(NAME mission_ai_observer_offline COMMAND mission_ai_observer_test)
    set_tests_properties(mission_ai_observer_offline PROPERTIES TIMEOUT 30)

    find_program(SDGO_POWERSHELL NAMES powershell pwsh)
    if(SDGO_POWERSHELL)
        add_test(NAME daily_rewards_runtime COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/daily_rewards_runtime_test.ps1"
            -Config "$<CONFIG>")
        set_tests_properties(daily_rewards_runtime PROPERTIES TIMEOUT 120)
        add_test(NAME ai_catalog_integration COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_catalog_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(ai_catalog_integration PROPERTIES TIMEOUT 60)
        add_test(NAME battle_ai_server_native_integration COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/battle_ai_server_native_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(battle_ai_server_native_integration PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
        add_test(NAME catalog_gates_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/catalog_gate_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(catalog_gates_offline PROPERTIES TIMEOUT 60)
        add_test(NAME package_catalog_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_catalog_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        # Eight editions plus packaged PowerShell installer/update readbacks.
        set_tests_properties(package_catalog_offline PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
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

# Optional full-title adapters use the current shared command grammar.
foreach(mode coop pvp)
    add_executable(${mode}_title_rules_test tests/${mode}_title_rules_test.cpp)
    target_link_libraries(${mode}_title_rules_test PRIVATE sdgo_api)
    sdgo_strict(${mode}_title_rules_test)
endforeach()

# Offline room-title tests and real-core isolated Agent bridge.
add_executable(room_metadata_common_test tests/room_metadata_common_test.cpp)
target_link_libraries(room_metadata_common_test PRIVATE room_metadata_common)
sdgo_strict(room_metadata_common_test)

add_executable(room_metadata_allocation_test tests/room_metadata_allocation_test.cpp mods/room_titles_server/room_titles_server_logic.cpp)
target_link_libraries(room_metadata_allocation_test PRIVATE room_metadata_common)
sdgo_strict(room_metadata_allocation_test)

add_executable(room_titles_client_state_test tests/room_titles_client_state_test.cpp)
target_link_libraries(room_titles_client_state_test PRIVATE room_titles_client_state)
sdgo_strict(room_titles_client_state_test)

add_executable(room_titles_transport_test tests/room_titles_transport_test.cpp)
target_link_libraries(room_titles_transport_test PRIVATE sdgo_api room_metadata_common)
sdgo_strict(room_titles_transport_test)

add_executable(room_titles_native_ui_test tests/room_titles_native_ui_test.cpp mods/room_titles/native_ui.cpp)
target_link_libraries(room_titles_native_ui_test PRIVATE sdgo_api user32)
sdgo_strict(room_titles_native_ui_test)

add_executable(room_titles_client_test tests/room_titles_client_test.cpp)
target_link_libraries(room_titles_client_test PRIVATE room_titles_client_state room_titles_native_transport user32)
sdgo_strict(room_titles_client_test)

add_executable(room_titles_server_test tests/room_titles_server_test.cpp mods/room_titles_server/room_titles_server_logic.cpp)
target_include_directories(room_titles_server_test PRIVATE mods/room_titles_server)
target_link_libraries(room_titles_server_test PRIVATE room_metadata_common)
sdgo_strict(room_titles_server_test)

add_executable(room_titles_server_native_test tests/room_titles_server_native_test.cpp mods/room_titles_server/room_titles_server_logic.cpp)
target_include_directories(room_titles_server_native_test PRIVATE mods/room_titles_server)
target_link_libraries(room_titles_server_native_test PRIVATE sdgo_api room_metadata_common bcrypt)
sdgo_strict(room_titles_server_native_test)

add_executable(room_title_native_test tests/room_title_native_test.cpp)
target_link_libraries(room_title_native_test PRIVATE bcrypt)
sdgo_strict(room_title_native_test)

add_executable(room_titles_agent_test tests/room_titles_agent_test.cpp)
target_link_libraries(room_titles_agent_test PRIVATE sdgo_api room_metadata_common)
sdgo_strict(room_titles_agent_test)

add_library(room_titles_agent_conflict_mod SHARED tests/room_titles_agent_test.cpp)
target_compile_definitions(room_titles_agent_conflict_mod PRIVATE ROOM_TITLES_AGENT_CONFLICT_MOD=1)
target_link_libraries(room_titles_agent_conflict_mod PRIVATE sdgo_api)
sdgo_strict(room_titles_agent_conflict_mod)


if(BUILD_TESTING)
    foreach(test room_metadata_common room_metadata_allocation room_titles_client_state room_titles_transport room_titles_native_ui room_titles_client room_titles_server room_titles_server_native coop_title_rules pvp_title_rules)
        add_test(NAME ${test}_offline COMMAND ${test}_test)
        set_tests_properties(${test}_offline PROPERTIES TIMEOUT 30)
    endforeach()
    if(SDGO_POWERSHELL)
        add_test(NAME room_titles_agent_offline COMMAND "${SDGO_POWERSHELL}"
            -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/room_titles_agent_test.ps1"
            -Config "$<CONFIG>" -BuildDir "${CMAKE_BINARY_DIR}")
        set_tests_properties(room_titles_agent_offline PROPERTIES TIMEOUT 90)
    endif()
    set(SDGO_ROOM_TITLE_NATIVE_HOST "" CACHE FILEPATH "Optional known BattleServer for bounded constructor fixture")
    if(SDGO_ROOM_TITLE_NATIVE_HOST)
        add_test(NAME room_title_native_offline COMMAND room_title_native_test "${SDGO_ROOM_TITLE_NATIVE_HOST}")
        set_tests_properties(room_title_native_offline PROPERTIES TIMEOUT 30)
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
    add_test(NAME collection_capsule_offline COMMAND "${Python3_EXECUTABLE}" -m unittest discover
        -s "${CMAKE_CURRENT_SOURCE_DIR}/tests" -p "test_collection*.py" -v)
    set_tests_properties(collection_capsule_offline PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 60)
    add_executable(collection_capsule_sql_test tests/collection_capsule_sql_test.cpp)
    target_link_libraries(collection_capsule_sql_test PRIVATE sdgo_api ole32 oleaut32 uuid)
    sdgo_strict(collection_capsule_sql_test)
endif()

# Battle AI component tests never enter a shipped payload.
add_executable(battle_ai_math_test tests/battle_ai_math_test.cpp)
sdgo_strict(battle_ai_math_test)
add_executable(battle_ai_capabilities_test tests/battle_ai_capabilities_test.cpp)
sdgo_strict(battle_ai_capabilities_test)
add_executable(battle_ai_navigation_test tests/battle_ai_navigation_test.cpp)
sdgo_strict(battle_ai_navigation_test)
add_executable(battle_ai_special_budget_test tests/battle_ai_special_budget_test.cpp)
sdgo_strict(battle_ai_special_budget_test)
add_executable(battle_ai_items_test tests/battle_ai_items_test.cpp)
sdgo_strict(battle_ai_items_test)
add_executable(battle_ai_loadout_test tests/battle_ai_loadout_test.cpp)
sdgo_strict(battle_ai_loadout_test)
add_executable(battle_ai_server_native_test tests/battle_ai_server_native_test.cpp)
target_link_libraries(battle_ai_server_native_test PRIVATE sdgo_api)
sdgo_strict(battle_ai_server_native_test)
add_dependencies(battle_ai_server_native_test battle_ai_server ai_catalog_server sdgo_core)
add_library(battle_ai_bridge_fixture SHARED tests/battle_ai_bridge_fixture.cpp)
sdgo_strict(battle_ai_bridge_fixture)
add_executable(match_ai_loadout_bridge_test tests/match_ai_loadout_bridge_test.cpp)
target_link_libraries(match_ai_loadout_bridge_test PRIVATE sdgo_api)
sdgo_strict(match_ai_loadout_bridge_test)
if(TARGET ai_test_lua50)
 add_executable(battle_ai_lua_test tools/lua50_host.c)
 target_link_libraries(battle_ai_lua_test PRIVATE ai_test_lua50)
 add_executable(battle_ai_native_test tests/battle_ai_native_test.cpp)
 target_link_libraries(battle_ai_native_test PRIVATE ai_test_lua50 sdgo_api)
 sdgo_strict(battle_ai_native_test)
endif()

if(BUILD_TESTING)
    foreach(component math capabilities navigation special_budget items loadout)
        add_test(NAME battle_ai_${component}_offline COMMAND battle_ai_${component}_test)
    endforeach()
    if(TARGET battle_ai_lua_test)
        foreach(policy policy universal)
            add_test(NAME battle_ai_${policy}_offline
                COMMAND battle_ai_lua_test tests/battle_ai_${policy}.lua)
            set_tests_properties(battle_ai_${policy}_offline PROPERTIES
                WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" TIMEOUT 30)
        endforeach()
    endif()
endif()

# Independent hangar policy, native ABI and opt-in SQL fixtures; never staged.
if(BUILD_TESTING)
    add_executable(hangar_capacity_policy_test tests/hangar_capacity_policy_test.cpp)
    sdgo_strict(hangar_capacity_policy_test)
    add_test(NAME hangar_capacity_policy_offline COMMAND hangar_capacity_policy_test)
    set_tests_properties(hangar_capacity_policy_offline PROPERTIES TIMEOUT 30)
    add_executable(hangar_capacity_native_test tests/hangar_capacity_native_test.cpp)
    target_link_libraries(hangar_capacity_native_test PRIVATE sdgo_api ole32 oleaut32 uuid)
    sdgo_strict(hangar_capacity_native_test)
    add_test(NAME hangar_capacity_native_offline COMMAND hangar_capacity_native_test)
    set_tests_properties(hangar_capacity_native_offline PROPERTIES TIMEOUT 30)
    add_executable(hangar_capacity_sql_emit tests/hangar_capacity_sql_emit.cpp)
    sdgo_strict(hangar_capacity_sql_emit)
    add_executable(hangar_capacity_sql_test tests/hangar_capacity_sql_test.cpp)
    target_link_libraries(hangar_capacity_sql_test PRIVATE sdgo_api ole32 oleaut32 uuid)
    sdgo_strict(hangar_capacity_sql_test)
    target_compile_options(hangar_capacity_sql_test PRIVATE /Ob0)
endif()
