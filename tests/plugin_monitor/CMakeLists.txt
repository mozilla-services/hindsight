add_custom_target(plugin_monitor_copy_tests ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
  COMMAND ${CMAKE_COMMAND} -E make_directory load/input
  COMMAND ${CMAKE_COMMAND} -E make_directory load/analysis
  COMMAND ${CMAKE_COMMAND} -E make_directory load/output)

add_test(NAME plugin_monitor
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
         CONFIGURATIONS hindsight
         COMMAND sh -c "./run.sh")
