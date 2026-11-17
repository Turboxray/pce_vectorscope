cmake_minimum_required(VERSION 3.22)
project(PceVectorscope VERSION 0.3.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Two programs come out of the same sources: the Windows exe (build.bat), and the page for a browser when CMake is run
# through Emscripten (build_web.bat). Only the host differs: src/main.cpp or src/main_web.cpp.
set(IMGUI_CORE
  third_party/imgui/imgui.cpp
  third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp)

# The vectorscope graticule image is compiled into the program.
set(GRATICULE_PNG ${CMAKE_SOURCE_DIR}/assets/vector_scope_overlay__v4.png)
set(GRATICULE_CPP ${CMAKE_BINARY_DIR}/generated/graticule_png.cpp)
add_custom_command(
  OUTPUT ${GRATICULE_CPP}
  COMMAND ${CMAKE_COMMAND} -DIN=${GRATICULE_PNG} -DOUT=${GRATICULE_CPP} -DNAME=graticule_png
          -P ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${GRATICULE_PNG} ${CMAKE_SOURCE_DIR}/cmake/embed.cmake
  VERBATIM)

set(APP_SOURCES
  src/app.cpp
  src/vce_model.cpp
  src/stb_impl.cpp
  ${GRATICULE_CPP})

if(EMSCRIPTEN)
  # The page: index.html, index.js and index.wasm. GLFW is Emscripten's contrib.glfw3 port; drawing is WebGL 2.
  add_library(imgui STATIC ${IMGUI_CORE}
    third_party/imgui/backends/imgui_impl_glfw.cpp
    third_party/imgui/backends/imgui_impl_opengl3.cpp)
  target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
  target_compile_options(imgui PUBLIC --use-port=contrib.glfw3)
  target_link_options(imgui PUBLIC --use-port=contrib.glfw3)

  add_executable(pce_vectorscope src/main_web.cpp ${APP_SOURCES})
  target_include_directories(pce_vectorscope PRIVATE third_party/stb)
  target_link_libraries(pce_vectorscope PRIVATE imgui)
  set(WEB_SHELL ${CMAKE_SOURCE_DIR}/web/shell.html)
  set(WEB_FONT ${CMAKE_SOURCE_DIR}/third_party/imgui/misc/fonts/DroidSans.ttf)
  target_link_options(pce_vectorscope PRIVATE
    -sMIN_WEBGL_VERSION=2
    -sMAX_WEBGL_VERSION=2
    -sALLOW_MEMORY_GROWTH=1
    -sSTACK_SIZE=1048576
    -sFORCE_FILESYSTEM=1
    --shell-file ${WEB_SHELL}
    --embed-file ${WEB_FONT}@/DroidSans.ttf)
  set_target_properties(pce_vectorscope PROPERTIES
    OUTPUT_NAME index
    SUFFIX .html
    LINK_DEPENDS "${WEB_SHELL};${WEB_FONT}")
  set_source_files_properties(src/main_web.cpp src/app.cpp src/vce_model.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")
else()
  add_library(imgui STATIC ${IMGUI_CORE}
    third_party/imgui/backends/imgui_impl_win32.cpp
    third_party/imgui/backends/imgui_impl_dx11.cpp)
  target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
  target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi imm32)

  add_executable(pce_vectorscope WIN32 src/main.cpp ${APP_SOURCES})
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
endif()
