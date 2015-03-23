/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight unit tests @file */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hs_config.h"
#include "../hs_logger.h"

#ifdef _WIN32
#define snprintf _snprintf
#endif

#define mu_assert(cond, ...)                                                   \
do {                                                                           \
  if (!(cond)) {                                                               \
    int cnt = snprintf(mu_err, MU_ERR_LEN, "line: %d (%s) ", __LINE__, #cond); \
    if (cnt > 0 && cnt < MU_ERR_LEN) {                                         \
      cnt = snprintf(mu_err+cnt, MU_ERR_LEN-cnt, __VA_ARGS__);                 \
      if (cnt > 0 && cnt < MU_ERR_LEN) {                                       \
        return mu_err;                                                         \
      }                                                                        \
    }                                                                          \
    mu_err[MU_ERR_LEN - 1] = 0;                                                \
    return mu_err;                                                             \
  }                                                                            \
} while (0)

#define mu_run_test(test)                                                      \
do {                                                                           \
  char *message = test();                                                      \
  mu_tests_run++;                                                              \
  if (message)                                                                 \
    return message;                                                            \
} while (0)

#define MU_ERR_LEN 1024
int mu_tests_run = 0;
char mu_err[MU_ERR_LEN] = { 0 };
char* e = NULL;

static char* test_load_default_config()
{
  hs_config cfg;
  int ret = hs_load_config("cfg/default.cfg", &cfg);
  mu_assert(ret == 0, "hindsight_load_config: %d", ret);
  mu_assert(cfg.mode == HS_MODE_INPUT, "received %d", cfg.mode);
  mu_assert(strcmp(cfg.output_path, "output_path") == 0, "received %s",
            cfg.output_path);
  mu_assert(cfg.output_size == 1024 * 1024 * 64, "received %d",
            cfg.output_size);
  mu_assert(strcmp(cfg.run_path, "run") == 0, "received %s",
            cfg.run_path);
  mu_assert(strcmp(cfg.load_path, "load") == 0, "received %s",
            cfg.load_path);
  mu_assert(cfg.sbc.output_limit == 1024 * 64, "received %d",
            cfg.sbc.output_limit);
  mu_assert(cfg.sbc.memory_limit == 1024 * 1024 * 8, "received %d",
            cfg.sbc.memory_limit);
  mu_assert(cfg.sbc.instruction_limit == 1000000, "received %d",
            cfg.sbc.instruction_limit);
  mu_assert(cfg.sbc.preserve_data == false, "received %d",
            cfg.sbc.preserve_data);
  mu_assert(strcmp(cfg.sbc.module_path, "module") == 0, "received %s",
            cfg.sbc.module_path);
  hs_free_config(&cfg);
  return NULL;
}

static char* test_load_config()
{
  hs_config cfg;
  int ret = hs_load_config("cfg/valid.cfg", &cfg);
  mu_assert(ret == 0, "hindsight_load_config: %d", ret);
  mu_assert(cfg.mode == HS_MODE_INPUT, "received %d", cfg.mode);
  mu_assert(strcmp(cfg.output_path, "output_path") == 0, "received %s",
            cfg.output_path);
  mu_assert(cfg.output_size == 1024, "received %d",
            cfg.output_size);
  mu_assert(strcmp(cfg.run_path, "run") == 0, "received %s",
            cfg.run_path);
  mu_assert(strcmp(cfg.load_path, "load") == 0, "received %s",
            cfg.load_path);
  mu_assert(cfg.sbc.output_limit == 1023, "received %d",
            cfg.sbc.output_limit);
  mu_assert(cfg.sbc.memory_limit == 32767, "received %d",
            cfg.sbc.memory_limit);
  mu_assert(cfg.sbc.instruction_limit == 1000, "received %d",
            cfg.sbc.instruction_limit);
  mu_assert(cfg.sbc.preserve_data == true, "received %d",
            cfg.sbc.preserve_data);
  mu_assert(strcmp(cfg.sbc.module_path, "module") == 0, "received %s",
            cfg.sbc.module_path);
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
  hs_init_log();
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
