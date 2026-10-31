cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})
include(CTest)

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

cmake_policy(SET CMP0077 NEW)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "Build command-line programs" FORCE)
set(
  OBS_AIRPLAY_DEPS_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/.deps/source"
  CACHE PATH
  "Directory containing fetched AAC encoder and doubletake source trees"
)
set(LIBXAAC_SOURCE_DIR "${OBS_AIRPLAY_DEPS_DIR}/libxaac")
set(DOUBLETAKE_SOURCE_DIR "${OBS_AIRPLAY_DEPS_DIR}/doubletake")
foreach(
  dependency_dir
  IN ITEMS
    "${LIBXAAC_SOURCE_DIR}"
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
  "${LIBXAAC_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}/libxaac"
  EXCLUDE_FROM_ALL
)
if(MSVC)
  # The pinned libxaac commit contains non-UTF-8 source bytes and warnings that
  # are benign on MSVC. Keep the project's warnings-as-errors policy elsewhere.
  target_compile_options(libxaacenc PRIVATE /WX- /wd4819 /wd4828)
  target_compile_options(libxaacdec PRIVATE /WX- /wd4819 /wd4828)
endif()

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
target_link_libraries(obs-airplay-aac-eld PRIVATE libxaacenc)
target_include_directories(
  obs-airplay-aac-eld
  PRIVATE
    "${LIBXAAC_SOURCE_DIR}/common"
    "${LIBXAAC_SOURCE_DIR}/encoder"
)
target_compile_features(obs-airplay-aac-eld PRIVATE cxx_std_17)
target_compile_definitions(obs-airplay-aac-eld PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
set_target_properties(
  obs-airplay-aac-eld
  PROPERTIES
    OUTPUT_NAME "obs-airplay-aac-eld"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
add_dependencies(${CMAKE_PROJECT_NAME} obs-airplay-aac-eld)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.cpp
    src/airplay-cast.cpp
    src/airplay-cast.hpp
    src/airplay-cast-state.hpp
    src/airplay-auto-cast.hpp
    src/airplay-bridge.cpp
    src/airplay-bridge.hpp
    src/airplay-receiver.hpp
    src/airplay-settings.hpp
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

if(BUILD_TESTING)
  add_executable(obs-airplay-cast-tests tests/airplay-receiver-test.cpp)
  target_include_directories(obs-airplay-cast-tests PRIVATE src)
  target_compile_features(obs-airplay-cast-tests PRIVATE cxx_std_17)
  target_compile_definitions(
    obs-airplay-cast-tests
    PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
  )
  add_test(NAME obs-airplay-cast-receiver COMMAND obs-airplay-cast-tests)
endif()
