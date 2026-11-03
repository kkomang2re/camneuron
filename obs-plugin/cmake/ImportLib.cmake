# 설치된 OBS의 DLL에서 MSVC 링크용 import library(.lib)를 만든다.
# OBS는 SDK를 따로 배포하지 않으므로 dumpbin으로 export 목록을 뽑아 .def를 만들고 lib.exe로 변환한다.

get_filename_component(_pcam_msvc_bin "${CMAKE_LINKER}" DIRECTORY)
find_program(PCAM_DUMPBIN dumpbin HINTS "${_pcam_msvc_bin}" REQUIRED)
find_program(PCAM_LIB_TOOL lib HINTS "${_pcam_msvc_bin}" REQUIRED)

function(pcam_import_lib_from_dll target dll)
  if(NOT EXISTS "${dll}")
    message(FATAL_ERROR "DLL을 찾을 수 없습니다: ${dll}")
  endif()

  get_filename_component(_name "${dll}" NAME_WE)
  set(_dir "${CMAKE_CURRENT_BINARY_DIR}/importlibs")
  set(_def "${_dir}/${_name}.def")
  set(_lib "${_dir}/${_name}.lib")
  file(MAKE_DIRECTORY "${_dir}")

  execute_process(
    COMMAND "${PCAM_DUMPBIN}" /NOLOGO /EXPORTS "${dll}"
    OUTPUT_VARIABLE _exports
    RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "dumpbin 실패: ${dll}")
  endif()

  # "    ordinal hint RVA      name" 아래 줄들: "  1    0 00012340 base_get_alignment"
  string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +[A-Za-z_][A-Za-z0-9_@?$]*" _lines "${_exports}")
  set(_def_content "LIBRARY ${_name}\nEXPORTS\n")
  foreach(_line IN LISTS _lines)
    string(REGEX REPLACE ".* ([A-Za-z_][A-Za-z0-9_@?$]*)$" "\\1" _symbol "${_line}")
    string(APPEND _def_content "  ${_symbol}\n")
  endforeach()
  list(LENGTH _lines _count)
  if(_count EQUAL 0)
    message(FATAL_ERROR "${dll}에서 export를 찾지 못했습니다.")
  endif()
  file(WRITE "${_def}" "${_def_content}")

  execute_process(
    COMMAND "${PCAM_LIB_TOOL}" /NOLOGO /MACHINE:X64 "/DEF:${_def}" "/OUT:${_lib}"
    OUTPUT_QUIET
    RESULT_VARIABLE _result)
  if(NOT _result EQUAL 0)
    message(FATAL_ERROR "lib.exe 실패: ${_def}")
  endif()
  message(STATUS "Import library: ${_lib} (${_count} exports)")

  add_library(${target} SHARED IMPORTED GLOBAL)
  set_target_properties(${target} PROPERTIES IMPORTED_IMPLIB "${_lib}" IMPORTED_LOCATION "${dll}")
endfunction()
