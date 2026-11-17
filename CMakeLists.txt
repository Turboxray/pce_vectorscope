cmake_minimum_required(VERSION 3.22)
project(PceVectorscope VERSION 0.2.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(imgui STATIC
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_win32.cpp
  third_party/imgui/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi imm32)

# The vectorscope graticule image is compiled into the exe.
set(GRATICULE_PNG ${CMAKE_SOURCE_DIR}/assets/vector_scope_overlay__v4.png)
set(GRATICULE_CPP ${CMAKE_BINARY_DIR}/generated/graticule_png.cpp)
add_custom_command(
  OUTPUT ${GRATICULE_CPP}
  COMMAND ${CMAKE_COMMAND} -DIN=${GRATICULE_PNG} -DOUT=${GRATICULE_CPP} -DNAME=graticule_png
          -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${GRATICULE_PNG} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  VERBATIM)

add_executable(pce_vectorscope WIN32
  src/main.cpp
  src/app.cpp
  src/vce_model.cpp
  src/stb_impl.cpp
  ${GRATICULE_CPP})
target_include_directories(pce_vectorscope PRIVATE third_party/stb)
target_compile_definitions(pce_vectorscope PRIVATE NOMINMAX UNICODE _UNICODE)
target_link_libraries(pce_vectorscope PRIVATE imgui comdlg32 shell32 gdi32 user32)
if(MSVC)
  target_compile_options(pce_vectorscope PRIVATE /W4)
else()
  set_source_files_properties(src/main.cpp src/app.cpp src/vce_model.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
endif()
if(MINGW)
  target_link_options(pce_vectorscope PRIVATE -municode -static)
endif()
