
add_executable(ns
  main.c
  swap.c
  proc.c
)
target_link_libraries(ns PRIVATE $<TARGET_NAME:nebase>)
