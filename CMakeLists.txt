cmake_minimum_required(VERSION 3.21)
project(utmtftm VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)

if(WIN32)
  set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)

  # Installed by vcpkg from vcpkg.json (triplet x64-windows-static).
  set(DEPS ${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows-static)

  add_compile_definitions(_USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS CAIRO_WIN32_STATIC_BUILD)
  add_compile_options(/utf-8 /W3)
  include_directories(src third_party/openvr/headers ${DEPS}/include ${DEPS}/include/cairo)
  link_directories(third_party/openvr/lib ${DEPS}/lib)

  set(PLATFORM_SRC src/platform_windows.cpp)
  set(UPLOAD_SRC src/d3dupload.cpp)
  set(UPLOAD_LIBS d3d11 dxgi)
  set(CAIRO_LIBS cairo pixman-1 libpng16 zs pthreadVC3 gdi32 msimg32 dwrite d2d1 windowscodecs user32 ole32)
  set(JSON_LIBS jsoncpp shell32 advapi32)
  set(THREAD_LIBS "")
  set(DRIVER_DIR ${CMAKE_SOURCE_DIR}/dist/driver/bin/win64)
else()
  if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    message(FATAL_ERROR "The Linux build is for the Steam Frame, which is ARM64: third_party/openvr only has the library for that.")
  endif()

  find_package(PkgConfig REQUIRED)
  find_package(Threads REQUIRED)
  pkg_check_modules(CAIRO REQUIRED cairo)
  pkg_check_modules(FREETYPE REQUIRED freetype2)
  pkg_check_modules(VULKAN REQUIRED vulkan)
  pkg_check_modules(JSONCPP REQUIRED jsoncpp)
  find_library(JSONCPP_STATIC libjsoncpp.a REQUIRED)

  # cairo, FreeType, Vulkan and the C library come from SteamOS. Everything else is inside the program, so it doesn't
  # matter which compiler version built it or what's installed on the headset. -fsigned-char: plain char is unsigned on
  # ARM and signed on x86, and this keeps it the way the Windows build has been tested.
  add_compile_options(-Wall -Wno-unused-parameter -fsigned-char -fvisibility=hidden)
  add_link_options(-static-libstdc++ -static-libgcc -Wl,--as-needed -Wl,--disable-new-dtags)
  include_directories(src third_party/openvr/headers ${CAIRO_INCLUDE_DIRS} ${FREETYPE_INCLUDE_DIRS} ${VULKAN_INCLUDE_DIRS} ${JSONCPP_INCLUDE_DIRS})
  link_directories(third_party/openvr/bin/linuxarm64)

  # OpenVR's loader sits next to the program. If that copy goes missing, SteamVR on the Frame has its own.
  set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
  set(CMAKE_INSTALL_RPATH "\$ORIGIN:/opt/steamvr/bin/linuxarm64")

  set(PLATFORM_SRC src/platform_linux.cpp)
  set(UPLOAD_SRC src/vkupload.cpp)
  set(UPLOAD_LIBS ${VULKAN_LIBRARIES})
  set(CAIRO_LIBS ${CAIRO_LIBRARIES} ${FREETYPE_LIBRARIES})
  set(JSON_LIBS ${JSONCPP_STATIC})
  set(THREAD_LIBS Threads::Threads ${CMAKE_DL_LIBS})
  set(DRIVER_DIR ${CMAKE_SOURCE_DIR}/dist/driver/bin/linuxarm64)
endif()

set(COMMON_LIBS openvr_api ${JSON_LIBS})

# The SteamVR driver add-on: moves the hand controllers for every game, from inside SteamVR.
add_library(driver_utmtftm SHARED src/driver/driver_main.cpp)
set_target_properties(driver_utmtftm PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${DRIVER_DIR} LIBRARY_OUTPUT_DIRECTORY ${DRIVER_DIR} PREFIX "")
if(NOT WIN32)
  # It lives inside SteamVR, so nothing of ours may be visible to the libraries already in there.
  target_link_options(driver_utmtftm PRIVATE -Wl,-z,defs -Wl,--exclude-libs,ALL)
endif()

add_executable(utmtftm src/main.cpp src/store.cpp ${PLATFORM_SRC} src/ui.cpp src/glove.cpp ${UPLOAD_SRC})
target_link_libraries(utmtftm PRIVATE ${COMMON_LIBS} ${CAIRO_LIBS} ${UPLOAD_LIBS})
add_custom_command(TARGET utmtftm POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/input ${CMAKE_SOURCE_DIR}/dist/input
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/driver ${CMAKE_SOURCE_DIR}/dist/driver
  VERBATIM)
if(WIN32)
  # WIN32 + mainCRTStartup: no console window when SteamVR starts it, and main() stays main().
  set_target_properties(utmtftm PROPERTIES WIN32_EXECUTABLE TRUE)
  target_link_options(utmtftm PRIVATE /ENTRY:mainCRTStartup)
  add_custom_command(TARGET utmtftm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/openvr/bin/openvr_api.dll ${CMAKE_SOURCE_DIR}/dist/
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/utmtftm.vrmanifest ${CMAKE_SOURCE_DIR}/install.cmd ${CMAKE_SOURCE_DIR}/uninstall.cmd ${CMAKE_SOURCE_DIR}/dist/
    VERBATIM)
else()
  # The dashboard's text. Windows has its own font for this.
  add_custom_command(TARGET utmtftm POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_SOURCE_DIR}/third_party/openvr/bin/linuxarm64/libopenvr_api.so ${CMAKE_SOURCE_DIR}/dist/
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/third_party/fonts/Barlow ${CMAKE_SOURCE_DIR}/dist/fonts
    VERBATIM)
endif()
add_custom_command(TARGET utmtftm POST_BUILD
  COMMAND $<TARGET_FILE:utmtftm> --write-icon ${CMAKE_SOURCE_DIR}/dist/utmtftm.png
  VERBATIM)
add_dependencies(utmtftm driver_utmtftm)

# Offline tests: no headset, no SteamVR.
add_executable(test_ghost tests/test_ghost.cpp)

add_executable(test_ui tests/test_ui.cpp src/ui.cpp ${PLATFORM_SRC} src/glove.cpp)
target_link_libraries(test_ui PRIVATE ${JSON_LIBS} ${CAIRO_LIBS})
add_dependencies(test_ui utmtftm)

add_executable(test_session tests/test_session.cpp src/store.cpp ${PLATFORM_SRC})
target_link_libraries(test_session PRIVATE ${COMMON_LIBS})
add_dependencies(test_session utmtftm)

add_executable(test_driver tests/test_driver.cpp)
target_link_libraries(test_driver PRIVATE ${THREAD_LIBS})
add_dependencies(test_driver driver_utmtftm)

add_executable(test_controllers tests/test_controllers.cpp ${PLATFORM_SRC})
target_link_libraries(test_controllers PRIVATE ${JSON_LIBS})
add_dependencies(test_controllers utmtftm)

if(WIN32)
  # Review tool for the hand on every controller SteamVR can draw. Needs SteamVR installed.
  add_executable(glove_sheet tests/glove_sheet.cpp src/ui.cpp src/glove.cpp ${PLATFORM_SRC})
  target_link_libraries(glove_sheet PRIVATE ${COMMON_LIBS} ${CAIRO_LIBS})
  add_dependencies(glove_sheet utmtftm)
endif()
