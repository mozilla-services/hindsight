/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight main configuration module @file */

#ifndef hs_config_h_
#define hs_config_h_

#include <luasandbox/lua.h>
#include <luasandbox/util/output_buffer.h>
#include <stdbool.h>

#define HS_EXT_LEN 4
#define HS_MAX_PATH 260
#define HS_MAX_ANALYSIS_THREADS 64

extern const char *hs_input_dir;
extern const char *hs_analysis_dir;
extern const char *hs_output_dir;
extern const char *hs_lua_ext;
extern const char *hs_cfg_ext;
extern const char *hs_rtc_ext;
extern const char *hs_off_ext;
extern const char *hs_err_ext;


typedef struct hs_sandbox_config
{
  char *dir;
  char *filename;
  char *cfg_name;
  char *cfg_lua;
  char *message_matcher; // analysis/output sandbox only

  unsigned thread; // analysis sandbox only
  unsigned async_buffer_size; // output sandbox only
  unsigned output_limit;
  unsigned memory_limit;
  unsigned instruction_limit;
  unsigned ticker_interval;

  bool preserve_data;
  bool restricted_headers;
  bool shutdown_terminate;
  bool rm_cp_terminate;   // output sandbox only

  char read_queue;           // output sandbox only
  unsigned char pm_im_limit; // analysis sandbox only
  unsigned char te_im_limit; // analysis sandbox only
} hs_sandbox_config;

typedef struct hs_config
{
  char *run_path;
  char *run_path_input;
  char *run_path_analysis;
  char *run_path_output;
  char *load_path;
  char *load_path_input;
  char *load_path_analysis;
  char *load_path_output;
  char *output_path;
  char *install_path;
  char *io_lua_path;
  char *io_lua_cpath;
  char *analysis_lua_path;
  char *analysis_lua_cpath;
  char *hostname;

  unsigned max_message_size;
  unsigned output_size;
  unsigned analysis_threads;
  unsigned backpressure;
  unsigned backpressure_df;
  int      pid;

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
void hs_free_sandbox_config(hs_sandbox_config *cfg);

/**
 * Free any memory allocated by the configuration
 *
 * @param cfg Configuration structure to free
 *
 */
void hs_free_config(hs_config *cfg);

/**
 * Loads the sandbox configuration from a file
 *
 * @param dir Run or Load path
 * @param fn Filename with proper extension
 * @param cfg Configuration structure to populate
 * @param dflt Default sandbox setting
 * @param type Type of sandbox 'i', 'a', 'o'
 *
 * @return bool false on failure
 */
bool hs_load_sandbox_config(const char *dir,
                            const char *fn,
                            hs_sandbox_config *cfg,
                            const hs_sandbox_config *dflt,
                            char type);

/**
 * Loads the Hinsight configuration from a file
 *
 * @param fn Filename
 * @param cfg Configuration structure to populate
 *
 * @return int 0 on success
 */
int hs_load_config(const char *fn, hs_config *cfg);

int hs_process_load_cfg(const char *lpath, const char *rpath, const char *name);


bool hs_output_runtime_cfg(lsb_output_buffer *ob, char type,
                           const hs_config *cfg, hs_sandbox_config *sbc);

#endif
