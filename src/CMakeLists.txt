# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

set(HINDSIGHT_SRC
hindsight.c
hs_analysis_plugins.c
hs_checkpoint_reader.c
hs_checkpoint_writer.c
hs_config.c
hs_input.c
hs_input_plugins.c
hs_logger.c
hs_output.c
hs_output_plugins.c
hs_util.c
)

set(HINDSIGHT_LIBS
${LUASANDBOX_LIBRARIES}
${UNIX_LIBRARIES})

add_executable(hindsight ${HINDSIGHT_SRC})
set(HINDSIGHT_LIBS
${LUASANDBOX_LIBRARIES}
${UNIX_LIBRARIES})

target_link_libraries(hindsight ${HINDSIGHT_LIBS})

add_executable(hindsight_cli ${HINDSIGHT_SRC})

target_compile_definitions(hindsight_cli PUBLIC "-DHINDSIGHT_CLI")
target_link_libraries(hindsight_cli ${HINDSIGHT_LIBS})

install(TARGETS hindsight DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS hindsight_cli DESTINATION ${CMAKE_INSTALL_BINDIR})

add_subdirectory(test)
