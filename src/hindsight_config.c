/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight configuration implementation @file */

#include "hindsight_config.h"

#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* cfg_mode = "mode";
static const char* cfg_output_path = "output_path";
static const char* cfg_output_size = "output_size";
static const char* cfg_load_path = "sandbox_load_path";
static const char* cfg_run_path = "sandbox_run_path";

static const char* cfg_sb = "sandbox_defaults";
static const char* cfg_sb_output = "output_limit";
static const char* cfg_sb_memory = "memory_limit";
static const char* cfg_sb_instruction = "instruction_limit";
static const char* cfg_sb_preserve = "preserve_data";
static const char* cfg_sb_module = "module_path";
static const char* cfg_sb_filename = "filename";

static void init_sandbox_config(sandbox_config* cfg)
{
  cfg->output_limit = 1024 * 64;
  cfg->memory_limit = 1024 * 1024 * 8;
  cfg->instruction_limit = 1000000;
  cfg->preserve_data = false;
  cfg->module_path = NULL;
  cfg->filename = NULL;
}


static void init_config(hindsight_config* cfg)
{
  cfg->mode = HS_MODE_UNKNOWN;
  cfg->run_path = NULL;
  cfg->load_path = NULL;
  cfg->output_path = NULL;
  cfg->output_size = 1024 * 1024 * 64;
  init_sandbox_config(&cfg->sbc);
}


static int check_for_unknown_options(lua_State* L, int idx, const char* parent)
{
  int cnt = 0;

  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    switch (lua_type(L, -2)) {
    case LUA_TSTRING:
      fprintf(stderr, "invalid option '%s%s'\n", parent, lua_tostring(L, -2));
      break;
    default:
      fprintf(stderr, "non string key\n");
      break;
    }
    ++cnt;
    lua_pop(L, 1);
  }
  return cnt;
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
      fprintf(stderr, "%s must be set to a string\n", name);
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


static int get_numeric_item(lua_State* L, int idx, const char* name, int* val)
{
  lua_getfield(L, idx, name);
  int t = lua_type(L, -1);
  switch (t) {
  case LUA_TNUMBER:
    *val = lua_tonumber(L, -1);
    if (*val < 0) {
      fprintf(stderr, "%s must be set to a positive number\n", name);
      return 1;
    }
    break;
  case LUA_TNIL:
    break; // use the default
  default:
    fprintf(stderr, "%s must be set to a number\n", name);
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
    fprintf(stderr, "%s must be set to a bool\n", cfg_sb_preserve);
    return 1;
  }
  remove_item(L, idx, name);

  return 0;
}


static int load_sandbox_defaults(lua_State* L, sandbox_config* cfg)
{
  lua_getglobal(L, cfg_sb);
  if (!lua_istable(L, -1)) {
    fprintf(stderr, "%s must be a table\n", cfg_sb);
    return 1;
  }
  if (get_numeric_item(L, 1, cfg_sb_output, &cfg->output_limit)) return 1;
  if (get_numeric_item(L, 1, cfg_sb_memory, &cfg->memory_limit)) return 1;
  if (get_numeric_item(L, 1, cfg_sb_instruction, &cfg->instruction_limit)) {
    return 1;
  }
  if (get_string_item(L, 1, cfg_sb_module, &cfg->module_path, NULL)) return 1;
  if (get_bool_item(L, 1, cfg_sb_preserve, &cfg->preserve_data)) return 1;

  if (check_for_unknown_options(L, 1, "sandbox_default.")) return 1;

  remove_item(L, LUA_GLOBALSINDEX, cfg_sb);

  return 0;
}


void hs_free_sandbox_config(sandbox_config* cfg)
{
  free(cfg->module_path);
  cfg->module_path = NULL;

  free(cfg->filename);
  cfg->filename = NULL;
}


void hs_free_config(hindsight_config* cfg)
{
  free(cfg->run_path);
  cfg->run_path = NULL;

  free(cfg->load_path);
  cfg->load_path = NULL;

  free(cfg->output_path);
  cfg->output_path = NULL;

  hs_free_sandbox_config(&cfg->sbc);
}

lua_State* hs_load_sandbox_config(const char* fn,
                                  sandbox_config* cfg,
                                  const sandbox_config* dflt)
{
  if (!cfg) return NULL;

  lua_State* L = luaL_newstate();
  if (!L) {
    fprintf(stderr, "luaL_newstate failed\n");
    return NULL;
  }

  int ret = 0;
  init_sandbox_config(cfg);
  char* module_path = NULL;
  if (dflt) {
    cfg->output_limit = dflt->output_limit;
    cfg->memory_limit = dflt->memory_limit;
    cfg->instruction_limit = dflt->instruction_limit;
    cfg->preserve_data = dflt->preserve_data;
    module_path = dflt->module_path;
  }

  if (luaL_dofile(L, fn)) {
    fprintf(stderr, "Loading %s failed: %s\n", fn, lua_tostring(L, -1));
    ret = 1;
    goto cleanup;
  }

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_sb_output,
                         &cfg->output_limit);
  if (ret) goto cleanup;

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_sb_memory,
                         &cfg->memory_limit);
  if (ret) goto cleanup;

  ret = get_numeric_item(L, LUA_GLOBALSINDEX, cfg_sb_instruction,
                         &cfg->instruction_limit);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_sb_filename, &cfg->filename,
                        NULL);
  if (ret) goto cleanup;

  ret = get_string_item(L, LUA_GLOBALSINDEX, cfg_sb_module, &cfg->module_path,
                        module_path);
  if (ret) goto cleanup;

  ret = get_bool_item(L, LUA_GLOBALSINDEX, cfg_sb_preserve,
                      &cfg->preserve_data);

cleanup:
  if (ret) {
    lua_close(L);
    return NULL;
  }

  return L;
}


int hs_load_config(const char* fn, hindsight_config* cfg)
{
  if (!cfg) return 1;

  lua_State* L = luaL_newstate();
  if (!L) {
    fprintf(stderr, "luaL_newstate failed\n");
    return 1;
  }

  int ret = 0;
  init_config(cfg);
  if (luaL_dofile(L, fn)) {
    fprintf(stderr, "Loading %s failed: %s\n", fn, lua_tostring(L, -1));
    ret = 1;
    goto cleanup;
  }

  // set mode
  lua_getglobal(L, cfg_mode);
  const char* tmp = lua_tostring(L, -1);
  if (tmp && strcmp(tmp, "input") == 0) {
    cfg->mode = HS_MODE_INPUT;
  } else if (tmp && strcmp(tmp, "analysis") == 0) {
    cfg->mode = HS_MODE_ANALYSIS;
  } else if (tmp && strcmp(tmp, "output") == 0) {
    cfg->mode = HS_MODE_OUTPUT;
  } else {
    fprintf(stderr, "%s must be set to input|analysis|output\n", cfg_mode);
    ret = 1;
    goto cleanup;
  }
  remove_item(L, LUA_GLOBALSINDEX, cfg_mode);

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

  ret = load_sandbox_defaults(L, &cfg->sbc);
  if (ret) goto cleanup;

  ret = check_for_unknown_options(L, LUA_GLOBALSINDEX, "");

cleanup:
  lua_close(L);

  return ret;
}
