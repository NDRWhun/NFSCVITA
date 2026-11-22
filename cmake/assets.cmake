# Button prompt glyphs: texture images in assets/prompts/ (A8R8G8B8 .argb, DXT5 .dxt5 with their mip levels),
# embedded as C arrays (symbols prompt_<name>_<ext>, prompt_<name>_<ext>_size).

set(NFSC_ASSET_BINARY_DIR "${CMAKE_BINARY_DIR}/assets")
file(MAKE_DIRECTORY "${NFSC_ASSET_BINARY_DIR}")

# nfsc_add_prompt_glyphs(<list-var>): one generated C file per glyph image.
function(nfsc_add_prompt_glyphs out_var)
  set(list "")
  file(GLOB glyphs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/prompts/*.argb"
                                     "${CMAKE_SOURCE_DIR}/assets/prompts/*.dxt5")
  foreach(glyph ${glyphs})
    get_filename_component(name "${glyph}" NAME_WE)
    get_filename_component(ext "${glyph}" EXT)
    string(SUBSTRING "${ext}" 1 -1 ext)
    set(csrc "${NFSC_ASSET_BINARY_DIR}/prompt_${name}_${ext}.c")
    add_custom_command(
      OUTPUT "${csrc}"
      COMMAND "${CMAKE_COMMAND}" "-DINPUT=${glyph}" "-DOUTPUT=${csrc}" "-DSYMBOL=prompt_${name}_${ext}"
              -P "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
      DEPENDS "${glyph}" "${CMAKE_SOURCE_DIR}/cmake/bin2c.cmake"
      COMMENT "Embedding prompt glyph ${name}.${ext}"
      VERBATIM)
    list(APPEND list "${csrc}")
  endforeach()
  set(${out_var} "${list}" PARENT_SCOPE)
endfunction()
