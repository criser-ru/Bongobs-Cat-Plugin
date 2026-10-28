# Standalone build of Bongobs Cat for Windows x64.
# Builds against the libobs headers (OBS source) and an obs.lib import library
# made from obs.dll of the installed OBS version. See .github/workflows/build.yml.
#
#   cmake -B build -G "Visual Studio 17 2022" -A x64
#         -DOBS_SOURCE_DIR=<path to obs-studio source> -DOBS_LIB=<path to obs.lib>
#         -DCMAKE_POLICY_VERSION_MINIMUM=3.5
#   cmake --build build --config Release
#   cmake --install build --config Release --prefix <package dir>

cmake_minimum_required(VERSION 3.16)

set(APP_NAME bongobs-cat)
project(${APP_NAME} C CXX)

set(OBS_SOURCE_DIR "" CACHE PATH "Path to obs-studio source (for libobs headers)")
set(OBS_LIB "" CACHE FILEPATH "Path to obs.lib (import library for obs.dll)")

if(NOT MSVC)
  message(FATAL_ERROR "[${APP_NAME}] Only Visual Studio on Windows is supported.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "[${APP_NAME}] Only 64-bit builds are supported (use -A x64).")
endif()
if(NOT EXISTS "${OBS_SOURCE_DIR}/libobs/obs.h")
  message(FATAL_ERROR "[${APP_NAME}] OBS_SOURCE_DIR does not point to obs-studio source.")
endif()
if(NOT EXISTS "${OBS_LIB}")
  message(FATAL_ERROR "[${APP_NAME}] OBS_LIB (obs.lib) not found.")
endif()

# Directory paths.
set(SDK_ROOT_PATH ${CMAKE_CURRENT_SOURCE_DIR})
set(CORE_PATH ${SDK_ROOT_PATH}/CubismSdk/Core)
set(FRAMEWORK_PATH ${SDK_ROOT_PATH}/CubismSdk/Framework)
set(THIRD_PARTY_PATH ${SDK_ROOT_PATH}/thirdParty)
set(STB_PATH ${THIRD_PARTY_PATH}/stb)
set(GLEW_PATH ${THIRD_PARTY_PATH}/glew)
set(GLFW_PATH ${THIRD_PARTY_PATH}/glfw)

set(ARCH x86_64)

# The bundled Live2D Cubism Core has static libraries up to VS2019 (v142).
# Static libraries built with v142 link fine with VS2022 (v143).
if(MSVC_VERSION GREATER_EQUAL 1920)
  set(COMPILER 142)
elseif(MSVC_VERSION GREATER_EQUAL 1910)
  set(COMPILER 141)
else()
  message(FATAL_ERROR "[${APP_NAME}] Visual Studio 2017 or newer is required.")
endif()

# OBS plugins use the DLL C runtime (/MD), so Live2D Core must match.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CRT MD)

# Live2D Cubism Core (static).
add_library(Live2DCubismCore STATIC IMPORTED)
set(CORE_LIB_SUFFIX ${CORE_PATH}/lib/windows/${ARCH}/${COMPILER})
set_target_properties(Live2DCubismCore
  PROPERTIES
    IMPORTED_LOCATION_DEBUG ${CORE_LIB_SUFFIX}/Live2DCubismCore_${CRT}d.lib
    IMPORTED_LOCATION_RELWITHDEBINFO ${CORE_LIB_SUFFIX}/Live2DCubismCore_${CRT}.lib
    IMPORTED_LOCATION_RELEASE ${CORE_LIB_SUFFIX}/Live2DCubismCore_${CRT}.lib
    IMPORTED_LOCATION_MINSIZEREL ${CORE_LIB_SUFFIX}/Live2DCubismCore_${CRT}.lib
    INTERFACE_INCLUDE_DIRECTORIES ${CORE_PATH}/include
)

# GLEW and GLFW (bundled).
set(BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(${GLEW_PATH}/build/cmake ${CMAKE_CURRENT_BINARY_DIR}/glew EXCLUDE_FROM_ALL)
add_subdirectory(${GLFW_PATH} ${CMAKE_CURRENT_BINARY_DIR}/glfw EXCLUDE_FROM_ALL)

# Live2D Cubism Native Framework with OpenGL rendering.
set(FRAMEWORK_SOURCE OpenGL)
add_subdirectory(${FRAMEWORK_PATH} ${CMAKE_CURRENT_BINARY_DIR}/Framework)
target_compile_definitions(Framework PUBLIC CSM_TARGET_WIN_GL)
target_include_directories(Framework PUBLIC ${GLEW_PATH}/include)
target_link_libraries(Framework Live2DCubismCore glew_s)

find_package(OpenGL REQUIRED)

# libobs: headers from OBS source + generated obsconfig.h, linked to obs.lib.
set(OBS_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/obs-generated)
file(WRITE ${OBS_GENERATED_DIR}/obsconfig.h
"#pragma once
#define OBS_DATA_PATH \"../../data\"
#define OBS_PLUGIN_PATH \"../../obs-plugins/64bit\"
#define OBS_PLUGIN_DESTINATION \"obs-plugins/64bit\"
#define OBS_INSTALL_PREFIX \"\"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
")
add_library(libobs SHARED IMPORTED)
set_target_properties(libobs
  PROPERTIES
    IMPORTED_IMPLIB ${OBS_LIB}
    INTERFACE_INCLUDE_DIRECTORIES "${OBS_SOURCE_DIR}/libobs;${OBS_GENERATED_DIR}"
)

set(${APP_NAME}_SOURCES
  vtuber_plugin.cpp
  VtuberPlugin.cpp
  VtuberFrameWork.cpp
  VtuberDelegate.cpp
  View.cpp
  Live2DManager.cpp
  Model.cpp
  Pal.cpp
  LAppTextureManager.cpp
  LAppAllocator.cpp
  Sprite.cpp
  Hook.cpp
  EventManager.cpp
  Define.cpp
  jsoncpp.cpp
  InfoReader.cpp
  MouthSync.cpp
)

set(${APP_NAME}_HEADERS
  LAppAllocator.hpp
  Define.hpp
  VtuberDelegate.hpp
  Live2DManager.hpp
  LAppTextureManager.hpp
  Model.hpp
  Pal.hpp
  View.hpp
  VtuberFrameWork.hpp
  VtuberPlugin.hpp
  Sprite.hpp
  Hook.hpp
  EventManager.hpp
  InfoReader.hpp
  MouthSync.hpp
  json.h
  json-forwards.h
)

add_library(${APP_NAME} MODULE ${${APP_NAME}_SOURCES} ${${APP_NAME}_HEADERS})

target_compile_features(${APP_NAME} PRIVATE cxx_std_17)
# Source files contain Chinese/Japanese comments; read them as UTF-8.
target_compile_options(${APP_NAME} PRIVATE /utf-8)
target_compile_options(Framework PRIVATE /utf-8)
target_compile_definitions(${APP_NAME} PRIVATE _CRT_SECURE_NO_WARNINGS)

target_link_libraries(${APP_NAME}
  libobs
  Framework
  glfw
  ${OPENGL_LIBRARIES}
)
target_include_directories(${APP_NAME} PRIVATE ${STB_PATH})

set_target_properties(${APP_NAME} PROPERTIES PREFIX "" OUTPUT_NAME ${APP_NAME})

# Package layout matches the OBS folder: unzip the result into the OBS root.
install(TARGETS ${APP_NAME} LIBRARY DESTINATION obs-plugins/64bit RUNTIME DESTINATION obs-plugins/64bit)
install(DIRECTORY data/ DESTINATION data/obs-plugins/${APP_NAME})
