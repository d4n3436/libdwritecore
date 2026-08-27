add_library(chromium_vtable_patch SHARED
  src/vtable_patch.cpp
  src/windows_path.cpp
  src/typeface_bridge.cpp
  src/font_facts.cpp
  src/dwrite_raster.cpp
  src/parity_gate.cpp
  src/render_params.cpp
  src/render_params_patch.cpp
  src/fallback_order.cpp
  src/hook_thunk.S)

target_include_directories(chromium_vtable_patch
  PRIVATE "${CMAKE_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_SOURCE_DIR}/cleartype/src")

target_compile_definitions(chromium_vtable_patch PRIVATE DWRITE_CORE=1)

target_link_libraries(chromium_vtable_patch PRIVATE ${CMAKE_DL_LIBS})

set_target_properties(chromium_vtable_patch PROPERTIES
  OUTPUT_NAME chromium-vtable-patch
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
