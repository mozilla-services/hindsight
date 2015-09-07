# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

configure_file(test.h.in test.h ESCAPE_QUOTES)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

set(HINDSIGHT_TEST_SRC
../hs_analysis_plugins.c
../hs_checkpoint_reader.c
../hs_checkpoint_writer.c
../hs_config.c
../hs_heka_message.c
../hs_heka_stream_reader.c
../hs_input.c
../hs_input_plugins.c
../hs_logger.c
../hs_message_matcher.c
../hs_output.c
../hs_output_plugins.c
../hs_running_stats.c
../hs_sandbox.c
../hs_string_matcher.c
../hs_util.c
)


add_executable(test_config ../hs_config.c ../hs_logger.c ../hs_checkpoint_reader.c ../hs_util.c test_config.c)
target_link_libraries(test_config ${HINDSIGHT_LIBS})
add_test(NAME test_config WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND test_config)

add_executable(test_heka_message ../hs_heka_message.c ../hs_logger.c ../hs_util.c test_heka_message.c)
target_link_libraries(test_heka_message ${HINDSIGHT_LIBS})
add_test(NAME test_heka_message WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND test_heka_message)

add_executable(test_message_matcher ../hs_message_matcher.c ../hs_heka_message.c ../hs_logger.c ../hs_util.c ../hs_string_matcher.c test_message_matcher.c)
target_link_libraries(test_message_matcher ${HINDSIGHT_LIBS})
add_test(NAME test_message_matcher WORKING_DIRECTORY ${CMAKE_BINARY_DIR} COMMAND test_message_matcher)

add_executable(test_string_matcher ../hs_string_matcher.c test_string_matcher.c)
target_link_libraries(test_string_matcher ${HINDSIGHT_LIBS})
add_test(NAME test_string_matcher WORKING_DIRECTORY ${CMAKE_BINARY_DIR} COMMAND test_string_matcher)

add_executable(test_sandbox ${HINDSIGHT_TEST_SRC} test_sandbox.c)
target_link_libraries(test_sandbox ${HINDSIGHT_LIBS})
add_test(NAME test_sandbox WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND test_sandbox)
