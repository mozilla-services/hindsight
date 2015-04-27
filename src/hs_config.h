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

#include "hs_checkpoint_reader.h"

extern const char* hs_input_dir;
extern const char* hs_analysis_dir;

typedef enum {
  HS_SB_TYPE_UNKNOWN,
  HS_SB_TYPE_INPUT,
  HS_SB_TYPE_ANALYSIS,
  HS_SB_TYPE_OUTPUT
} hs_sb_type;

typedef struct hs_sandbox_config
{
  hs_sb_type type;
  char* module_path;
  char* filename;
  char* message_matcher; // analysis/output sandbox only

  int thread; // analysis sandbox only
  int output_limit;
  int memory_limit;
  int instruction_limit;
  bool preserve_data;
  int ticker_interval;
} hs_sandbox_config;

typedef struct hs_config
{
  char* run_path;
  char* load_path;
  char* output_path;
  hs_checkpoint_reader cp_reader;
  int output_size;
  int analysis_threads;
  hs_sandbox_config ipd; // input plugin defaults
  hs_sandbox_config apd; // analysis plugin defaults
  hs_sandbox_config opd; // output plugin defaults
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
                                  hs_sb_type mode);

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
