/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Sandbox implementation @file */

#include "hs_sandbox.h"

#include <ctype.h>
#include <lauxlib.h>
#include <stdlib.h>

#include "hs_logger.h"


static int read_config(lua_State* L)
{
  luaL_checkstring(L, 1);
  luaL_argcheck(L, lua_gettop(L) == 1, 0, "too many arguments");
  lua_getfield(L, LUA_ENVIRONINDEX, lua_tostring(L, 1));
  return 1;
}


static void populate_environment(lua_State* sb,
                                 lua_State* env,
                                 const hs_sandbox_config* sbc)
{
  lua_pushcclosure(sb, read_config, 0);
  lua_newtable(sb);

  // load the user provided configuration variables
  lua_pushnil(env);
  while (lua_next(env, LUA_GLOBALSINDEX) != 0) {
    int kt = lua_type(env, -2);
    int vt = lua_type(env, -1);
    switch (kt) {
    case LUA_TSTRING:
      switch (vt) {
      case LUA_TSTRING:
        {
          size_t len;
          const char* tmp = lua_tolstring(env, -1, &len);
          if (tmp) {
            lua_pushlstring(sb, tmp, len);
            lua_setfield(sb, -2, lua_tostring(env, -2));
          }
        }
        break;
      case LUA_TNUMBER:
        lua_pushnumber(sb, lua_tonumber(env, -1));
        lua_setfield(sb, -2, lua_tostring(env, -2));
        break;
      case LUA_TBOOLEAN:
        lua_pushboolean(sb, lua_tonumber(env, -1));
        lua_setfield(sb, -2, lua_tostring(env, -2));
        break;
      default:
        hs_log(sbc->filename, 4, "skipping config value type: %s",
               lua_typename(env, vt));
        break;
      }
      break;
    default:
      hs_log(sbc->filename, 4, "skipping config key type: %s",
             lua_typename(env, kt));
      break;
    }
    lua_pop(env, 1);
  }

  // load the known configuration variables
  lua_pushinteger(sb, sbc->output_limit);
  lua_setfield(sb, -2, "output_limit");
  lua_pushinteger(sb, sbc->memory_limit);
  lua_setfield(sb, -2, "memory_limit");
  lua_pushinteger(sb, sbc->instruction_limit);
  lua_setfield(sb, -2, "instruction_limit");
  lua_pushboolean(sb, sbc->preserve_data);
  lua_setfield(sb, -2, "preserve_data");
  lua_pushstring(sb, sbc->module_path);
  lua_setfield(sb, -2, "module_path");
  lua_pushstring(sb, sbc->filename);
  lua_setfield(sb, -2, "filename");
  lua_pushinteger(sb, sbc->ticker_interval);
  lua_setfield(sb, -2, "ticker_interval");

  // add the table as the environment for the read_config function
  lua_setfenv(sb, -2);
  lua_setglobal(sb, "read_config");
}


static int expand_path(const char* path, int n, const char* fmt, char* opath)
{
  // not an exhaustive check, just making sure the Lua markers are accounted for
  for (int i = 0; (path[i]); ++i) {
    if (path[i] == '\'' || path[i] == ';' || path[i] == '?'
        || !isprint(path[i])) {
      return 1;
    }
  }
  int result = snprintf(opath, n, fmt, path);
  if (result < 0 || result > n - 1) {
    return 1;
  }
  return 0;
}


hs_sandbox* hs_create_sandbox(void* parent,
                              const char* file,
                              const char* cfg_template,
                              const hs_sandbox_config* cfg,
                              lua_State* env)
{

  char config[1024 * 2];
  char lpath[260] = { 0 };
  char cpath[260] = { 0 };

  if (cfg->module_path) {
#if defined(_WIN32)
    if (expand_path(cfg->module_path, sizeof(lpath), "%s\\?.lua", lpath)) {
      return NULL;
    }
    if (expand_path(cfg->module_path, sizeof(cpath), "%s\\?.dll", cpath)) {
      return NULL;
    }
#else
    if (expand_path(cfg->module_path, sizeof(lpath), "%s/?.lua", lpath)) {
      return NULL;
    }
    if (expand_path(cfg->module_path, sizeof(cpath), "%s/?.so", cpath)) {
      return NULL;
    }
#endif
  }

  int ret = snprintf(config, sizeof(config), cfg_template,
                     cfg->memory_limit,
                     cfg->instruction_limit,
                     cfg->output_limit,
                     lpath,
                     cpath);

  if (ret < 0 || ret > (int)sizeof(config) - 1) {
    return NULL;
  }

  hs_sandbox* p = calloc(1, sizeof(hs_sandbox));
  if (!p) return NULL;

  p->ticker_interval = cfg->ticker_interval;

  p->lsb = lsb_create_custom(parent, file, config);
  if (!p->lsb) {
    free(p);
    hs_log(file, 3, "lsb_create_custom failed");
    return NULL;
  }
  populate_environment(lsb_get_lua(p->lsb), env, cfg);
  p->matcher = NULL;
  return p;
}


void hs_free_sandbox(hs_sandbox* p)
{
  if (!p) return;

  char* e = lsb_destroy(p->lsb, NULL);
  if (e) {
    hs_log(p->filename, 3, "lsb_destroy() received: %s", e);
    free(e);
  }
  p->lsb = NULL;

  free(p->filename);
  p->filename = NULL;

  free(p->state);
  p->state = NULL;

  if (p->matcher) {
    hs_free_message_matcher(p->matcher);
    free(p->matcher);
    p->matcher = NULL;
  }
}
