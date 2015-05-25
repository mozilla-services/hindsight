/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight configuration implementation @file */

#include "hs_config.h"

#include <luasandbox/lauxlib.h>
#include <luasandbox/lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hs_logger.h"

const char* hs_input_dir = "input";
const char* hs_analysis_dir = "analysis";

static const char g_module[] = "config_parser";

static const char* cfg_output_path = "output_path";
static const char* cfg_output_size = "output_size";
static const char* cfg_load_path = "sandbox_load_path";
static const char* cfg_run_path = "sandbox_run_path";
static const char* cfg_threads = "analysis_threads";
static const char* cfg_analysis_lua_path = "analysis_lua_path";
static const char* cfg_analysis_lua_cpath = "analysis_lua_cpath";
static const char* cfg_io_lua_path = "io_lua_path";
static const char* cfg_io_lua_cpath = "io_lua_cpath";
static const char* cfg_max_message_size = "max_message_size";
static const char* cfg_hostname = "hostname";

static const char* cfg_sb_ipd = "input_defaults";
static const char* cfg_sb_apd = "analysis_defaults";
static const char* cfg_sb_opd = "output_defaults";
static const char* cfg_sb_output = "output_limit";
static const char* cfg_sb_memory = "memory_limit";
static const char* cfg_sb_instruction = "instruction_limit";
static const char* cfg_sb_preserve = "preserve_data";
static const char* cfg_sb_filename = "filename";
static const char* cfg_sb_ticker_interval = "ticker_interval";
static const char* cfg_sb_thread = "thread";
static const char* cfg_sb_matcher = "message_matcher";

static void init_sandbox_config(hs_sandbox_config* cfg)
{
  cfg->output_limit = 1024 * 64;
  cfg->memory_limit = 1024 * 1024 * 8;
  cfg->instruction_limit = 1000000;
  cfg->preserve_data = false;
  cfg->filename = NULL;
  cfg->message_matcher = NULL;
  cfg->ticker_interval = 0;
  cfg->thread = 0;
}


static void init_config(hs_config* cfg)
{
  cfg->run_path = NULL;
  cfg->load_path = NULL;
  cfg->output_path = NULL;
  cfg->io_lua_path = NULL;
  cfg->io_lua_cpath = NULL;
  cfg->analysis_lua_path = NULL;
  cfg->analysis_lua_cpath = NULL;
  cfg->hostname = NULL;
  cfg->output_size = 1024 * 1024 * 64;
  cfg->analysis_threads = 0;
  cfg->max_message_size = 1024 * 64;
  cfg->pid = (int)getpid();
  init_sandbox_config(&cfg->ipd);
  init_sandbox_config(&cfg->apd);
  init_sandbox_config(&cfg->opd);
}


static int check_for_unknown_options(lua_State* L, int idx, const char* parent)
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


static void remove_item(lua_State* L, int idx, const char* name)
{
  lua_pop(L, 1);
  lua_pushnil(L);
  lua_setfield(L, idx, name);
}


static int get_string_item(lua_State* L, int idx, const char* name, char** val,
                           const char* dflt)
{
  size_t len;
  lua_getfield(L, idx, name);
  const char* tmp = lua_tolstring(L, -1, &len);
  if (!tmp) {
    if (!dflt) {
      lua_pushfstring(L, "%s must be set to a string", name);
      return 1;
    }
    len = strlen(dflt);
    tmp = dflt;
  }
  *val = malloc(len + 1);
  memcpy(*val, tmp, len + 1);
  remove_item(L, idx, name);

  return 0;
}


static int get_numeric_item(lua_State* L, int idx, const char* name,
                            unsigned* val)
{
  lua_getfield(L, idx, name);
  int t = lua_type(L, -1);
  double d;
  switch (t) {
  case LUA_TNUMBER:
    d = lua_tonumber(L, -1);
    if (d < 0) {
      lua_pushfstring(L, "%s must be set to a positive number", name);
      return 1;
    }
    *val = (unsigned)d;
    break;
  case LUA_TNIL:
    break; // use the default
  default:
    lua_pushfstring(L, "%s must be set to a number", name);
    return 1;
    break;
  }
  remove_item(L, idx, name);

  return 0;
}


static int get_bool_item(lua_State* L, int idx, const char* name, bool* val)
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


static int load_sandbox_defaults(lua_State* L,
                                 const char* key,
                                 hs_sandbox_config* cfg)
{
  lua_getglobal(L, key);
  if (!lua_istable(L, -1)) {
    lua_pushfstring(L, "%s must be a table", key);
    return 1;
  }
  if (get_numeric_item(L, 1, cfg_sb_output, &cfg->output_limit)) return 1;
  if (get_numeric_item(L, 1, cfg_sb_memory, &cfg->memory_limit)) return 1;
  if (get_numeric_item(L, 1, cfg_sb_instruction, &cfg->instruction_limit)) {
    return 1;
  }
  if (get_numeric_item(L, 1, cfg_sb_ticker_interval, &cfg->ticker_interval)) {
    return 1;
  }
  if (get_bool_item(L, 1, cfg_sb_preserve, &cfg->preserve_data)) return 1;

  if (check_for_unknown_options(L, 1, key)) return 1;

  remove_item(L, LUA_GLOBALSINDEX, key);

  return 0;
}


void hs_free_sandbox_config(hs_sandbox_config* cfg)
{
  free(cfg->filename);
  cfg->filename = NULL;

  free(cfg->message_matcher);
  cfg->message_matcher = NULL;
}


void hs_free_config(hs_config* cfg)
{
  free(cfg->run_path);
  cfg->run_path = NULL;

  free(cfg->load_path);
  cfg->load_path = NULL;

  free(cfg->output_path);
  cfg->output_path = NULL;

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

  hs_free_checkpoint_reader(&cfg->cp_reader);
}


lua_State* hs_load_sandbox_config(const char* fn,
                                  hs_sandbox_config* cfg,
                                  const hs_sandbox_config* dflt,
                                  hs_sb_type mode)
{
  if (!cfg) return NULL;

  lua_State* L = luaL_newstate();
  if (!L) {
    hs_log(g_module, 3, "luaL_newstate failed: %s", fn);
    return NULL;
  }

  init_sandbox_config(cfg);
  if (dflt) {
    cfg->output_limit = dflt->output_limit;
    cfg->memory_limit = dflt->memory_limit;
    cfg->instruction_limit = dflt->instruction_limit;
    cfg->preserve_data = dflt->preserve_data;
  }

  int ret = luaL_dofile(L, fn);
  if (ret) goto cleanup;

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_sb_output,
                         &cfg->output_limit);
  if (ret) goto cleanup;

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_sb_memory,
                         &cfg->memory_limit);
  if (ret) goto cleanup;

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_sb_instruction,
                         &cfg->instruction_limit);
  if (ret) goto cleanup;

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_sb_ticker_interval,
                         &cfg->ticker_interval);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_sb_filename, &cfg->filename,
                        NULL);
  if (ret) goto cleanup;

  ret = get_bool_item(L, LUA_GLOBALSINDEX, cfg_sb_preserve,
                      &cfg->preserve_data);
  if (ret) goto cleanup;

  if (mode == HS_SB_TYPE_ANALYSIS || mode == HS_SB_TYPE_OUTPUT) {
    ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_sb_matcher,
                          &cfg->message_matcher, NULL);
    if (ret) goto cleanup;
  }

  if (mode == HS_SB_TYPE_ANALYSIS) {
    ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_sb_thread,
                           &cfg->thread);
    if (ret) goto cleanup;
  }

cleanup:
  if (ret) {
    hs_log(g_module, 3, "loading %s failed: %s", fn, lua_tostring(L, -1));
    lua_close(L);
    return NULL;
  }

  return L;
}


int hs_load_config(const char* fn, hs_config* cfg)
{
  if (!cfg) return 1;

  lua_State* L = luaL_newstate();
  if (!L) {
    hs_log(g_module, 3, "luaL_newstate failed: %s", fn);
    return 1;
  }

  init_config(cfg);

  int ret = luaL_dofile(L, fn);
  if (ret) goto cleanup;

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_max_message_size,
                         &cfg->max_message_size);
  if (cfg->max_message_size < 1024) {
    lua_pushfstring(L, "%s must be > 1023", cfg_max_message_size);
    ret = 1;
  }
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_output_path,
                        &cfg->output_path, NULL);
  if (ret) goto cleanup;

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_output_size,
                         &cfg->output_size);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_load_path, &cfg->load_path,
                        NULL);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_run_path, &cfg->run_path,
                        NULL);
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
    hs_log(g_module, 4, "the system hostname was trucated to: %s", hostname);
  }

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_hostname,
                        &cfg->hostname, hostname);
  if (ret) goto cleanup;

  if (strlen(cfg->hostname) > sizeof(hostname) - 1) {
    cfg->hostname[sizeof(hostname) - 1] = 0;
    hs_log(g_module, 4, "the configured hostname was trucated to: %s",
           cfg->hostname);
  }

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_threads,
                         &cfg->analysis_threads);
  if (ret) goto cleanup;

  ret = load_sandbox_defaults(L, cfg_sb_ipd, &cfg->ipd);
  if (ret) goto cleanup;

  ret = load_sandbox_defaults(L, cfg_sb_apd, &cfg->apd);
  if (ret) goto cleanup;

  ret = load_sandbox_defaults(L, cfg_sb_opd, &cfg->opd);
  if (ret) goto cleanup;

  ret = check_for_unknown_options(L, LUA_GLOBALSINDEX, NULL);
  if (ret) goto cleanup;

  hs_init_checkpoint_reader(&cfg->cp_reader, cfg->output_path);

cleanup:
  if (ret) {
    hs_log(g_module, 3, "loading %s failed: %s", fn, lua_tostring(L, -1));
  }
  lua_close(L);

  return ret;
}


bool hs_get_config_fqfn(const char* path,
                        const char* name,
                        char* fqfn,
                        size_t fqfn_len)
{
  static const size_t ext_size = 4;
  size_t len = strlen(name);
  if (len <= ext_size) return false;

  if (strcmp(".cfg", name + len - ext_size)) return false;

  int ret = snprintf(fqfn, fqfn_len, "%s/%s", path, name);
  if (ret < 0 || ret > (int)fqfn_len - 1) {
    return false;
  }
  return true;
}
