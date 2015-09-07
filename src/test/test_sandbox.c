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

#include "../hs_analysis_plugins.h"
#include "../hs_config.h"
#include "../hs_input_plugins.h"
#include "../hs_logger.h"
#include "../hs_output_plugins.h"
#include "../hs_sandbox.h"

char* e = NULL;


static int inject_message(lua_State* L)
{
  if (L) return 0;
  return 0;
}


static char* test_create_input_sandbox()
{
  hs_sandbox_config cfg;
  bool ret = hs_load_sandbox_config("sandbox", "input.cfg", &cfg, NULL,
                                    HS_SB_TYPE_INPUT);
  mu_assert(ret, "hs_load_sandbox_config failed");

  hs_sandbox *sb = hs_create_sandbox(NULL, "{}", &cfg);
  mu_assert(hs_init_input_sandbox(sb, &inject_message) == 0,
            "hs_init_input_sandbox failed: %s", lsb_get_error(sb->lsb));

  mu_assert(sb != NULL, "hs_create_sandbox failed");
  int status = hs_process_message(sb->lsb, NULL);
  mu_assert(status == 0, "process_message failed: %s", lsb_get_error(sb->lsb));
  status = hs_timer_event(sb->lsb, 0);
  mu_assert(status == 0, "timer_event failed: %s", lsb_get_error(sb->lsb));

  hs_free_sandbox(sb);
  hs_free_sandbox_config(&cfg);
  return NULL;
}


static char* test_create_analysis_sandbox()
{
  hs_sandbox_config cfg;
  bool ret = hs_load_sandbox_config("sandbox", "analysis.cfg", &cfg, NULL,
                                    HS_SB_TYPE_ANALYSIS);
  mu_assert(ret, "hs_load_sandbox_config failed");

  hs_sandbox *sb = hs_create_sandbox(NULL, "{}", &cfg);
  mu_assert(hs_init_analysis_sandbox(sb, &inject_message) == 0,
            "hs_init_analysis_sandbox failed: %s", lsb_get_error(sb->lsb));

  mu_assert(sb != NULL, "hs_create_sandbox failed");
  int status = hs_process_message(sb->lsb, NULL);
  mu_assert(status == 0, "process_message failed: %s", lsb_get_error(sb->lsb));
  status = hs_timer_event(sb->lsb, 0);
  mu_assert(status == 0, "timer_event failed: %s", lsb_get_error(sb->lsb));

  hs_free_sandbox(sb);
  hs_free_sandbox_config(&cfg);
  return NULL;
}


static char* test_create_output_sandbox()
{
  hs_sandbox_config cfg;
  bool ret = hs_load_sandbox_config("sandbox", "output.cfg", &cfg, NULL,
                                    HS_SB_TYPE_OUTPUT);
  mu_assert(ret, "hs_load_sandbox_config failed");

  hs_sandbox *sb = hs_create_sandbox(NULL, "{}", &cfg);
  mu_assert(hs_init_output_sandbox(sb) == 0,
            "hs_init_output_sandbox failed: %s", lsb_get_error(sb->lsb));

  mu_assert(sb != NULL, "hs_create_sandbox failed");
  int status = hs_process_message(sb->lsb, NULL);
  mu_assert(status == 0, "process_message failed: %s", lsb_get_error(sb->lsb));
  status = hs_timer_event(sb->lsb, 0);
  mu_assert(status == 0, "timer_event failed: %s", lsb_get_error(sb->lsb));

  hs_free_sandbox(sb);
  hs_free_sandbox_config(&cfg);
  return NULL;
}


static char* all_tests()
{
  mu_run_test(test_create_input_sandbox);
  mu_run_test(test_create_analysis_sandbox);
  mu_run_test(test_create_output_sandbox);
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
