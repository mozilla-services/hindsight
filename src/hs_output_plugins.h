/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight output sandbox loader @file */

#ifndef hs_output_plugins_h_
#define hs_output_plugins_h_

#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "hs_config.h"
#include "hs_heka_message.h"
#include "hs_input.h"
#include "hs_message_matcher.h"
#include "hs_output.h"
#include "hs_sandbox.h"

typedef struct hs_output_plugins hs_output_plugins;
typedef struct hs_output_plugin hs_output_plugin;


struct hs_output_plugins
{
  hs_output_plugin** list;
  hs_config* cfg;
  hs_message_match_builder* mmb;

  int list_cnt;
  int list_cap;
  bool stop;

  pthread_mutex_t list_lock;
};


struct hs_output_plugin
{
  hs_sandbox* sb;
  hs_output_plugins* plugins;
  hs_heka_message* msg;

  pthread_t thread;
  int list_index;
  bool matched;
  bool sample;
  bool batching;

  hs_input input;
  hs_input analysis;

  pthread_mutex_t cp_lock;
  hs_checkpoint_pair cp;
  hs_checkpoint_pair cur;
  hs_checkpoint_pair *async_cp;
  int async_len;
};


void hs_init_output_plugins(hs_output_plugins* plugins,
                            hs_config* cfg,
                            hs_message_match_builder* mmb);
void hs_free_output_plugins(hs_output_plugins* plugins);
void hs_load_output_plugins(hs_output_plugins* plugins,
                            const hs_config* cfg,
                            const char* path);
void hs_wait_output_plugins(hs_output_plugins* plugins);

hs_sandbox* hs_create_output_sandbox(void* parent,
                                     const char* file,
                                     const hs_config* cfg,
                                     const hs_sandbox_config* sbc,
                                     lua_State* env);

int hs_init_output_sandbox(hs_sandbox* sb);
#endif
