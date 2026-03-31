cmake_minimum_required(VERSION 3.20)
project(sdgo_mods LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Mods must be built as 32-bit (x86). Use -A Win32.")
endif()

set(MOD_API_DIR "${CMAKE_SOURCE_DIR}/../d3d9_proxy/src")
set(MODS_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/../../SDGO-client/mods")

# Helper function: define a mod DLL target
function(add_mod MOD_NAME)
    add_library(${MOD_NAME} SHARED ${MOD_NAME}/${MOD_NAME}.cpp)
    target_include_directories(${MOD_NAME} PRIVATE ${MOD_API_DIR})
    target_compile_definitions(${MOD_NAME} PRIVATE SDGO_MOD_BUILDING)
    set_target_properties(${MOD_NAME} PROPERTIES
        OUTPUT_NAME "${MOD_NAME}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MODS_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MODS_OUTPUT_DIR}"
    )
endfunction()

# ---- Core effects (moved from proxy built-in) ----
add_mod(mod_bloom)        # Priority 100: Bright area glow
add_mod(mod_ssao)         # Priority 200: Ambient occlusion
add_mod(mod_dof)          # Priority 250: Depth of field
add_mod(mod_fxaa)         # Priority 400: Anti-aliasing
add_mod(mod_sharpen)      # Priority 500: Contrast adaptive sharpening
add_mod(mod_colorgrade)   # Priority 650: Cinematic color grading

# ---- Cinematic mods ----
add_mod(mod_motionblur)   # Priority 300: Camera motion blur
add_mod(mod_godrays)      # Priority 400: Volumetric light scattering
add_mod(mod_lenseffects)  # Priority 600: CA + barrel + grain + flare
add_mod(mod_vignette)     # Priority 700: Screen edge darkening

# ---- Gameplay mods ----
add_mod(mod_capsule_event)  # Capsule machine 78 event UI injection
