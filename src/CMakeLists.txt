# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

set(HINDSIGHT_SRC
hindsight.c
hs_analysis_plugins.c
hs_checkpoint_reader.c
hs_checkpoint_writer.c
hs_config.c
hs_heka_message.c
hs_heka_stream_reader.c
hs_input.c
hs_input_plugins.c
hs_logger.c
hs_message_matcher.c
hs_output.c
hs_output_plugins.c
hs_sandbox.c
hs_string_matcher.c
hs_util.c
)

set ( CMAKE_SKIP_BUILD_RPATH FALSE CACHE STRING "" FORCE )
set ( CMAKE_BUILD_WITH_INSTALL_RPATH FALSE CACHE STRING "" FORCE )

add_executable(hindsight ${HINDSIGHT_SRC})
set(HINDSIGHT_LIBS
${LUASANDBOX_LIBRARIES}
${UNIX_LIBRARIES})

target_link_libraries(hindsight ${HINDSIGHT_LIBS})

install(TARGETS hindsight DESTINATION bin)
install(DIRECTORY "${PROJECT_PATH}/lib/"  DESTINATION lib  PATTERN "lua" EXCLUDE)
install(DIRECTORY "${PROJECT_PATH}/lib/lua/" DESTINATION modules PATTERN "strict.lua" EXCLUDE)
install(DIRECTORY "${PROJECT_PATH}/include/" DESTINATION include)

add_subdirectory(test)
