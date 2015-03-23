/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight input plugins @file */

#ifndef hs_input_plugins_h_
#define hs_input_plugins_h_

#include <pthread.h>
#include <stddef.h>

#include "hs_config.h"
#include "hs_output.h"
#include "hs_sandbox.h"

typedef struct hs_input_plugin hs_input_plugin;
typedef struct hs_input_plugins hs_input_plugins;

struct hs_input_plugin
{
  hs_input_plugins* plugins;

  long long cp_offset;  // numeric checkpoint
  char* cp_string;      // string checkpoint
  size_t cp_capacity;   // string checkpoint capacity
  hs_sandbox* sb;
};

struct hs_input_plugins
{
  pthread_mutex_t lock;
  pthread_mutex_t* shutdown;
  pthread_t* threads;

  hs_input_plugin** list;
  hs_config* cfg;

  hs_output output;
  int plugin_cnt;
};

void hs_init_input_plugins(hs_input_plugins* plugins, hs_config* cfg,
                           pthread_mutex_t* shutdown);
void hs_free_input_plugins(hs_input_plugins* plugins);

void hs_load_input_plugins(hs_input_plugins* plugins, const hs_config* cfg,
                           const char* path);

void hs_stop_input_plugins(hs_input_plugins* plugins);

#endif
