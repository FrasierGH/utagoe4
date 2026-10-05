cmake_minimum_required(VERSION 3.16)
project(utagoe VERSION 3.0.0 LANGUAGES CXX RC)

if(NOT WIN32)
  message(FATAL_ERROR "Utagoe Rip is a Windows program")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(MSVC)
  add_compile_options(/utf-8 /W3 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")  # static CRT: no runtime DLLs
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -finput-charset=UTF-8 -fexec-charset=UTF-8)
  add_link_options(-static -s)  # self-contained exe: no libstdc++/libgcc/winpthread DLLs
endif()
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

# --- processing engine
add_library(utagoe_engine STATIC
  src/engine/settings.cpp
  src/engine/wav.cpp
  src/engine/dsp.cpp
  src/engine/engine.cpp
  src/engine/v4.cpp)
target_include_directories(utagoe_engine PUBLIC src)

# --- tests (ctest)
enable_testing()
add_executable(engine_test tests/engine_test.cpp)
target_link_libraries(engine_test PRIVATE utagoe_engine)
if(NOT MSVC)
  target_link_options(engine_test PRIVATE -municode)
endif()
add_test(NAME engine COMMAND engine_test)

# the Utagoe Rip 4 separation from the command line (used by eval/run.py, engine "v4-cpp")
add_executable(v4_cli tests/v4_cli.cpp)
target_link_libraries(v4_cli PRIVATE utagoe_engine)
if(NOT MSVC)
  target_link_options(v4_cli PRIVATE -municode)
endif()

# --- the program
add_executable(utagoe WIN32
  src/app/main.cpp
  src/app/main_form.cpp
  src/app/settings_form.cpp
  src/app/play_form.cpp
  src/app/about_form.cpp
  src/app/ui.cpp
  src/app/lang.cpp
  src/app/logic.cpp
  src/app/utagoe.rc)
target_link_libraries(utagoe PRIVATE utagoe_engine comctl32 uxtheme msimg32 winmm shell32 comdlg32 gdi32 user32)
target_include_directories(utagoe PRIVATE src/app)
if(MSVC)
  # utagoe.rc embeds src/app/utagoe.manifest; subsystem 5.02 (as MinGW) keeps the classic window frame
  # metrics of the original instead of the padded border Windows gives 6.0+ executables
  target_link_options(utagoe PRIVATE /MANIFEST:NO /SUBSYSTEM:WINDOWS,5.02)
else()
  target_link_options(utagoe PRIVATE -municode)
endif()
