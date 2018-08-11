
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  set(OS_LINUX ON)
  set(DEFAULT_INSTALL_PREFIX "/usr")
  add_definitions(-D_GNU_SOURCE)
  add_definitions(-D_FILE_OFFSET_BITS=64) # see feature_test_macros(7)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "FreeBSD")
  set(OS_FREEBSD ON)
  set(DEFAULT_INSTALL_PREFIX "/usr/local")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "NetBSD")
  set(OS_NETBSD ON)
  set(DEFAULT_INSTALL_PREFIX "/usr/pkg")
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "SunOS")
  set(OS_SOLARIS ON)
  set(DEFAULT_INSTALL_PREFIX "/usr")
  set(CMAKE_LIBRARY_ARCHITECTURE "64") # currently we only support 64
  add_definitions(-D_LARGEFILE64_SOURCE) # see lfcompile64(7)
else()
  message(FATAL_ERROR "Unsupported Host System: ${CMAKE_HOST_SYSTEM_NAME}")
endif()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${DEFAULT_INSTALL_PREFIX}" CACHE PATH "Install Prefix" FORCE)
endif()

include(GNUInstallDirs)
if(KERNEL_SOLARIS)
  set(CMAKE_INSTALL_LIBDIR "${CMAKE_INSTALL_LIBDIR}/${CMAKE_LIBRARY_ARCHITECTURE}")
  set(CMAKE_INSTALL_FULL_LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}/${CMAKE_LIBRARY_ARCHITECTURE}")
endif(KERNEL_SOLARIS)

set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)
link_directories(${CMAKE_INSTALL_FULL_LIBDIR})

# set -fPIC/-fPIE
#   cmake will add -fPIE to execute src files, if compiler complains about
# recompiling with -fPIC, just add a object library for that src first.
set(CMAKE_POSITION_INDEPENDENT_CODE TRUE)
