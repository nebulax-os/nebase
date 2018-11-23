
macro(_export_linker_info _LINKER_VERSION_STRING)
  if("${_LINKER_VERSION_STRING}" MATCHES "^GNU ld ([^ ]+) \\[FreeBSD\\].*")
    set(NeBase_LINKER_ID "GNU.bfd")
    set(NeBase_LINKER_VERSION ${CMAKE_MATCH_1})
  elseif("${_LINKER_VERSION_STRING}" MATCHES "^GNU ld.* ([^ ]+)$")
    set(NeBase_LINKER_ID "GNU.bfd")
    set(NeBase_LINKER_VERSION ${CMAKE_MATCH_1})
  elseif("${_LINKER_VERSION_STRING}" MATCHES "^GNU gold.* ([^ ]+)$")
    set(NeBase_LINKER_ID "GNU.gold")
    set(NeBase_LINKER_VERSION ${CMAKE_MATCH_1})
  elseif("${_LINKER_VERSION_STRING}" MATCHES "^LLD ([^ ]+).*")
    set(NeBase_LINKER_ID "LLVM.lld")
    set(NeBase_LINKER_VERSION ${CMAKE_MATCH_1})
  elseif("${_LINKER_VERSION_STRING}" MATCHES ".*Solaris Link Editors: ([^ ]+)")
    set(NeBase_LINKER_ID "SUN.ld")
    set(NeBase_LINKER_VERSION ${CMAKE_MATCH_1})
  else()
    message(WARNING "Unsupported ld version:\n${_LINKER_VERSION_STRING}")
  endif()
endmacro(_export_linker_info)

macro(_detect_linker_id)
  message(STATUS "Detecting linker version info")

  # check -fuse-ld=XXX in CFLAGS
  if(NOT $ENV{CFLAGS} STREQUAL "")
    string(REGEX MATCHALL "-fuse-ld=[^ ]+" CFLAGS_USE_LDS $ENV{CFLAGS})
    if(CFLAGS_USE_LDS)
      list(GET CFLAGS_USE_LDS -1 CFLAGS_LD)
      string(SUBSTRING ${CFLAGS_LD} 9 -1 LD_TYPE)
      if("${LD_TYPE}" STREQUAL "bfd")
        set(NeBase_LINKER_ID "GNU.bfd")
      elseif("${LD_TYPE}" STREQUAL "gold")
        set(NeBase_LINKER_ID "GNU.gold")
      elseif("${LD_TYPE}" STREQUAL "lld")
        set(NeBase_LINKER_ID "LLVM.lld")
      else()
        message(WARNING "Unsupported -fuse-ld value ${LD_TYPE}")
      endif()

      if(NOT NeBase_LINKER_ID STREQUAL "")
        message(STATUS "The linker is ${NeBase_LINKER_ID}")
        return()
      endif()
    endif()
  endif()

  if(OS_DARWIN)
    set(NeBase_LINKER_ID "Apple.ld")
    message(STATUS "The linker is ${NeBase_LINKER_ID}")
    return()
  endif()

  # check ld command
  set(LD_EXE "ld")
  if(NOT $ENV{LD} STREQUAL "")
    set(LD_EXE $ENV{LD})
  endif()

  execute_process(COMMAND ${LD_EXE} -V
    RESULT_VARIABLE LD_VERSION_RESULT
    OUTPUT_VARIABLE LD_VERSION_OUTPUT)
  if(LD_VERSION_RESULT EQUAL 0)
    string(REGEX MATCH "^[^\n]+" LD_VERSION_STRING ${LD_VERSION_OUTPUT})
    _export_linker_info("${LD_VERSION_STRING}")
    message(STATUS "The linker is ${NeBase_LINKER_ID} version ${NeBase_LINKER_VERSION}")
    return()
  endif()

  message(FATAL_ERROR "Failed to get linker version info")
endmacro(_detect_linker_id)

_detect_linker_id()

set(NeBase_LD_FLAGS "-Wl,--as-needed")
set(NeBase_LD_HARDEN_FLAGS "")
set(NeBase_LD_PIE_FLAGS "")
set(GNU_COMPATIBLE_LINKERS "GNU.bfd;GNU.gold;LLVM.lld")
if(NeBase_LINKER_ID IN_LIST GNU_COMPATIBLE_LINKERS)
  set(LD_HARDEN_LIST "relro;now")
elseif(NeBase_LINKER_ID STREQUAL "SUN.ld")
  set(LD_HARDEN_LIST "nodeferred")
endif()
if(NeBase_CC_USE_WL)
  if(WITH_HARDEN_FLAGS)
    set(NeBase_LD_PIE_FLAGS "-Wl,-pie")
    foreach(HARDEN_FLAG IN LISTS LD_HARDEN_LIST)
      set(NeBase_LD_HARDEN_FLAGS "${LD_HARDEN_FLAGS} -Wl,-z,${HARDEN_FLAG}")
    endforeach()
  endif()
else()
  if(WITH_HARDEN_FLAGS)
    set(NeBase_LD_PIE_FLAGS "-pie")
    foreach(HARDEN_FLAG IN LISTS LD_HARDEN_LIST)
      set(NeBase_LD_HARDEN_FLAGS "${LD_HARDEN_FLAGS} -z ${HARDEN_FLAG}")
    endforeach()
  endif()
endif()

#Hardening flags
if(WITH_HARDEN_FLAGS)
  # also set -pie when linking exe
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${NeBase_LD_FLAGS} ${NeBase_LD_PIE_CFLAGS} ${NeBase_LD_HARDEN_FLAGS}")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${NeBase_LD_FLAGS} ${NeBase_LD_HARDEN_FLAGS}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${NeBase_LD_FLAGS} ${NeBase_LD_HARDEN_FLAGS}")
endif(WITH_HARDEN_FLAGS)
