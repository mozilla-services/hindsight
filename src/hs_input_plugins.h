/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight input plugins @file */

#ifndef hs_input_plugins_h_
#define hs_input_plugins_h_

#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>

#include "hs_config.h"
#include "hs_checkpoint_reader.h"
#include "hs_output.h"
#include "hs_sandbox.h"

typedef struct hs_input_plugin hs_input_plugin;
typedef struct hs_input_plugins hs_input_plugins;

struct hs_input_plugin
{
  hs_sandbox* sb;
  hs_input_plugins* plugins;
  pthread_t thread;
  int list_index;
  hs_ip_checkpoint cp;
  sem_t shutdown;
};

struct hs_input_plugins
{
  hs_input_plugin** list;
  hs_config* cfg;

  pthread_mutex_t list_lock;
  int list_cnt;
  int list_cap;

  hs_output output;
};

void hs_init_input_plugins(hs_input_plugins* plugins,
                           hs_config* cfg);
void hs_free_input_plugins(hs_input_plugins* plugins);

void hs_load_input_plugins(hs_input_plugins* plugins, const hs_config* cfg,
                           bool dynamic);

void hs_stop_input_plugins(hs_input_plugins* plugins);
void hs_wait_input_plugins(hs_input_plugins* plugins);

hs_sandbox* hs_create_input_sandbox(void* parent,
                                    const hs_config* cfg,
                                    hs_sandbox_config* sbc);

int hs_init_input_sandbox(hs_sandbox* sb, lua_CFunction im_fp);

#endif
