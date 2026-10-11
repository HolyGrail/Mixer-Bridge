cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

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

add_library(vendored-deps STATIC deps/libebur128/ebur128.c deps/cjson/cJSON.c)
target_include_directories(vendored-deps PUBLIC deps/libebur128 deps/cjson PRIVATE deps/libebur128/queue)
set_target_properties(vendored-deps PROPERTIES FOLDER deps POSITION_INDEPENDENT_CODE ON)
# 同梱ライブラリには自前の警告設定を当てない
target_compile_definitions(vendored-deps PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
target_compile_options(vendored-deps PRIVATE $<$<C_COMPILER_ID:MSVC>:/W0> $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE vendored-deps)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.c src/meter.c src/tap.c src/protocol.c)

if(WIN32)
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/server-win.c)
else()
  target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/server-stub.c)
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
