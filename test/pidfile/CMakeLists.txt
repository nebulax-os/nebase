
add_executable(pidfile_test_daemon test_pidfile_daemon.c)
target_link_libraries(pidfile_test_daemon $<TARGET_NAME:nebase>)
add_test(NAME pidfile_test_daemon COMMAND $<TARGET_NAME:pidfile_test_daemon>)
