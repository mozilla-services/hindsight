/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/** Hindsight output sandbox loader @file */

#ifndef hs_output_plugins_h_
#define hs_output_plugins_h_

#include <luasandbox/heka/sandbox.h>
#include <luasandbox/util/heka_message.h>
#include <luasandbox/util/heka_message_matcher.h>
#include <luasandbox/util/running_stats.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include <stdint.h>

#include "hs_config.h"
#include "hs_input.h"
#include "hs_logger.h"
#include "hs_output.h"

typedef struct hs_output_plugin hs_output_plugin;
typedef struct hs_output_plugins hs_output_plugins;

struct hs_output_plugin {
  char                *name;
  lsb_heka_sandbox    *hsb;
  lsb_message_matcher *mm;
  hs_output_plugins   *plugins;
  uintptr_t           sequence_id;
  lsb_running_stats   mms;
  lsb_heka_stats      stats;
  int                 ticker_interval;
  int                 mm_delta_cnt;
  int                 pm_delta_cnt;
  int                 max_mps;
  time_t              ticker_expires;

  pthread_t thread;
  int       list_index;
  bool      batching;
  bool      stop;
  bool      sample;
  bool      pm_sample;
  bool      rm_cp_terminate;
  bool      shutdown_terminate;
  char      read_queue;
  hs_input input;
  hs_input analysis;

  pthread_mutex_t     cp_lock;
  hs_checkpoint_pair  cp;
  hs_checkpoint_pair  cur;
  hs_checkpoint_pair  *async_cp;
  int                 async_len;
  hs_log_context      ctx;
  uintptr_t           ack_sequence_id;
};

struct hs_output_plugins {
  hs_output_plugin      **list;
  hs_config             *cfg;
  hs_checkpoint_reader  *cpr;
  hs_output             *output;

  pthread_mutex_t list_lock;
  int list_cnt;
  int list_cap;

#ifdef HINDSIGHT_CLI
  bool terminated;
#endif
};

void hs_init_output_plugins(hs_output_plugins *plugins,
                            hs_config *cfg,
                            hs_checkpoint_reader *cpr,
                            hs_output *output);

void hs_free_output_plugins(hs_output_plugins *plugins);

void hs_load_output_startup(hs_output_plugins *plugins);

void hs_load_output_dynamic(hs_output_plugins *plugins, const char *name);

void hs_stop_output_plugins(hs_output_plugins *plugins);

void hs_wait_output_plugins(hs_output_plugins *plugins);

#endif
