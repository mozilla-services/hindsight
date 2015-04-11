# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

add_executable(test_config ../hs_config.c ../hs_logger.c test_config.c)
target_link_libraries(test_config ${HINDSIGHT_LIBS})
add_test(NAME test_config WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND test_config)

add_executable(test_heka_message ../hs_heka_message.c ../hs_logger.c test_heka_message.c)
target_link_libraries(test_heka_message ${HINDSIGHT_LIBS})
add_test(NAME test_heka_message WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND test_heka_message)

add_executable(test_message_matcher ../hs_message_matcher.c ../hs_heka_message.c ../hs_logger.c test_message_matcher.c)
target_link_libraries(test_message_matcher ${HINDSIGHT_LIBS})
add_test(NAME test_message_matcher WORKING_DIRECTORY ${CMAKE_BINARY_DIR} COMMAND test_message_matcher)
