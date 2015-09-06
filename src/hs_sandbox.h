/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Sandbox structures and functions @file */

#ifndef hs_sandbox_h_
#define hs_sandbox_h_

#include <luasandbox.h>
#include <time.h>

#include "hs_config.h"
#include "hs_message_matcher.h"


typedef struct hs_running_stats
{
  double count;
  double mean;
  double sum;
} hs_running_stats;


typedef struct hs_sandbox_stats
{
  size_t im_cnt;
  size_t im_bytes;

  size_t pm_cnt;
  size_t pm_failures;

  unsigned cur_memory;
  unsigned max_memory;
  unsigned max_output;
  unsigned max_instructions;

  hs_running_stats mm;
  hs_running_stats pm;
  hs_running_stats te;
} hs_sandbox_stats;


typedef struct hs_sandbox
{
  lua_sandbox* lsb;
  char* filename;
  char* state;
  hs_message_matcher* mm;
  lua_CFunction im_fp;
  int ticker_interval;
  time_t next_timer_event;
  hs_sandbox_stats stats;
} hs_sandbox;

hs_sandbox* hs_create_sandbox(void* parent,
                              const char* file,
                              const char* lsb_config,
                              const hs_sandbox_config* sbc,
                              lua_State* env);

void hs_free_sandbox(hs_sandbox* p);


int hs_process_message(lua_sandbox* lsb, void* sequence_id);
int hs_timer_event(lua_sandbox* lsb, time_t t);
void hs_update_running_stats(hs_running_stats* s, double d);
double hs_sd_running_stats(hs_running_stats* s);

#endif
