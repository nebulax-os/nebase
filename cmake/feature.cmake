
set(WITH_SYSTEMD_DESC "Build with systemd support")
option(WITH_SYSTEMD ${WITH_SYSTEMD_DESC} ON)
if(OS_LINUX)
  add_feature_info(WITH_SYSTEMD WITH_SYSTEMD ${WITH_SYSTEMD_DESC})
else()
  set(WITH_SYSTEMD OFF)
endif()

set(WITH_GLIB2_DESC "Build with glib2")
option(WITH_GLIB2 ${WITH_GLIB2_DESC} ON)
add_feature_info(WITH_GLIB2 WITH_GLIB2 ${WITH_GLIB2_DESC})

set(USE_AIO_POLL_DESC "Build using aio poll instead of epoll")
option(USE_AIO_POLL ${USE_AIO_POLL_DESC} ON)
if(OS_LINUX)
  if(CMAKE_HOST_SYSTEM_VERSION VERSION_LESS 4.19.0)
    set(USE_AIO_POLL OFF)
  endif()
  add_feature_info(USE_AIO_POLL USE_AIO_POLL ${USE_AIO_POLL_DESC})
else()
  set(USE_AIO_POLL OFF)
endif()

set(USE_IO_URING_DESC "Build using io_uring instead of epoll")
option(USE_IO_URING ${USE_IO_URING_DESC} OFF)
if(OS_LINUX)
  if(CMAKE_HOST_SYSTEM_VERSION VERSION_LESS 5.1.0)
    set(USE_IO_URING OFF)
  endif()
  add_feature_info(USE_IO_URING USE_IO_URING ${USE_IO_URING_DESC})
else()
  set(USE_IO_URING OFF)
endif()
