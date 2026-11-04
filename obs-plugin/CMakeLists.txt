cmake_minimum_required(VERSION 3.24)

project(obs-phonecam VERSION 0.2.4 LANGUAGES C RC)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "지금은 Windows x64만 지원합니다.")
endif()

# 플러그인은 설치된 OBS와 같은 버전의 헤더/FFmpeg으로 빌드해야 한다. scripts/setup-deps.ps1이 .deps를 채운다.
set(OBS_INSTALL_DIR "C:/Program Files/obs-studio" CACHE PATH "설치된 OBS Studio 폴더")
set(OBS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-studio" CACHE PATH "설치된 OBS와 같은 버전의 OBS 소스")
set(OBS_DEPS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/obs-deps" CACHE PATH "설치된 OBS와 같은 버전의 obs-deps")

foreach(_required IN ITEMS "${OBS_SOURCE_DIR}/libobs/obs-module.h" "${OBS_DEPS_DIR}/include/libavcodec/avcodec.h")
  if(NOT EXISTS "${_required}")
    message(FATAL_ERROR "${_required} 가 없습니다. 먼저 scripts/setup-deps.ps1을 실행하세요.")
  endif()
endforeach()

include(cmake/ImportLib.cmake)
pcam_import_lib_from_dll(pcam_obs "${OBS_INSTALL_DIR}/bin/64bit/obs.dll")
pcam_import_lib_from_dll(pcam_w32_pthreads "${OBS_INSTALL_DIR}/bin/64bit/w32-pthreads.dll")
# 장면 목록·장면 바꾸기·방송 상태 (폰에서 방송 진행)
pcam_import_lib_from_dll(pcam_obs_frontend "${OBS_INSTALL_DIR}/bin/64bit/obs-frontend-api.dll")

# libobs의 obs-config.h가 빌드 때 생성되는 obsconfig.h를 include한다. 플러그인에 필요한 값은 없다.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h"
     "#pragma once\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")

# DLL 파일 정보(버전). 설치 프로그램과 탐색기 "자세히" 탭에 보인다.
configure_file(cmake/version.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc" @ONLY)

add_library(obs-phonecam MODULE)
target_sources(
  obs-phonecam
  PRIVATE src/plugin-main.c
          src/phonecam-source.c
          src/control.c
          src/writer.c
          src/remote.c
          src/monitor.c
          src/overlay.c
          src/decoder.c
          src/audio.c
          src/discovery.c
          src/pairing.c
          src/net.c
          src/adb.c
          "${CMAKE_CURRENT_BINARY_DIR}/generated/version.rc")

set_target_properties(obs-phonecam PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON PREFIX "")
target_compile_definitions(obs-phonecam PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}" _CRT_SECURE_NO_WARNINGS)
# 소스 주석이 UTF-8 한글이라 /utf-8이 없으면 한국어 Windows(CP949)에서 경고·오류가 난다.
target_compile_options(obs-phonecam PRIVATE $<$<COMPILE_LANGUAGE:C>:/utf-8 /W3>)

target_include_directories(
  obs-phonecam
  PRIVATE "${OBS_SOURCE_DIR}/libobs"
          "${OBS_SOURCE_DIR}/frontend/api"
          "${OBS_SOURCE_DIR}/deps/w32-pthreads"
          "${CMAKE_CURRENT_BINARY_DIR}/generated"
          "${OBS_DEPS_DIR}/include")

target_link_libraries(
  obs-phonecam
  PRIVATE pcam_obs
          pcam_obs_frontend
          pcam_w32_pthreads
          "${OBS_DEPS_DIR}/lib/avcodec.lib"
          "${OBS_DEPS_DIR}/lib/avutil.lib"
          ws2_32
          iphlpapi
          bcrypt)

# OBS 33의 플러그인 위치: C:/ProgramData/obs-studio/plugins/<이름>/<이름>.dll, 데이터는 <이름>/data
file(TO_CMAKE_PATH "$ENV{ProgramData}/obs-studio/plugins/obs-phonecam" PCAM_INSTALL_DIR)
install(TARGETS obs-phonecam LIBRARY DESTINATION "${PCAM_INSTALL_DIR}" RUNTIME DESTINATION "${PCAM_INSTALL_DIR}")
install(DIRECTORY data/ DESTINATION "${PCAM_INSTALL_DIR}/data")
