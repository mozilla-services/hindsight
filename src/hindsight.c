/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight main executable @file */

#include <errno.h>
#include <lauxlib.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hs_analysis_plugins.h"
#include "hs_config.h"
#include "hs_input.h"
#include "hs_input_plugins.h"
#include "hs_logger.h"
#include "hs_sandbox.h"

static pthread_mutex_t g_shutdown;

static void stop_signal(int sig)
{
  fprintf(stderr, "stop signal received %d\n", sig);
  signal(SIGINT, SIG_DFL);
  pthread_mutex_unlock(&g_shutdown);
}


static void input_main(hs_config* cfg)
{
  hs_input_plugins plugins;
  hs_init_input_plugins(&plugins, cfg, &g_shutdown);
  hs_load_input_plugins(&plugins, cfg, cfg->run_path);

  struct timespec ts;
  while (true) {
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
      hs_log(HS_APP_NAME, 3, "clock_gettime failed");
    }
    ts.tv_sec += 60;
    if (!pthread_mutex_timedlock(plugins.shutdown, &ts)) {
      pthread_mutex_unlock(plugins.shutdown);
      break; // shutting down
    }
    fprintf(stderr, "todo scan the load directory\n");
  }
  hs_stop_input_plugins(&plugins);
  hs_free_input_plugins(&plugins);
  hs_free_config(cfg);
  hs_free_log();
}


static void analysis_main(hs_config* cfg)
{
  hs_analysis_plugins plugins;
  hs_init_analysis_plugins(&plugins, cfg, &g_shutdown);
  hs_start_analysis_threads(&plugins);
  hs_load_analysis_plugins(&plugins, cfg, cfg->run_path);
  sched_yield();
  // the input uses the extra thread slot allocated at the end
  hs_start_analysis_input(&plugins, &plugins.threads[cfg->threads]);

  struct timespec ts;
  while (true) {
    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
      hs_log(HS_APP_NAME, 3, "clock_gettime failed");
    }
    ts.tv_sec += 60;
    if (!pthread_mutex_timedlock(plugins.shutdown, &ts)) {
      pthread_mutex_unlock(plugins.shutdown);
      break; // shutting down
    }
    fprintf(stderr, "todo scan the load directory\n");
  }
  plugins.stop = true;
  hs_free_analysis_plugins(&plugins);
  hs_free_config(cfg);
  hs_free_log();
}


int main(int argc, char* argv[])
{
  if (argc != 2) {
    fprintf(stderr, "usage: %s <cfg>\n", argv[0]);
    return EXIT_FAILURE;
  }
  hs_init_log();

  hs_config cfg;
  if (hs_load_config(argv[1], &cfg)) {
    return EXIT_FAILURE;
  }
  signal(SIGINT, stop_signal);

  switch (cfg.mode) {
  case HS_MODE_INPUT:
    hs_log(HS_APP_NAME, 6, "starting (input mode)");
    input_main(&cfg);
    break;
  case HS_MODE_ANALYSIS:
    hs_log(HS_APP_NAME, 6, "starting (analysis mode)");
    analysis_main(&cfg);
    break;
  default:
    fprintf(stderr, "'output' mode has not been implemented yet\n");
    return EXIT_FAILURE;
    break;
  }
  hs_log(HS_APP_NAME, 6, "exiting", cfg.mode);
  return 0;
}
