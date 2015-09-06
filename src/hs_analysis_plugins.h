/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight analysis sandbox loader @file */

#ifndef hs_analysis_plugins_h_
#define hs_analysis_plugins_h_

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "hs_config.h"
#include "hs_heka_message.h"
#include "hs_input.h"
#include "hs_message_matcher.h"
#include "hs_output.h"
#include "hs_sandbox.h"

typedef struct hs_analysis_plugins hs_analysis_plugins;
typedef struct hs_analysis_thread hs_analysis_thread;

typedef struct hs_analysis_plugin
{
  hs_sandbox* sb;
  hs_analysis_thread* at;
} hs_analysis_plugin;

struct hs_analysis_plugins
{
  hs_analysis_thread* list;
  pthread_t* threads;
  hs_config* cfg;
  hs_message_match_builder* mmb;

  int thread_cnt;
  bool stop;
  bool sample;

  hs_output output;
};

struct hs_analysis_thread
{
  hs_analysis_plugins* plugins;
  hs_analysis_plugin** list;
  hs_heka_message* msg;

  pthread_mutex_t list_lock;
  pthread_mutex_t cp_lock;
  hs_checkpoint cp;
  time_t current_t;

  int list_cap;
  int list_cnt;
  int tid;
  bool matched;

  hs_input input;
};

void hs_init_analysis_plugins(hs_analysis_plugins* plugins,
                              hs_config* cfg,
                              hs_message_match_builder* mmb);
void hs_free_analysis_plugins(hs_analysis_plugins* plugins);

void hs_start_analysis_threads(hs_analysis_plugins* plugins);

void hs_load_analysis_plugins(hs_analysis_plugins* plugins,
                              const hs_config* cfg,
                              const char* path);

void hs_start_analysis_input(hs_analysis_plugins* plugins, pthread_t* t);
void hs_wait_analysis_plugins(hs_analysis_plugins* plugins);

hs_sandbox* hs_create_analysis_sandbox(void* parent,
                                       const char* file,
                                       const hs_config* cfg,
                                       const hs_sandbox_config* sbc,
                                       lua_State* env);
int hs_init_analysis_sandbox(hs_sandbox* sb);
#endif
