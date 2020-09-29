/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/** @brief Hindsight logging implementation @file */

#include "hs_logger.h"
#include "hs_util.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static pthread_mutex_t g_logger;
static int g_loglevel;

void hs_init_log(int loglevel)
{
  g_loglevel = loglevel;
  if (pthread_mutex_init(&g_logger, NULL)) {
    perror("loger pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


int hs_get_log_level()
{
  return g_loglevel;
}


void hs_free_log()
{
  pthread_mutex_destroy(&g_logger);
}


static void release_mutex(void *arg)
{
  pthread_mutex_unlock((pthread_mutex_t *)arg);
}


void
hs_log(void *context, const char *plugin, int severity, const char *fmt, ...)
{
  if (severity > g_loglevel) return;

  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
    ts.tv_sec = time(NULL);
    fprintf(stderr, "%lld [error] hs_log clock_gettime failed\n",
            ts.tv_sec * 1000000000LL);
    ts.tv_nsec = 0;
  }

  const char *level;
  switch (severity) {
  case 7:
    level = "debug";
    break;
  case 6:
    level = "info";
    break;
  case 5:
    level = "notice";
    break;
  case 4:
    level = "warning";
    break;
  case 3:
    level = "error";
    break;
  case 2:
    level = "crit";
    break;
  case 1:
    level = "alert";
    break;
  case 0:
    level = "panic";
    break;
  default:
    level = "debug";
    break;
  }

  va_list args;
  va_start(args, fmt);
  pthread_mutex_lock(&g_logger);
  pthread_cleanup_push(release_mutex, &g_logger);
  fprintf(stderr, "%lld [%s] %s ", ts.tv_sec * 1000000000LL + ts.tv_nsec, level,
          plugin ? plugin : "unnamed");
  vfprintf(stderr, fmt, args);
  fwrite("\n", 1, 1, stderr);
  pthread_cleanup_pop(1);
  va_end(args);

  if (context && severity < 4) {
    hs_log_context *ctx = (hs_log_context *)context;
    if (ctx->output_path) { // currently only set during plugin creation
      va_start(args, fmt);
      hs_save_termination_err_vfmt(ctx->output_path, ctx->plugin_name, fmt, args);
      va_end(args);
    }
  }
}
