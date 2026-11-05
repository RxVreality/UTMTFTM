cmake_minimum_required(VERSION 3.21)
project(utmtftm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)

# Installed by vcpkg from vcpkg.json (triplet x64-windows-static).
set(DEPS ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows-static)

add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS CAIRO_WIN32_STATIC_BUILD)
add_compile_options(/utf-8 /W3)
include_directories(src third_party/openvr/headers ${DEPS}/include ${DEPS}/include/cairo)
link_directories(third_party/openvr/lib ${DEPS}/lib)

set(CAIRO_LIBS cairo pixman-1 libpng16 zs pthreadVC3 gdi32 msimg32 dwrite d2d1 windowscodecs user32 ole32)
set(COMMON_LIBS openvr_api jsoncpp shell32 advapi32)

# The SteamVR driver add-on: moves the hand controllers for every game, from inside SteamVR.
add_library(driver_utmtftm SHARED src/driver/driver_main.cpp)
set_target_properties(driver_utmtftm PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist/driver/bin/win64)

# WIN32 + mainCRTStartup: no console window when SteamVR starts it, and main() stays main().
add_executable(utmtftm WIN32 src/main.cpp src/store.cpp src/platform.cpp src/ui.cpp src/glove.cpp src/d3dupload.cpp)
target_link_libraries(utmtftm PRIVATE ${COMMON_LIBS} ${CAIRO_LIBS} d3d11 dxgi)
target_link_options(utmtftm PRIVATE /ENTRY:mainCRTStartup)
add_custom_command(TARGET utmtftm POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/openvr/bin/openvr_api.dll ${CMAKE_SOURCE_DIR}/dist/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/utmtftm.vrmanifest ${CMAKE_SOURCE_DIR}/install.cmd ${CMAKE_SOURCE_DIR}/uninstall.cmd ${CMAKE_SOURCE_DIR}/dist/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/input ${CMAKE_SOURCE_DIR}/dist/input
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/driver ${CMAKE_SOURCE_DIR}/dist/driver
  COMMAND ${CMAKE_SOURCE_DIR}/dist/utmtftm.exe --write-icon ${CMAKE_SOURCE_DIR}/dist/utmtftm.png
  VERBATIM)
add_dependencies(utmtftm driver_utmtftm)

# Offline tests: no headset, no SteamVR.
add_executable(test_ghost tests/test_ghost.cpp)

add_executable(test_ui tests/test_ui.cpp src/ui.cpp src/platform.cpp src/glove.cpp)
target_link_libraries(test_ui PRIVATE jsoncpp shell32 advapi32 ${CAIRO_LIBS})

add_executable(test_session tests/test_session.cpp src/store.cpp src/platform.cpp)
target_link_libraries(test_session PRIVATE ${COMMON_LIBS})

add_executable(test_driver tests/test_driver.cpp)
add_dependencies(test_driver driver_utmtftm)

add_executable(test_controllers tests/test_controllers.cpp src/platform.cpp)
target_link_libraries(test_controllers PRIVATE jsoncpp shell32 advapi32)
add_dependencies(test_controllers utmtftm)

# Review tool for the hand on every controller SteamVR can draw. Needs SteamVR installed.
add_executable(glove_sheet tests/glove_sheet.cpp src/ui.cpp src/glove.cpp src/platform.cpp)
target_link_libraries(glove_sheet PRIVATE ${COMMON_LIBS} ${CAIRO_LIBS})
add_dependencies(glove_sheet utmtftm)
