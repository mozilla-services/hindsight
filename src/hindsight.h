/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight common functions and structures @file */

#ifndef hindsight_h_
#define hindsight_h_

#include <lua.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "lsb.h"

typedef struct hs_plugin hs_plugin;
typedef struct hs_plugins hs_plugins;

struct hs_plugin
{
  hs_plugins* plugins;
  lua_sandbox* lsb;
  char* filename;
  char* state;
  long long cp_offset;  // numeric checkpoint
  char* cp_string;      // string checkpoint
  size_t cp_capacity;   // string checkpoint capacity
  int ticker_interval;
};

struct hs_plugins
{
  pthread_mutex_t lock;
  pthread_mutex_t* shutdown;

  hs_plugin** list;
  pthread_t* threads;
  struct hindsight_config* hs_cfg;

  FILE* output_fh;
  size_t output_offset;
  long long output_id;

  FILE* cp_fh;
  lua_State* cp_values;

  int cnt;
};


/**
 * Hindsight logging facility
 *
 * @param plugin
 * @param level
 * @param fmt
 */
void hs_log(const char* plugin, int level, const char* fmt, ...);

/**
 * Frees all resources allocated by the plugin
 *
 * @param p
 */
void hs_free_plugin(hs_plugin* p);

/**
 * Constructs a fully qualified filename from the provided components
 *
 * @param path Base path
 * @param name File name
 * @param fqfn Buffer to construct the string in
 * @param fqfn_len Length of the buffer
 *
 * @return bool true if string was successfully constructed
 */
bool hs_get_fqfn(const char* path,
                 const char* name,
                 char* fqfn,
                 size_t fqfn_len);

/**
 * Helper function to write out the plugin checkpoint info
 *
 * @param plugins
 */
void hs_write_checkpoint(hs_plugins* plugins);

/**
 * Rolls the output file
 *
 * @param plugins
 */
void hs_open_output_file(hs_plugins* plugins);

#endif
