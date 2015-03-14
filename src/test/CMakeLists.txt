# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

add_executable(test_hindsight ../hindsight_config.c ../hindsight_logger.c test_hindsight.c)
target_link_libraries(test_hindsight ${HINDSIGHT_LIBS})

add_test(NAME test_hindsight WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND test_hindsight)
