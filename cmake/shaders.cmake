# Shaders: Cg sources in shaders/, prebuilt .gxp binaries in shaders/gxp/, embedded as C arrays.
# With SCE_PSP2_SDK_DIR (official PSP2 SDK) the sources are compiled with psp2cgc and the result also refreshes
# shaders/gxp/, so the committed binaries follow the sources. Without it the committed binaries are used.

if(NOT DEFINED SCE_PSP2_SDK_DIR AND DEFINED ENV{SCE_PSP2_SDK_DIR})
  set(SCE_PSP2_SDK_DIR "$ENV{SCE_PSP2_SDK_DIR}" CACHE PATH "Official PSP2 SDK root (optional, provides psp2cgc)")
endif()

if(SCE_PSP2_SDK_DIR)
  find_program(PSP2CGC psp2cgc
    PATHS "${SCE_PSP2_SDK_DIR}/host_tools/bin"
    NO_DEFAULT_PATH)
  if(NOT PSP2CGC)
    message(FATAL_ERROR "psp2cgc not found under ${SCE_PSP2_SDK_DIR}/host_tools/bin")
  endif()
  message(STATUS "Shaders: compiled with ${PSP2CGC}")
else()
  message(STATUS "Shaders: prebuilt binaries from shaders/gxp")
endif()

set(NFSC_SHADER_BINARY_DIR "${CMAKE_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${NFSC_SHADER_BINARY_DIR}")

# nfsc_add_shader(<list-var> <name> <sce_vp_psp2|sce_fp_psp2> [SOURCE <file.cg>] [DEFINES <NAME=value>...])
# Adds <name>.gxp -> <name>_gxp.c (symbols shader_<name>, shader_<name>_size). SOURCE defaults to <name>.cg;
# DEFINES build a variant of a shared source.
function(nfsc_add_shader out_var name profile)
  cmake_parse_arguments(SHADER "" "SOURCE" "DEFINES" ${ARGN})
  if(NOT SHADER_SOURCE)
    set(SHADER_SOURCE "${name}.cg")
  endif()
  set(defines "")
  foreach(define ${SHADER_DEFINES})
    list(APPEND defines "-D${define}")
  endforeach()
  set(src "${CMAKE_SOURCE_DIR}/shaders/${SHADER_SOURCE}")
  set(prebuilt "${CMAKE_SOURCE_DIR}/shaders/gxp/${name}.gxp")
  set(csrc "${NFSC_SHADER_BINARY_DIR}/${name}_gxp.c")
  if(SCE_PSP2_SDK_DIR)
    set(gxp "${NFSC_SHADER_BINARY_DIR}/${name}.gxp")
    add_custom_command(
      OUTPUT "${gxp}"
      # Relative source name: psp2cgc hashes the source path string into the header (bytes 16-19), so the
      # output is then the same in every checkout.
      COMMAND "${PSP2CGC}" -profile ${profile} -O3 -Wperf ${defines} -o "${gxp}" "${SHADER_SOURCE}"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${gxp}" "${prebuilt}"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/shaders"
      DEPENDS "${src}"
      COMMENT "Compiling shader ${name}.cg (${profile})"
      VERBATIM)
  else()
    set(gxp "${prebuilt}")
    if(NOT EXISTS "${gxp}")
      message(FATAL_ERROR "Missing prebuilt shader ${gxp}; set SCE_PSP2_SDK_DIR to compile it.")
    endif()
  endif()
  add_custom_command(
    OUTPUT "${csrc}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=${gxp}" "-DOUTPUT=${csrc}" "-DSYMBOL=shader_${name}"
            -P "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
    DEPENDS "${gxp}" "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
    COMMENT "Embedding shader ${name}.gxp"
    VERBATIM)
  list(APPEND ${out_var} "${csrc}")
  set(${out_var} "${${out_var}}" PARENT_SCOPE)
endfunction()

# nfsc_add_fragment_shader(<list-var> <name> [SOURCE <file.cg>] [DEFINES <NAME=value>...])
# A fragment program and its alpha-tested variant (-DATEST, suffix _atest).
function(nfsc_add_fragment_shader out_var name)
  cmake_parse_arguments(SHADER "" "SOURCE" "DEFINES" ${ARGN})
  if(NOT SHADER_SOURCE)
    set(SHADER_SOURCE "${name}.cg")
  endif()
  set(list "${${out_var}}")
  nfsc_add_shader(list ${name} sce_fp_psp2 SOURCE ${SHADER_SOURCE} DEFINES ${SHADER_DEFINES})
  nfsc_add_shader(list ${name}_atest sce_fp_psp2 SOURCE ${SHADER_SOURCE} DEFINES ${SHADER_DEFINES} ATEST=1)
  set(${out_var} "${list}" PARENT_SCOPE)
endfunction()

# Every program of the port: the present and clear programs, the fixed-function programs and the effect shader
# translations (include/platform/render.h); fragment programs also with -DATEST (alpha test, suffix _atest).
function(nfsc_add_shaders out_var)
  set(list "")
  nfsc_add_shader(list blit_v  sce_vp_psp2)
  nfsc_add_shader(list blit_f  sce_fp_psp2)
  nfsc_add_shader(list blit_ramp_f sce_fp_psp2)
  nfsc_add_shader(list clear_v sce_vp_psp2)
  nfsc_add_shader(list clear_f sce_fp_psp2)
  nfsc_add_shader(list clearz_v sce_vp_psp2)
  nfsc_add_shader(list ff_v sce_vp_psp2)
  nfsc_add_shader(list ff_screen_v sce_vp_psp2 SOURCE ff_v.cg DEFINES SCREEN=1)
  foreach(variant color tex proj)
    string(TOUPPER ${variant} define)
    set(defines "")
    if(NOT variant STREQUAL "color")
      set(defines ${define}=1)
    endif()
    nfsc_add_shader(list ff_${variant}_f sce_fp_psp2 SOURCE ff_f.cg DEFINES ${defines})
    nfsc_add_shader(list ff_${variant}_f_atest sce_fp_psp2 SOURCE ff_f.cg DEFINES ${defines} ATEST=1)
  endforeach()
  foreach(name fe_v femask_v filter_v screenfilter_v pip_v screen_v car_v glass_v glasslow_v carnm_v ucap_v world_v
               worldlow_v prelit_v sky_v worldnm_v road_v worldbone_v instance_v water_v particle_v streak_v flare_v
               raindrop_v ghost_v rvm_v)
    nfsc_add_shader(list ${name} sce_vp_psp2)
  endforeach()
  foreach(name fe_f femask_f pip_f vt_f passthru_f uves_f uvesdarken_f blend_f combine_f car_f glass_f glasslow_f
               carnm_f ucap_f world_f modulate_f prelit_f worldnm_f road_f texture_f color_f water_f particle_f
               streak_f flare_f raindrop_f ghost_f vtx_f)
    nfsc_add_fragment_shader(list ${name})
  endforeach()
  nfsc_add_fragment_shader(list fefog_f SOURCE fe_f.cg DEFINES FOG=1)
  nfsc_add_fragment_shader(list rvm_f SOURCE pip_f.cg DEFINES RVM=1)
  nfsc_add_shader(list caster_v sce_vp_psp2)
  nfsc_add_shader(list casterdepth_v sce_vp_psp2 SOURCE caster_v.cg DEFINES DEPTH=1)
  nfsc_add_fragment_shader(list castergreen_f SOURCE caster_f.cg DEFINES GREEN=1)
  nfsc_add_fragment_shader(list casterblack_f SOURCE caster_f.cg)
  nfsc_add_fragment_shader(list casterdepth_f SOURCE caster_f.cg DEFINES DEPTH=1)
  nfsc_add_shader(list depth_v sce_vp_psp2 DEFINES WORLD=1)
  nfsc_add_shader(list depthskinned_v sce_vp_psp2 SOURCE depth_v.cg DEFINES SKINNED=1)
  nfsc_add_fragment_shader(list depthtexture_f SOURCE caster_f.cg DEFINES DEPTH=1 TEXTURE=1)
  nfsc_add_shader(list motionblur_v sce_vp_psp2)
  nfsc_add_fragment_shader(list motionblur_f)
  nfsc_add_fragment_shader(list composite_f)
  foreach(name carnm1_v glass1_v road1_v)
    nfsc_add_shader(list ${name} sce_vp_psp2)
  endforeach()
  nfsc_add_shader(list flare1_v sce_vp_psp2 SOURCE flare_v.cg DEFINES SIMPLE=1)
  foreach(name car1_f carnm1_f glass1_f vt1_f particle1_f road1_f)
    nfsc_add_fragment_shader(list ${name})
  endforeach()
  nfsc_add_fragment_shader(list pip1_f SOURCE view1_f.cg)
  nfsc_add_fragment_shader(list rvm1_f SOURCE view1_f.cg DEFINES RVM=1)
  nfsc_add_fragment_shader(list opaque_f SOURCE caster_f.cg DEFINES OPAQUE=1)
  nfsc_add_shader(list world1_v sce_vp_psp2 DEFINES OFFSET=1)
  nfsc_add_shader(list worldnm1_v sce_vp_psp2 SOURCE world1_v.cg)
  nfsc_add_shader(list ucap1_v sce_vp_psp2)
  nfsc_add_fragment_shader(list textureopaque_f SOURCE texture_f.cg DEFINES OPAQUE=1)
  foreach(taps 1 2 3 4)
    nfsc_add_fragment_shader(list filter${taps}_f SOURCE filter_f.cg DEFINES TAPS=${taps})
  endforeach()
  nfsc_add_fragment_shader(list filterlum_f SOURCE filter_f.cg DEFINES TAPS=4 LUM=1)
  nfsc_add_fragment_shader(list filterpremul_f SOURCE filter_f.cg DEFINES TAPS=4 PREMUL=1)
  nfsc_add_fragment_shader(list screenfilter_f)
  nfsc_add_fragment_shader(list screenfiltergain_f SOURCE screenfilter_f.cg DEFINES GAIN=1)
  nfsc_add_fragment_shader(list down2_f)
  nfsc_add_fragment_shader(list down2mb_f SOURCE down2_f.cg DEFINES MOTIONBLUR=1)
  nfsc_add_fragment_shader(list down4_f)
  nfsc_add_fragment_shader(list downbloom_f SOURCE down4_f.cg DEFINES BLOOM=1)
  nfsc_add_fragment_shader(list down4tent_f SOURCE down4_f.cg DEFINES TENT=1)
  nfsc_add_fragment_shader(list downbloomtent_f SOURCE down4_f.cg DEFINES BLOOM=1 TENT=1)
  nfsc_add_fragment_shader(list vtnearest_f SOURCE vt_f.cg DEFINES NEAREST=1)
  nfsc_add_fragment_shader(list vtxnearest_f SOURCE vtx_f.cg DEFINES NEAREST=1)
  nfsc_add_fragment_shader(list gauss_f SOURCE blur_f.cg DEFINES TAPS=13)
  nfsc_add_fragment_shader(list blur15_f SOURCE blur_f.cg DEFINES TAPS=15)
  # The vertex programs with the lit power behind a branch (-DLIT_BRANCH, suffix _branch; setting vertex_branchless=0).
  foreach(name car_v glass_v glasslow_v carnm_v world_v worldlow_v prelit_v worldnm_v road_v carnm1_v glass1_v road1_v)
    nfsc_add_shader(list ${name}_branch sce_vp_psp2 SOURCE ${name}.cg DEFINES LIT_BRANCH=1)
  endforeach()
  nfsc_add_shader(list world1_v_branch sce_vp_psp2 SOURCE world1_v.cg DEFINES OFFSET=1 LIT_BRANCH=1)
  nfsc_add_shader(list worldnm1_v_branch sce_vp_psp2 SOURCE world1_v.cg DEFINES LIT_BRANCH=1)
  set(${out_var} "${list}" PARENT_SCOPE)
endfunction()
