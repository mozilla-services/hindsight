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
  mu_assert(cfg.output_size == 1024 * 1024 * 64, "received %d",
            cfg.output_size);
  mu_assert(strcmp(cfg.run_path, "run") == 0, "received %s",
            cfg.run_path);
  mu_assert(strcmp(cfg.load_path, "load") == 0, "received %s",
            cfg.load_path);
  mu_assert(cfg.ipd.output_limit == 1024 * 64, "received %d",
            cfg.ipd.output_limit);
  mu_assert(cfg.ipd.memory_limit == 1024 * 1024 * 8, "received %d",
            cfg.ipd.memory_limit);
  mu_assert(cfg.ipd.instruction_limit == 1000000, "received %d",
            cfg.ipd.instruction_limit);
  mu_assert(cfg.ipd.preserve_data == false, "received %d",
            cfg.ipd.preserve_data);
  mu_assert(strcmp(cfg.ipd.module_path, "module") == 0, "received %s",
            cfg.ipd.module_path);
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
  mu_assert(strcmp(cfg.run_path, "run") == 0, "received %s",
            cfg.run_path);
  mu_assert(strcmp(cfg.load_path, "load") == 0, "received %s",
            cfg.load_path);
  mu_assert(cfg.ipd.output_limit == 1023, "received %d",
            cfg.ipd.output_limit);
  mu_assert(cfg.ipd.memory_limit == 32767, "received %d",
            cfg.ipd.memory_limit);
  mu_assert(cfg.ipd.instruction_limit == 1000, "received %d",
            cfg.ipd.instruction_limit);
  mu_assert(cfg.ipd.preserve_data == true, "received %d",
            cfg.ipd.preserve_data);
  mu_assert(strcmp(cfg.ipd.module_path, "module") == 0, "received %s",
            cfg.ipd.module_path);
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

static char* all_tests()
{
  mu_run_test(test_load_default_config);
  mu_run_test(test_load_config);
  mu_run_test(test_load_invalid_config);
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
