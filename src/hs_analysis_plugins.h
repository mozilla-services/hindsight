/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight analysis sandbox loader @file */

#ifndef hs_analysis_plugins_h_
#define hs_analysis_plugins_h_

#include <luasandbox/heka/sandbox.h>
#include <luasandbox/util/heka_message.h>
#include <luasandbox/util/heka_message_matcher.h>
#include <luasandbox/util/running_stats.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "hs_config.h"
#include "hs_input.h"
#include "hs_output.h"

typedef struct hs_analysis_plugin hs_analysis_plugin;
typedef struct hs_analysis_plugins hs_analysis_plugins;
typedef struct hs_analysis_thread hs_analysis_thread;

struct hs_analysis_plugin {
  char                *name;
  lsb_heka_sandbox    *hsb;
  lsb_message_matcher *mm;
  hs_analysis_thread  *at;
  lsb_running_stats   mms;
  lsb_heka_stats      stats;
  int                 ticker_interval;
  bool                shutdown_terminate;
  time_t              ticker_expires;
};

struct hs_analysis_plugins {
  hs_analysis_thread    *list;
  pthread_t             *threads;
  hs_config             *cfg;
  hs_checkpoint_reader  *cpr;
  int                   thread_cnt;
  hs_output             output;
};

struct hs_analysis_thread {
  hs_analysis_plugins *plugins;
  hs_analysis_plugin  **list;
  lsb_heka_message    *msg;

  pthread_mutex_t list_lock;
  pthread_mutex_t cp_lock;
  hs_checkpoint   cp;
  time_t          current_t;

  int      list_cap;
  int      list_cnt;
  int      tid;
  hs_input input;
  bool     stop;
  bool     sample;
};

void hs_init_analysis_plugins(hs_analysis_plugins *plugins,
                              hs_config *cfg,
                              hs_checkpoint_reader *cpr);

void hs_free_analysis_plugins(hs_analysis_plugins *plugins);

void hs_start_analysis_threads(hs_analysis_plugins *plugins);

void hs_load_analysis_plugins(hs_analysis_plugins *plugins, bool dynamic);

void hs_start_analysis_input(hs_analysis_plugins *plugins, pthread_t *t);

void hs_stop_analysis_plugins(hs_analysis_plugins *plugins);

void hs_wait_analysis_plugins(hs_analysis_plugins *plugins);

#endif
