cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

cmake_policy(SET CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "Build command-line programs" FORCE)
set(
  FDK_AAC_INSTALL_CMAKE_CONFIG_MODULE
  OFF
  CACHE BOOL
  "Install FDK AAC CMake package configuration"
  FORCE
)
set(
  FDK_AAC_INSTALL_PKGCONFIG_MODULE
  OFF
  CACHE BOOL
  "Install FDK AAC pkg-config module"
  FORCE
)
set(
  OBS_AIRPLAY_DEPS_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/source"
  CACHE PATH
  "Directory containing fetched FDK AAC and doubletake source trees"
)
set(FDK_AAC_SOURCE_DIR "${OBS_AIRPLAY_DEPS_DIR}/fdk-aac")
set(DOUBLETAKE_SOURCE_DIR "${OBS_AIRPLAY_DEPS_DIR}/doubletake")
foreach(
  dependency_dir
  IN ITEMS
    "${FDK_AAC_SOURCE_DIR}"
    "${DOUBLETAKE_SOURCE_DIR}"
    "${DOUBLETAKE_SOURCE_DIR}/cmd/obsairplay-bridge"
)
  if(NOT EXISTS "${dependency_dir}")
    message(
      FATAL_ERROR
      "Missing dependency: ${dependency_dir}\n"
      "Run scripts/fetch-dependencies.ps1 before configuring the project."
    )
  endif()
endforeach()
add_subdirectory(
  "${FDK_AAC_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/fdk-aac"
  EXCLUDE_FROM_ALL
)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ws2_32)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

find_program(GO_EXECUTABLE go REQUIRED)
set(AIRPLAY_BRIDGE "${CMAKE_CURRENT_BINARY_DIR}/obs-airplay-cast-bridge.exe")
add_custom_target(
  obs-airplay-bridge ALL
  COMMAND
    "${CMAKE_COMMAND}" -E env CGO_ENABLED=0 "${GO_EXECUTABLE}" build -trimpath "-ldflags=-s -w"
    -o "${AIRPLAY_BRIDGE}" ./cmd/obsairplay-bridge
  WORKING_DIRECTORY "${DOUBLETAKE_SOURCE_DIR}"
  BYPRODUCTS "${AIRPLAY_BRIDGE}"
  COMMENT "Building OBS AirPlay bridge"
  VERBATIM
)
add_dependencies(${CMAKE_PROJECT_NAME} obs-airplay-bridge)

set(AIRPLAY_ELD_ENCODER "${CMAKE_CURRENT_BINARY_DIR}/obs-airplay-aac-eld.exe")
add_executable(obs-airplay-aac-eld src/aac-eld-encoder.cpp)
target_link_libraries(obs-airplay-aac-eld PRIVATE fdk-aac)
target_compile_features(obs-airplay-aac-eld PRIVATE cxx_std_17)
target_compile_definitions(obs-airplay-aac-eld PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(
  obs-airplay-aac-eld
  PROPERTIES
    OUTPUT_NAME "obs-airplay-aac-eld"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_dependencies(${CMAKE_PROJECT_NAME} obs-airplay-aac-eld)

add_executable(obs-airplay-aac-eld-probe EXCLUDE_FROM_ALL src/aac-eld-probe.cpp)
target_link_libraries(obs-airplay-aac-eld-probe PRIVATE fdk-aac)
target_compile_features(obs-airplay-aac-eld-probe PRIVATE cxx_std_17)
target_compile_definitions(obs-airplay-aac-eld-probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(
  obs-airplay-aac-eld-probe
  PROPERTIES
    OUTPUT_NAME "obs-airplay-aac-eld-probe"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/airplay-cast.cpp
    src/airplay-cast.hpp
    src/airplay-bridge.cpp
    src/airplay-bridge.hpp
)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})

add_custom_command(
  TARGET ${CMAKE_PROJECT_NAME}
  POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${AIRPLAY_BRIDGE}" "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:obs-airplay-aac-eld>" "$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${AIRPLAY_BRIDGE}" "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:obs-airplay-aac-eld>" "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
  COMMENT "Copy AirPlay bridge and AAC-ELD encoder next to the OBS plugin"
  VERBATIM
)

install(FILES "${AIRPLAY_BRIDGE}" DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit")
install(FILES "$<TARGET_FILE:obs-airplay-aac-eld>" DESTINATION "${CMAKE_PROJECT_NAME}/bin/64bit")
install(
  FILES
    README.md
    LICENSE
    THIRD_PARTY_NOTICES.md
  DESTINATION "${CMAKE_PROJECT_NAME}"
)
install(
  DIRECTORY licenses/
  DESTINATION "${CMAKE_PROJECT_NAME}/licenses"
)
