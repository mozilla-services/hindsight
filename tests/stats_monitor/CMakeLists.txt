add_test(NAME stats_monitor
         WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
         COMMAND sh -c "rm -rf output; $<TARGET_FILE:hindsight_cli> hindsight.cfg 7 2>&1 |grep -q 'output.test_alert success'")
