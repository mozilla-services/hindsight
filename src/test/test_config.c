/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight unit tests @file */

#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hs_config.h"
#include "../hs_logger.h"

char* e = NULL;

static char* test_load_default_config()
{
  hs_config cfg;
  int ret = hs_load_config("cfg/default.cfg", &cfg);
  mu_assert(ret == 0, "hindsight_load_config: %d", ret);
  mu_assert(strcmp(cfg.output_path, "output_path") == 0, "received %s",
            cfg.output_path);
  mu_assert(cfg.max_message_size == 1024 * 64, "received %d", cfg.max_message_size);
  mu_assert(cfg.output_size == 1024 * 1024 * 64, "received %d",
            cfg.output_size);
  mu_assert(cfg.backpressure == 0, "received %d", cfg.backpressure);
  mu_assert(strcmp(cfg.run_path, "run") == 0, "received %s",
            cfg.run_path);
  mu_assert(strcmp(cfg.load_path, "load") == 0, "received %s",
            cfg.load_path);
  mu_assert(strcmp(cfg.io_lua_path, "io/?.lua") == 0, "received %s",
            cfg.io_lua_path);
  mu_assert(strcmp(cfg.io_lua_cpath, "io/?.so") == 0, "received %s",
            cfg.io_lua_cpath);
  mu_assert(strcmp(cfg.analysis_lua_path, "analysis/?.lua") == 0, "received %s",
            cfg.analysis_lua_path);
  mu_assert(strcmp(cfg.analysis_lua_cpath, "analysis/?.so") == 0, "received %s",
            cfg.analysis_lua_cpath);
  mu_assert(cfg.ipd.output_limit == 1024 * 64, "received %d",
            cfg.ipd.output_limit);
  mu_assert(cfg.ipd.memory_limit == 1024 * 1024 * 8, "received %d",
            cfg.ipd.memory_limit);
  mu_assert(cfg.ipd.instruction_limit == 1000000, "received %d",
            cfg.ipd.instruction_limit);
  mu_assert(cfg.ipd.preserve_data == false, "received %d",
            cfg.ipd.preserve_data);
  hs_free_config(&cfg);
  return NULL;
}


static char* test_load_config()
{
  hs_config cfg;
  int ret = hs_load_config("cfg/valid.cfg", &cfg);
  mu_assert(ret == 0, "hindsight_load_config: %d", ret);
  mu_assert(strcmp(cfg.output_path, "output_path") == 0, "received %s",
            cfg.output_path);
  mu_assert(cfg.output_size == 1024, "received %d",
            cfg.output_size);
  mu_assert(cfg.backpressure == 10, "received %d", cfg.backpressure);
  mu_assert(strcmp(cfg.run_path, "run") == 0, "received %s",
            cfg.run_path);
  mu_assert(strcmp(cfg.load_path, "load") == 0, "received %s",
            cfg.load_path);
  mu_assert(strcmp(cfg.io_lua_path, "io/?.lua") == 0, "received %s",
            cfg.io_lua_path);
  mu_assert(strcmp(cfg.io_lua_cpath, "io/?.so") == 0, "received %s",
            cfg.io_lua_cpath);
  mu_assert(strcmp(cfg.analysis_lua_path, "analysis/?.lua") == 0, "received %s",
            cfg.analysis_lua_path);
  mu_assert(strcmp(cfg.analysis_lua_cpath, "analysis/?.so") == 0, "received %s",
            cfg.analysis_lua_cpath);
  mu_assert(cfg.ipd.output_limit == 1023, "received %d",
            cfg.ipd.output_limit);
  mu_assert(cfg.ipd.memory_limit == 32767, "received %d",
            cfg.ipd.memory_limit);
  mu_assert(cfg.ipd.instruction_limit == 1000, "received %d",
            cfg.ipd.instruction_limit);
  mu_assert(cfg.ipd.preserve_data == true, "received %d",
            cfg.ipd.preserve_data);
  hs_free_config(&cfg);
  return NULL;
}


static char* test_load_invalid_config()
{
  hs_config cfg;
  int ret = hs_load_config("cfg/extra.cfg", &cfg);
  mu_assert(ret == 1, "hindsight_load_config: %d", ret);
  hs_free_config(&cfg);
  return NULL;
}


static char* test_sandbox_input_config()
{
  hs_sandbox_config cfg;
  bool ret = hs_load_sandbox_config("sandbox", "input.cfg", &cfg, NULL,
                                    HS_SB_TYPE_INPUT);
  mu_assert(ret, "hs_load_sandbox_config failed");
  mu_assert(strcmp(cfg.filename, "input.lua") == 0, "received %s",
            cfg.filename);
  mu_assert(cfg.message_matcher == NULL, "received %s", cfg.message_matcher);
  mu_assert(cfg.async_buffer_size == 0, "received %d", cfg.async_buffer_size);
  mu_assert(cfg.thread == 0, "received %d", cfg.thread);

  hs_free_sandbox_config(&cfg);
  return NULL;
}


static char* test_sandbox_analysis_config()
{
  hs_sandbox_config cfg;
  bool ret = hs_load_sandbox_config("sandbox", "analysis.cfg", &cfg, NULL,
                                    HS_SB_TYPE_ANALYSIS);
  mu_assert(ret, "hs_load_sandbox_config failed");
  mu_assert(strcmp(cfg.filename, "analysis.lua") == 0, "received %s",
            cfg.filename);
  mu_assert(strcmp(cfg.cfg_name, "analysis") == 0, "received %s",
            cfg.cfg_name);
  mu_assert(cfg.output_limit == 77777, "received %d", cfg.output_limit);
  mu_assert(cfg.memory_limit == 88888, "received %d", cfg.memory_limit);
  mu_assert(cfg.instruction_limit == 99999, "received %d",
            cfg.instruction_limit);
  mu_assert(cfg.ticker_interval == 17, "received %d", cfg.ticker_interval);
  mu_assert(cfg.preserve_data == true, "received %s",
            cfg.preserve_data ? "true" : "false");
  mu_assert(strcmp(cfg.message_matcher, "TRUE") == 0, "received %s",
            cfg.message_matcher);
  mu_assert(cfg.thread == 1, "received %d", cfg.thread);
  mu_assert(cfg.async_buffer_size == 0, "received %d", cfg.async_buffer_size);

  hs_free_sandbox_config(&cfg);
  return NULL;
}


static char* test_sandbox_output_config()
{
  hs_sandbox_config cfg;
  bool ret = hs_load_sandbox_config("sandbox", "output.cfg", &cfg, NULL,
                                    HS_SB_TYPE_OUTPUT);
  mu_assert(ret, "hs_load_sandbox_config failed");
  mu_assert(strcmp(cfg.filename, "output.lua") == 0, "received %s",
            cfg.filename);
  mu_assert(cfg.async_buffer_size == 999, "received %d", cfg.async_buffer_size);
  mu_assert(cfg.thread == 0, "received %d", cfg.thread);

  hs_free_sandbox_config(&cfg);
  return NULL;
}


static char* test_sandbox_filename_config()
{
  hs_sandbox_config cfg;
  bool ret = hs_load_sandbox_config("sandbox", "path_in_fn.cfg", &cfg, NULL,
                                    HS_SB_TYPE_INPUT);
  mu_assert(!ret, "accepted a filename with a path");
  hs_free_sandbox_config(&cfg);

  ret = hs_load_sandbox_config("sandbox", "invalid_fn_ext.cfg", &cfg, NULL,
                               HS_SB_TYPE_INPUT);
  mu_assert(!ret, "accepted a filename with a invalid extension");
  hs_free_sandbox_config(&cfg);
  return NULL;
}


static char* all_tests()
{
  mu_run_test(test_load_default_config);
  mu_run_test(test_load_config);
  mu_run_test(test_load_invalid_config);
  mu_run_test(test_sandbox_input_config);
  mu_run_test(test_sandbox_analysis_config);
  mu_run_test(test_sandbox_output_config);
  mu_run_test(test_sandbox_filename_config);
  return NULL;
}


int main()
{
  hs_init_log(7);
  char* result = all_tests();
  if (result) {
    printf("%s\n", result);
  } else {
    printf("ALL TESTS PASSED\n");
  }
  printf("Tests run: %d\n", mu_tests_run);
  free(e);
  hs_free_log();

  return result != 0;
}
