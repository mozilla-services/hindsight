add_test(NAME plugin_monitor
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         CONFIGURATIONS hindsight
         COMMAND sh -c "./run.sh")
