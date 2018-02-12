/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight configuration implementation @file */

#include "hs_config.h"

#include <luasandbox/lauxlib.h>
#include <luasandbox/lua.h>
#include <luasandbox/util/util.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hs_logger.h"
#include "hs_util.h"

const char *hs_input_dir    = "input";
const char *hs_analysis_dir = "analysis";
const char *hs_output_dir   = "output";
const char *hs_lua_ext      = ".lua";
const char *hs_cfg_ext      = ".cfg";
const char *hs_off_ext      = ".off";
const char *hs_err_ext      = ".err";
const char *hs_rtc_ext      = ".rtc";

static const char g_module[] = "config_parser";
static const char *g_queue_options[] = { "both", "input", "analysis", NULL };

static const char *cfg_output_path = "output_path";
static const char *cfg_output_size = "output_size";
static const char *cfg_load_path = "sandbox_load_path";
static const char *cfg_run_path = "sandbox_run_path";
static const char *cfg_install_path = "sandbox_install_path";
static const char *cfg_threads = "analysis_threads";
static const char *cfg_analysis_lua_path = "analysis_lua_path";
static const char *cfg_analysis_lua_cpath = "analysis_lua_cpath";
static const char *cfg_analysis_utilization_limit = "analysis_utilization_limit";
static const char *cfg_io_lua_path = "io_lua_path";
static const char *cfg_io_lua_cpath = "io_lua_cpath";
static const char *cfg_max_message_size = "max_message_size";
static const char *cfg_hostname = "hostname";
static const char *cfg_backpressure = "backpressure";
static const char *cfg_backpressure_df = "backpressure_disk_free";

static const char *cfg_sb_ipd = "input_defaults";
static const char *cfg_sb_apd = "analysis_defaults";
static const char *cfg_sb_opd = "output_defaults";
static const char *cfg_sb_output = "output_limit";
static const char *cfg_sb_memory = "memory_limit";
static const char *cfg_sb_instruction = "instruction_limit";
static const char *cfg_sb_preserve = "preserve_data";
static const char *cfg_sb_restricted_headers = "restricted_headers";
static const char *cfg_sb_filename = "filename";
static const char *cfg_sb_ticker_interval = "ticker_interval";
static const char *cfg_sb_thread = "thread";
static const char *cfg_sb_async_buffer = "async_buffer_size";
static const char *cfg_sb_matcher = "message_matcher";
static const char *cfg_sb_shutdown_terminate = "shutdown_on_terminate";
static const char *cfg_sb_rm_cp_terminate = "remove_checkpoints_on_terminate";
static const char *cfg_sb_pm_im_limit = "process_message_inject_limit";
static const char *cfg_sb_te_im_limit = "timer_event_inject_limit";
static const char *cfg_sb_read_queue = "read_queue";

static void init_sandbox_config(hs_sandbox_config *cfg)
{
  cfg->dir = NULL;
  cfg->filename = NULL;
  cfg->cfg_name = NULL;
  cfg->cfg_lua = NULL;
  cfg->message_matcher = NULL;

  cfg->thread = UINT_MAX;
  cfg->async_buffer_size = 0;
  cfg->output_limit = 1024 * 64;
  cfg->memory_limit = 1024 * 1024 * 8;
  cfg->instruction_limit = 1000000;
  cfg->ticker_interval = 0;

  cfg->preserve_data = false;
  cfg->restricted_headers = true;
  cfg->shutdown_terminate = false;
  cfg->rm_cp_terminate = false;
  cfg->read_queue = 'b';

  cfg->pm_im_limit = 0;
  cfg->te_im_limit = 10;
}


static void init_config(hs_config *cfg)
{
  cfg->run_path = NULL;
  cfg->run_path_input = NULL;
  cfg->run_path_analysis = NULL;
  cfg->run_path_output = NULL;
  cfg->load_path = NULL;
  cfg->load_path_input = NULL;
  cfg->load_path_analysis = NULL;
  cfg->load_path_output = NULL;
  cfg->output_path = NULL;
  cfg->install_path = NULL;
  cfg->io_lua_path = NULL;
  cfg->io_lua_cpath = NULL;
  cfg->analysis_lua_path = NULL;
  cfg->analysis_lua_cpath = NULL;
  cfg->hostname = NULL;
  cfg->output_size = 1024 * 1024 * 64;
  cfg->analysis_threads = 1;
  cfg->analysis_utilization_limit = 95;
  cfg->max_message_size = 1024 * 64;
  cfg->backpressure = 0;
  cfg->backpressure_df = 4;
  cfg->pid = (int)getpid();
  init_sandbox_config(&cfg->ipd);
  init_sandbox_config(&cfg->apd);
  init_sandbox_config(&cfg->opd);

  cfg->ipd.restricted_headers = false;
  cfg->opd.restricted_headers = false;
}


static int check_for_unknown_options(lua_State *L, int idx, const char *parent)
{
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    switch (lua_type(L, -2)) {
    case LUA_TSTRING:
      if (parent) {
        lua_pushfstring(L, "invalid option: '%s.%s'", parent,
                        lua_tostring(L, -2));
      } else {
        lua_pushfstring(L, "invalid option: '%s'", lua_tostring(L, -2));
      }
      return 1;
    default:
      lua_pushstring(L, "non string key");
      return 1;;
    }
  }
  return 0;
}


static void remove_item(lua_State *L, int idx, const char *name)
{
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setfield(L, idx, name);
}


static int get_string_item(lua_State *L, int idx, const char *name, char **val,
                           const char *dflt)
{
  size_t len;
  lua_getfield(L, idx, name);
  const char *tmp = lua_tolstring(L, -1, &len);
  if (!tmp) {
    if (!dflt) {
      lua_pushfstring(L, "%s must be set to a string", name);
      return 1;
    }
    len = strlen(dflt);
    tmp = dflt;
  }
  *val = malloc(len + 1);
  if (!*val) {
    hs_log(NULL, g_module, 0, "%s malloc failed", __func__);
    exit(EXIT_FAILURE);
  }
  memcpy(*val, tmp, len + 1);
  remove_item(L, idx, name);
  return 0;
}


static int get_unsigned_int(lua_State *L, int idx, const char *name,
                            unsigned *val)
{
  lua_getfield(L, idx, name);
  int t = lua_type(L, -1);
  double d;
  switch (t) {
  case LUA_TNUMBER:
    d = lua_tonumber(L, -1);
    if (d < 0 || d > UINT_MAX) {
      lua_pushfstring(L, "%s must be an unsigned int", name);
      return 1;
    }
    *val = (unsigned)d;
    break;
  case LUA_TNIL:
    break; // use the default
  default:
    lua_pushfstring(L, "%s must be set to a number", name);
    return 1;
  }
  remove_item(L, idx, name);
  return 0;
}


static int get_uint8(lua_State *L, int idx, const char *name,
                     uint8_t *val)
{
  lua_getfield(L, idx, name);
  int t = lua_type(L, -1);
  double d;
  switch (t) {
  case LUA_TNUMBER:
    d = lua_tonumber(L, -1);
    if (d < 0 || d > UINT8_MAX) {
      lua_pushfstring(L, "%s must be a uint8_t", name);
      return 1;
    }
    *val = (uint8_t)d;
    break;
  case LUA_TNIL:
    break; // use the default
  default:
    lua_pushfstring(L, "%s must be set to a number", name);
    return 1;
  }
  remove_item(L, idx, name);
  return 0;
}


static int get_option_char(lua_State *L, int idx, const char *name,
                           char *val, const char *options[])
{
  lua_getfield(L, idx, name);
  int t = lua_type(L, -1);
  switch (t) {
  case LUA_TSTRING:
    {
      bool found = false;
      const char *s = lua_tostring(L, -1);
      for (const char **p = options; *p != NULL && found != true; ++p) {
        if (strcmp(*p, s) == 0) {
          *val = s[0];
          found = true;
        }
      }
      if (!found) {
        lua_pushfstring(L, "%s invalid option %s", name, s);
        return 1;
      }
    }
    break;
  case LUA_TNIL:
    break; // use the default
  default:
    lua_pushfstring(L, "%s must be set to a string", name);
    return 1;
  }
  remove_item(L, idx, name);
  return 0;
}


static int get_bool_item(lua_State *L, int idx, const char *name, bool *val)
{
  lua_getfield(L, idx, name);
  int t = lua_type(L, -1);
  switch (t) {
  case LUA_TBOOLEAN:
    *val = (bool)lua_toboolean(L, -1);
    break;
  case LUA_TNIL:
    break; // use the default
  default:
    lua_pushfstring(L, "%s must be set to a bool", cfg_sb_preserve);
    return 1;
  }
  remove_item(L, idx, name);
  return 0;
}


static int load_sandbox_defaults(lua_State *L,
                                 const char *key,
                                 hs_sandbox_config *cfg)
{
  lua_getglobal(L, key);
  if (!lua_istable(L, -1)) {
    lua_pushfstring(L, "%s must be a table", key);
    return 1;
  }
  if (get_unsigned_int(L, 1, cfg_sb_output, &cfg->output_limit)) return 1;
  if (get_unsigned_int(L, 1, cfg_sb_memory, &cfg->memory_limit)) return 1;
  if (get_unsigned_int(L, 1, cfg_sb_instruction, &cfg->instruction_limit)) {
    return 1;
  }
  if (get_unsigned_int(L, 1, cfg_sb_ticker_interval, &cfg->ticker_interval)) {
    return 1;
  }
  if (get_bool_item(L, 1, cfg_sb_preserve, &cfg->preserve_data)) return 1;

  if (get_bool_item(L, 1, cfg_sb_restricted_headers,
                    &cfg->restricted_headers)) {
    return 1;
  }

  if (get_bool_item(L, 1, cfg_sb_shutdown_terminate,
                    &cfg->shutdown_terminate)) {
    return 1;
  }

  if (strcmp(key, cfg_sb_apd) == 0) {
    if (get_unsigned_int(L, 1, cfg_sb_pm_im_limit, &cfg->pm_im_limit)) {
      return 1;
    }
    if (get_unsigned_int(L, 1, cfg_sb_te_im_limit, &cfg->te_im_limit)) {
      return 1;
    }
  }

  if (strcmp(key, cfg_sb_opd) == 0) {
    if (get_bool_item(L, 1, cfg_sb_rm_cp_terminate, &cfg->rm_cp_terminate)) {
      return 1;
    }
    if (get_option_char(L, 1, cfg_sb_read_queue, &cfg->read_queue,
                        g_queue_options)) {
      return 1;
    }
  }

  if (check_for_unknown_options(L, 1, key)) return 1;

  remove_item(L, LUA_GLOBALSINDEX, key);
  return 0;
}


void hs_free_sandbox_config(hs_sandbox_config *cfg)
{
  free(cfg->dir);
  cfg->dir = NULL;

  free(cfg->filename);
  cfg->filename = NULL;

  free(cfg->cfg_name);
  cfg->cfg_name = NULL;

  free(cfg->cfg_lua);
  cfg->cfg_lua = NULL;

  free(cfg->message_matcher);
  cfg->message_matcher = NULL;
}


void hs_free_config(hs_config *cfg)
{
  free(cfg->run_path);
  cfg->run_path = NULL;

  free(cfg->run_path_input);
  cfg->run_path_input = NULL;

  free(cfg->run_path_analysis);
  cfg->run_path_analysis = NULL;

  free(cfg->run_path_output);
  cfg->run_path_output = NULL;

  free(cfg->load_path);
  cfg->load_path = NULL;

  free(cfg->load_path_input);
  cfg->load_path_input = NULL;

  free(cfg->load_path_analysis);
  cfg->load_path_analysis = NULL;

  free(cfg->load_path_output);
  cfg->load_path_output = NULL;

  free(cfg->output_path);
  cfg->output_path = NULL;

  free(cfg->install_path);
  cfg->install_path = NULL;

  free(cfg->io_lua_path);
  cfg->io_lua_path = NULL;

  free(cfg->io_lua_cpath);
  cfg->io_lua_cpath = NULL;

  free(cfg->analysis_lua_path);
  cfg->analysis_lua_path = NULL;

  free(cfg->analysis_lua_cpath);
  cfg->analysis_lua_cpath = NULL;

  free(cfg->hostname);
  cfg->hostname = NULL;

  hs_free_sandbox_config(&cfg->ipd);
  hs_free_sandbox_config(&cfg->apd);
  hs_free_sandbox_config(&cfg->opd);
}


static char* create_name(const char *prefix, const char *fn)
{
  size_t ne_len = strlen(fn) - HS_EXT_LEN;
  size_t len = strlen(prefix) + ne_len + 2;
  char *name = malloc(len);
  if (!name) return NULL;

  int ret = snprintf(name, len, "%s.%.*s", prefix, (int)ne_len, fn);
  if (ret < 0 || ret > (int)len - 1) {
    free(name);
    return NULL;
  }
  return name;
}


bool hs_load_sandbox_config(const char *dir,
                            const char *fn,
                            hs_sandbox_config *cfg,
                            const hs_sandbox_config *dflt,
                            char type)
{
  if (!cfg) return false;

  char fqfn[HS_MAX_PATH];
  if (hs_has_ext(fn, hs_cfg_ext)) {
    if (hs_get_fqfn(dir, fn, fqfn, sizeof(fqfn))) {
      return false;
    }
  } else if (hs_has_ext(fn, hs_err_ext) || hs_has_ext(fn, hs_rtc_ext)) {
    // todo the rtc check can be removed after the migration Issue #128
    if (!hs_get_fqfn(dir, fn, fqfn, sizeof(fqfn))) {
      unlink(fqfn);
    }
    return false;
  } else {
    return false;
  }

  init_sandbox_config(cfg);
  cfg->cfg_lua = lsb_read_file(fqfn);
  if (!cfg->cfg_lua) return false;

  lua_State *L = luaL_newstate();
  if (!L) {
    hs_log(NULL, g_module, 3, "luaL_newstate failed: %s", fn);
    return false;
  }

  if (dflt) {
    cfg->output_limit = dflt->output_limit;
    cfg->memory_limit = dflt->memory_limit;
    cfg->instruction_limit = dflt->instruction_limit;
    cfg->ticker_interval = dflt->ticker_interval;
    cfg->preserve_data = dflt->preserve_data;
    cfg->restricted_headers = dflt->restricted_headers;
    cfg->shutdown_terminate = dflt->shutdown_terminate;
    cfg->rm_cp_terminate = dflt->rm_cp_terminate;
    cfg->pm_im_limit = dflt->pm_im_limit;
    cfg->te_im_limit = dflt->te_im_limit;
    cfg->read_queue = dflt->read_queue;
  }

  int ret = luaL_dostring(L, cfg->cfg_lua);
  if (ret) goto cleanup;

  size_t len = strlen(dir) + 1;
  cfg->dir = malloc(len);
  if (!cfg->dir) {
    ret = 1;
    goto cleanup;
  }
  memcpy(cfg->dir, dir, len);

  if (type == 'i') {
    cfg->cfg_name = create_name("input", fn);
  } else if (type == 'o') {
    cfg->cfg_name = create_name("output", fn);
  } else {
    cfg->cfg_name = create_name("analysis", fn);
  }
  if (!cfg->cfg_name) {
    lua_pushstring(L, "name allocation failed");
    ret = 1;
    goto cleanup;
  }

  ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_sb_output,
                         &cfg->output_limit);
  if (ret) goto cleanup;

  ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_sb_memory,
                         &cfg->memory_limit);
  if (ret) goto cleanup;

  ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_sb_instruction,
                         &cfg->instruction_limit);
  if (ret) goto cleanup;

  ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_sb_ticker_interval,
                         &cfg->ticker_interval);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_sb_filename, &cfg->filename,
                        NULL);
  if (!ret) {
    if (strpbrk(cfg->filename, "/\\")) {
      lua_pushfstring(L, "%s must not contain a path component",
                      cfg_sb_filename);
      ret = 1;
    } else if (!hs_has_ext(cfg->filename, hs_lua_ext)) {
      lua_pushfstring(L, "%s must have a %s extension", hs_lua_ext,
                      cfg_sb_filename);
      ret = 1;
    }
  }
  if (ret) goto cleanup;

  ret = get_bool_item(L, LUA_GLOBALSINDEX, cfg_sb_preserve,
                      &cfg->preserve_data);
  if (ret) goto cleanup;

  ret = get_bool_item(L, LUA_GLOBALSINDEX, cfg_sb_restricted_headers,
                      &cfg->restricted_headers);
  if (ret) goto cleanup;

  ret = get_bool_item(L, LUA_GLOBALSINDEX, cfg_sb_shutdown_terminate,
                      &cfg->shutdown_terminate);
  if (ret) goto cleanup;

  if (type == 'a' || type == 'o') {
    ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_sb_matcher,
                          &cfg->message_matcher, NULL);
    if (ret) goto cleanup;
  }

  if (type == 'a') {
    ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_sb_thread,
                           &cfg->thread);
    ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_sb_pm_im_limit,
                            &cfg->pm_im_limit);
    ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_sb_te_im_limit,
                            &cfg->te_im_limit);
    if (ret) goto cleanup;
  }

  if (type == 'o') {
    ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_sb_async_buffer,
                           &cfg->async_buffer_size);
    if (ret) goto cleanup;

    ret = get_bool_item(L, LUA_GLOBALSINDEX, cfg_sb_rm_cp_terminate,
                        &cfg->rm_cp_terminate);

    ret = get_option_char(L, LUA_GLOBALSINDEX, cfg_sb_read_queue,
                        &cfg->read_queue, g_queue_options);
    if (ret) goto cleanup;
  }

cleanup:
  if (ret) {
    hs_log(NULL, g_module, 3, "loading %s failed: %s", fn, lua_tostring(L, -1));
    hs_free_sandbox_config(cfg);
    return false;
  }
  lua_close(L);
  return true;
}


int hs_load_config(const char *fn, hs_config *cfg)
{
  if (!cfg) return 1;

  lua_State *L = luaL_newstate();
  if (!L) {
    hs_log(NULL, g_module, 3, "luaL_newstate failed: %s", fn);
    return 1;
  }

  init_config(cfg);

  int ret = luaL_dofile(L, fn);
  if (ret) goto cleanup;

  ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_max_message_size,
                         &cfg->max_message_size);
  if (cfg->max_message_size < 1024) {
    lua_pushfstring(L, "%s must be > 1023", cfg_max_message_size);
    ret = 1;
    goto cleanup;
  }

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_output_path,
                        &cfg->output_path, NULL);
  if (ret) goto cleanup;

  ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_output_size,
                         &cfg->output_size);
  if (ret) goto cleanup;

  ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_backpressure,
                         &cfg->backpressure);
  if (ret) goto cleanup;

  ret = get_unsigned_int(L, LUA_GLOBALSINDEX, cfg_backpressure_df,
                         &cfg->backpressure_df);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_load_path, &cfg->load_path,
                        "");
  if (ret) goto cleanup;

  ret = get_uint8(L, LUA_GLOBALSINDEX, cfg_analysis_utilization_limit,
                  &cfg->analysis_utilization_limit);
  if (cfg->analysis_utilization_limit > 100) {
    lua_pushfstring(L, "%s must be 0-100", cfg_analysis_utilization_limit);
    ret = 1;
    goto cleanup;
  }

  size_t len = strlen(cfg->load_path) + strlen(hs_input_dir) + 2;
  cfg->load_path_input = malloc(len);
  if (!cfg->load_path_input) {
    lua_pushfstring(L, "load_path_input malloc failed");
    ret = 1;
    goto cleanup;
  }
  hs_get_fqfn(cfg->load_path, hs_input_dir, cfg->load_path_input, len);

  len = strlen(cfg->load_path) + strlen(hs_analysis_dir) + 2;
  cfg->load_path_analysis = malloc(len);
  if (!cfg->load_path_analysis) {
    lua_pushfstring(L, "load_path_analysis malloc failed");
    ret = 1;
    goto cleanup;
  }
  hs_get_fqfn(cfg->load_path, hs_analysis_dir, cfg->load_path_analysis, len);

  len = strlen(cfg->load_path) + strlen(hs_output_dir) + 2;
  cfg->load_path_output = malloc(len);
  if (!cfg->load_path_output) {
    lua_pushfstring(L, "load_path_output malloc failed");
    ret = 1;
    goto cleanup;
  }
  hs_get_fqfn(cfg->load_path, hs_output_dir, cfg->load_path_output, len);

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_run_path, &cfg->run_path,
                        NULL);
  if (ret) goto cleanup;

  len = strlen(cfg->run_path) + strlen(hs_input_dir) + 2;
  cfg->run_path_input = malloc(len);
  if (!cfg->run_path_input) {
    lua_pushfstring(L, "run_path_input malloc failed");
    ret = 1;
    goto cleanup;
  }
  hs_get_fqfn(cfg->run_path, hs_input_dir, cfg->run_path_input, len);

  len = strlen(cfg->run_path) + strlen(hs_analysis_dir) + 2;
  cfg->run_path_analysis = malloc(len);
  if (!cfg->run_path_analysis) {
    lua_pushfstring(L, "run_path_analysis malloc failed");
    ret = 1;
    goto cleanup;
  }
  hs_get_fqfn(cfg->run_path, hs_analysis_dir, cfg->run_path_analysis, len);

  len = strlen(cfg->run_path) + strlen(hs_output_dir) + 2;
  cfg->run_path_output = malloc(len);
  if (!cfg->run_path_output) {
    lua_pushfstring(L, "run_path_output malloc failed");
    ret = 1;
    goto cleanup;
  }
  hs_get_fqfn(cfg->run_path, hs_output_dir, cfg->run_path_output, len);

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_install_path,
                        &cfg->install_path,
                        "/usr/share/luasandbox/sandboxes/heka");
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_io_lua_path, &cfg->io_lua_path,
                        NULL);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_io_lua_cpath,
                        &cfg->io_lua_cpath, NULL);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_analysis_lua_path,
                        &cfg->analysis_lua_path, NULL);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_analysis_lua_cpath,
                        &cfg->analysis_lua_cpath, NULL);
  if (ret) goto cleanup;

  char hostname[65] = { 0 };
  if (gethostname(hostname, sizeof(hostname))) {
    hostname[sizeof(hostname) - 1] = 0;
    hs_log(NULL, g_module, 4, "the system hostname was truncated to: %s",
           hostname);
  }

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_hostname,
                        &cfg->hostname, hostname);
  if (ret) goto cleanup;

  if (strlen(cfg->hostname) > sizeof(hostname) - 1) {
    cfg->hostname[sizeof(hostname) - 1] = 0;
    hs_log(NULL, g_module, 4, "the configured hostname was truncated to: %s",
           cfg->hostname);
  }

  ret = get_uint8(L, LUA_GLOBALSINDEX, cfg_threads,
                  &cfg->analysis_threads);
  if (cfg->analysis_threads < 1
      || cfg->analysis_threads > HS_MAX_ANALYSIS_THREADS) {
    lua_pushfstring(L, "%s must be 1-%d", cfg_threads, HS_MAX_ANALYSIS_THREADS);
    ret = 1;
    goto cleanup;
  }

  ret = load_sandbox_defaults(L, cfg_sb_ipd, &cfg->ipd);
  if (ret) goto cleanup;

  ret = load_sandbox_defaults(L, cfg_sb_apd, &cfg->apd);
  if (ret) goto cleanup;

  ret = load_sandbox_defaults(L, cfg_sb_opd, &cfg->opd);
  if (ret) goto cleanup;

  if (cfg->max_message_size < cfg->ipd.output_limit
      || cfg->max_message_size < cfg->apd.output_limit
      || cfg->max_message_size < cfg->opd.output_limit) {
    lua_pushfstring(L, "%s must be greater than or equal to the sandbox %s",
                    cfg_max_message_size, cfg_sb_output);
    ret = 1;
    goto cleanup;
  }

  ret = check_for_unknown_options(L, LUA_GLOBALSINDEX, NULL);
  if (ret) goto cleanup;

cleanup:
  if (ret) {
    hs_log(NULL, g_module, 3, "loading %s failed: %s", fn, lua_tostring(L, -1));
  }
  lua_close(L);
  return ret;
}


int hs_process_load_cfg(const char *lpath, const char *rpath, const char *name)
{
  if (hs_has_ext(name, hs_cfg_ext)) {
    char cfg_lpath[HS_MAX_PATH];
    if (hs_get_fqfn(lpath, name, cfg_lpath, sizeof(cfg_lpath))) {
      hs_log(NULL, g_module, 0, "load cfg path too long");
      exit(EXIT_FAILURE);
    }
    char cfg_rpath[HS_MAX_PATH];
    if (hs_get_fqfn(rpath, name, cfg_rpath, sizeof(cfg_rpath))) {
      hs_log(NULL, g_module, 0, "run cfg path too long");
      exit(EXIT_FAILURE);
    }

    // if the plugin was off clear the flag and prepare for restart
    char off_rpath[HS_MAX_PATH];
    strcpy(off_rpath, cfg_rpath);
    size_t pos = strlen(off_rpath) - HS_EXT_LEN;
    strcpy(off_rpath + pos, hs_off_ext);
    if (hs_file_exists(off_rpath)) {
      if (unlink(off_rpath)) {
        hs_log(NULL, g_module, 3, "failed to delete: %s errno: %d", off_rpath,
               errno);
        return -1;
      }
    }

    // if the plugin was terminated clear the error and prepare for restart
    strcpy(off_rpath + pos, hs_err_ext);
    if (hs_file_exists(off_rpath)) {
      if (unlink(off_rpath)) {
        hs_log(NULL, g_module, 3, "failed to delete: %s errno: %d", off_rpath,
               errno);
        return -1;
      }
    }

    // move the cfg to the run directory and prepare for start/restart
    if (rename(cfg_lpath, cfg_rpath)) {
      hs_log(NULL, g_module, 3, "failed to move: %s to %s errno: %d", cfg_lpath,
             cfg_rpath, errno);
      return -1;
    }
    return 1;
  } else if (hs_has_ext(name, hs_off_ext)) {
    char off_lpath[HS_MAX_PATH];
    if (hs_get_fqfn(lpath, name, off_lpath, sizeof(off_lpath))) {
      hs_log(NULL, g_module, 0, "load off path too long");
      exit(EXIT_FAILURE);
    }
    if (unlink(off_lpath)) {
      hs_log(NULL, g_module, 3, "failed to delete: %s errno: %d", off_lpath,
             errno);
      return -1;
    }

    // move the current cfg to .off and shutdown the plugin
    char off_rpath[HS_MAX_PATH];
    if (hs_get_fqfn(rpath, name, off_rpath, sizeof(off_rpath))) {
      hs_log(NULL, g_module, 0, "run off path too long");
      exit(EXIT_FAILURE);
    }
    char cfg_rpath[HS_MAX_PATH];
    strcpy(cfg_rpath, off_rpath);
    strcpy(cfg_rpath + strlen(cfg_rpath) - HS_EXT_LEN, hs_cfg_ext);
    if (rename(cfg_rpath, off_rpath)) {
      hs_log(NULL, g_module, 4, "failed to move: %s to %s errno: %d", cfg_rpath,
             off_rpath, errno);
      return -1;
    }
    return 0;
  }
  return -1;
}


bool hs_output_runtime_cfg(lsb_output_buffer *ob, char type, const hs_config *cfg,
                           hs_sandbox_config *sbc)
{
  lsb_outputf(ob, "-- original configuration\n");
  lsb_outputf(ob, "%s\n", sbc->cfg_lua);

  lsb_outputf(ob, "-- Hindsight defaults and overrides\n");
  lsb_outputf(ob, "Hostname = [[%s]]\n", cfg->hostname);
  lsb_outputf(ob, "Pid = %d\n", cfg->pid);
  lsb_outputf(ob, "log_level = %d\n", hs_get_log_level());
  if (type == 'a') {
    lsb_outputf(ob, "path = [[%s]]\n", cfg->analysis_lua_path);
    lsb_outputf(ob, "cpath = [[%s]]\n", cfg->analysis_lua_cpath);
  } else {
    lsb_outputf(ob, "path = [[%s]]\n", cfg->io_lua_path);
    lsb_outputf(ob, "cpath = [[%s]]\n", cfg->io_lua_cpath);
    lsb_outputf(ob, "output_path = [[%s]]\n", cfg->output_path);
    lsb_outputf(ob, "output_size = %u\n", cfg->output_size);
    lsb_outputf(ob, "max_message_size = %u\n", cfg->max_message_size);
    lsb_outputf(ob, "sandbox_load_path = [[%s]]\n", cfg->load_path);
    lsb_outputf(ob, "sandbox_run_path = [[%s]]\n", cfg->run_path);
    lsb_outputf(ob, "sandbox_install_path = [[%s]]\n", cfg->install_path);
  }

  lsb_outputf(ob, "\n-- Sandbox defaults and overrides\n");
  lsb_outputf(ob, "Logger = [[%s]]\n", sbc->cfg_name);
  lsb_outputf(ob, "output_limit = %u\n", sbc->output_limit);
  lsb_outputf(ob, "memory_limit = %u\n", sbc->memory_limit);
  lsb_outputf(ob, "instruction_limit = %u\n", sbc->instruction_limit);
  lsb_outputf(ob, "ticker_interval = %u\n", sbc->ticker_interval);
  lsb_outputf(ob, "preserve_data = %s\n",
              sbc->preserve_data ? "true" : "false");
  lsb_outputf(ob, "restricted_headers = %s\n",
              sbc->restricted_headers ? "true" : "false");
  lsb_outputf(ob, "shutdown_on_terminate = %s\n",
              sbc->shutdown_terminate ? "true" : "false");

  if (type == 'a') {
    lsb_outputf(ob, "thread = %u\n", sbc->thread);
    lsb_outputf(ob, "process_message_inject_limit = %u\n", sbc->pm_im_limit);
    lsb_outputf(ob, "timer_event_inject_limit = %u\n", sbc->te_im_limit);
  }

  if (type == 'o') {
    lsb_outputf(ob, "async_buffer_size = %u\n", sbc->async_buffer_size);
    lsb_outputf(ob, "remove_checkpoints_on_terminate = %s\n",
                sbc->rm_cp_terminate ? "true" : "false");
    switch (sbc->read_queue) {
    case 'i':
      lsb_outputf(ob, "read_queue = \"input\"\n");
      break;
    case 'a':
      lsb_outputf(ob, "read_queue = \"analysis\"\n");
      break;
    default:
      lsb_outputf(ob, "read_queue = \"both\"\n");
      break;
    }
  }

  // just test the last write to make sure the buffer wasn't exhausted
  lsb_err_value ret = lsb_outputf(ob, "-- end Hindsight configuration\n");

  char fn[strlen(cfg->output_path) + strlen(sbc->cfg_name) +
    strlen(hs_rtc_ext) + 2];
  snprintf(fn, sizeof(fn), "%s/%s%s", cfg->output_path, sbc->cfg_name, hs_rtc_ext);
  FILE *fh = fopen(fn, "we");
  if (!fh) return false;
  fwrite(ob->buf, ob->pos, 1, fh);
  fclose(fh);
  return ret ? false : true;
}

