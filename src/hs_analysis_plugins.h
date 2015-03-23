/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight analysis sandbox loader @file */

#ifndef hs_analysis_plugins_h_
#define hs_analysis_plugins_h_

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>

#include "hs_config.h"
#include "hs_input.h"
#include "hs_output.h"
#include "hs_sandbox.h"

typedef struct hs_analysis_plugins hs_analysis_plugins;
typedef struct hs_analysis_thread hs_analysis_thread;

struct hs_analysis_plugins
{
  pthread_mutex_t lock;
  pthread_mutex_t* shutdown;

  hs_analysis_thread* list;
  pthread_t* threads;
  hs_config* cfg;
  hs_output output;
  hs_input input;

  void* msg;

  int thread_cnt;
  bool stop;
};

struct hs_analysis_thread
{
  hs_analysis_plugins* plugins;
  sem_t start;
  sem_t finished;

  hs_sandbox** list;
  int plugin_cnt;
  int tid;
};

void hs_init_analysis_plugins(hs_analysis_plugins* plugins, hs_config* cfg,
                              pthread_mutex_t* shutdown);
void hs_free_analysis_plugins(hs_analysis_plugins* plugins);

void hs_start_analysis_threads(hs_analysis_plugins* plugins);

void hs_load_analysis_plugins(hs_analysis_plugins* plugins,
                              const hs_config* cfg,
                              const char* path);

void hs_start_analysis_input(hs_analysis_plugins* plugins, pthread_t* t);
#endif
