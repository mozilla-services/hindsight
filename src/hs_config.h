/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight main configuration module @file */

#ifndef hs_config_h_
#define hs_config_h_

#include <lua.h>
#include <stdbool.h>

typedef enum {
  HS_MODE_UNKNOWN,
  HS_MODE_INPUT,
  HS_MODE_ANALYSIS,
  HS_MODE_OUTPUT,

  HS_MODE_MAX
} hs_mode;

typedef struct hs_sandbox_config
{
  char* module_path;
  char* filename;

  // analysis sandbox only
  char* message_matcher;
  int thread;
  // end analysis sandbox only

  int output_limit;
  int memory_limit;
  int instruction_limit;
  bool preserve_data;
  int ticker_interval;
} hs_sandbox_config;

typedef struct hs_config
{
  hs_mode mode;
  char* run_path;
  char* load_path;
  char* output_path;
  char* input_path; // used by analysis/output
  int output_size;
  int threads;
  hs_sandbox_config sbc;
} hs_config;


/**
 * Free any memory allocated by the configuration
 *
 * @param cfg Configuration structure to free
 *
 */
void hs_free_sandbox_config(hs_sandbox_config* cfg);

/**
 * Free any memory allocated by the configuration
 *
 * @param cfg Configuration structure to free
 *
 */
void hs_free_config(hs_config* cfg);

/**
 * Loads the sandbox configuration from a file
 *
 * @param fn Filename
 * @param cfg Configuration structure to populate
 * @param dflt
 *
 * @return int NULL on failure
 */
lua_State* hs_load_sandbox_config(const char* fn,
                                  hs_sandbox_config* cfg,
                                  const hs_sandbox_config* dflt,
                                  hs_mode mode);

/**
 * Loads the Hinsight configuration from a file
 *
 * @param fn Filename
 * @param cfg Configuration structure to populate
 *
 * @return int 0 on success
 */
int hs_load_config(const char* fn, hs_config* cfg);

bool hs_get_config_fqfn(const char* path,
                        const char* name,
                        char* fqfn,
                        size_t fqfn_len);


#endif
