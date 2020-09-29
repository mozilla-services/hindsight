# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

configure_file(test.h.in test.h ESCAPE_QUOTES)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

add_executable(test_config ../hs_config.c ../hs_logger.c ../hs_checkpoint_reader.c ../hs_util.c test_config.c)
target_link_libraries(test_config ${HINDSIGHT_LIBS})
add_test(NAME test_config WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND test_config)

