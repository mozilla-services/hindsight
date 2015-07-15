/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Sandbox implementation @file */

#include "hs_sandbox.h"

#include <ctype.h>
#include <math.h>
#include <luasandbox/lauxlib.h>
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
  if (env) {
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
  lua_pushstring(sb, sbc->filename);
  lua_setfield(sb, -2, "filename");
  lua_pushinteger(sb, sbc->ticker_interval);
  lua_setfield(sb, -2, "ticker_interval");

  // add the table as the environment for the read_config function
  lua_setfenv(sb, -2);
  lua_setglobal(sb, "read_config");
}


hs_sandbox* hs_create_sandbox(void* parent,
                              const char* file,
                              const char* lsb_config,
                              const hs_sandbox_config* sbc,
                              lua_State* env)
{
  hs_sandbox* sb = calloc(1, sizeof(hs_sandbox));
  if (!sb) return NULL;

  sb->ticker_interval = sbc->ticker_interval;
  int stagger = sbc->ticker_interval > 60 ? 60 : sbc->ticker_interval;
  // distribute when the timer_events will fire
  if (stagger) {
    sb->next_timer_event = time(NULL) + rand() % stagger;
  }

  sb->lsb = lsb_create_custom(parent, file, lsb_config);
  if (!sb->lsb) {
    free(sb);
    hs_log(file, 3, "lsb_create_custom failed");
    return NULL;
  }
  populate_environment(lsb_get_lua(sb->lsb), env, sbc);
  sb->mm = NULL;
  return sb;
}


void hs_free_sandbox(hs_sandbox* sb)
{
  if (!sb) return;

  char* e = lsb_destroy(sb->lsb, sb->state);
  if (e) {
    hs_log(sb->filename, 3, "lsb_destroy() received: %s", e);
    free(e);
  }
  sb->lsb = NULL;

  free(sb->filename);
  sb->filename = NULL;

  free(sb->state);
  sb->state = NULL;

  if (sb->mm) {
    hs_free_message_matcher(sb->mm);
    free(sb->mm);
    sb->mm = NULL;
  }
}


int hs_process_message(lua_sandbox* lsb)
{
  static const char* func_name = "process_message";
  lua_State* lua = lsb_get_lua(lsb);
  if (!lua) return 1;

  if (lsb_pcall_setup(lsb, func_name)) {
    char err[LSB_ERROR_SIZE];
    snprintf(err, LSB_ERROR_SIZE, "%s() function was not found", func_name);
    lsb_terminate(lsb, err);
    return 1;
  }

  if (lua_pcall(lua, 0, 2, 0) != 0) {
    char err[LSB_ERROR_SIZE];
    size_t len = snprintf(err, LSB_ERROR_SIZE, "%s() %s", func_name,
                          lua_tostring(lua, -1));
    if (len >= LSB_ERROR_SIZE) {
      err[LSB_ERROR_SIZE - 1] = 0;
    }
    lsb_terminate(lsb, err);
    return 1;
  }

  if (lua_type(lua, 1) != LUA_TNUMBER) {
    char err[LSB_ERROR_SIZE];
    size_t len = snprintf(err, LSB_ERROR_SIZE,
                          "%s() must return a numeric status code",
                          func_name);
    if (len >= LSB_ERROR_SIZE) {
      err[LSB_ERROR_SIZE - 1] = 0;
    }
    lsb_terminate(lsb, err);
    return 1;
  }

  int status = (int)lua_tointeger(lua, 1);
  switch (lua_type(lua, 2)) {
  case LUA_TNIL:
    lsb_set_error(lsb, NULL);
    break;
  case LUA_TSTRING:
    lsb_set_error(lsb, lua_tostring(lua, 2));
    break;
  default:
    {
      char err[LSB_ERROR_SIZE];
      int len = snprintf(err, LSB_ERROR_SIZE,
                         "%s() must return a nil or string error message",
                         func_name);
      if (len >= LSB_ERROR_SIZE || len < 0) {
        err[LSB_ERROR_SIZE - 1] = 0;
      }
      lsb_terminate(lsb, err);
      return 1;
    }
    break;
  }
  lua_pop(lua, 2);
  lsb_pcall_teardown(lsb);
  return status;
}


int hs_timer_event(lua_sandbox* lsb, time_t t)
{
  static const char* func_name = "timer_event";
  lua_State* lua = lsb_get_lua(lsb);
  if (!lua) return 1;

  if (lsb_pcall_setup(lsb, func_name)) {
    char err[LSB_ERROR_SIZE];
    snprintf(err, LSB_ERROR_SIZE, "%s() function was not found", func_name);
    lsb_terminate(lsb, err);
    return 1;
  }

  lua_pushnumber(lua, t * 1e9); // todo change if we need more than 1 sec resolution
  if (lua_pcall(lua, 1, 0, 0) != 0) {
    char err[LSB_ERROR_SIZE];
    size_t len = snprintf(err, LSB_ERROR_SIZE, "%s() %s", func_name,
                          lua_tostring(lua, -1));
    if (len >= LSB_ERROR_SIZE) {
      err[LSB_ERROR_SIZE - 1] = 0;
    }
    lsb_terminate(lsb, err);
    return 1;
  }
  lsb_pcall_teardown(lsb);
  lua_gc(lua, LUA_GCCOLLECT, 0);
  return 0;
}


void hs_update_running_stats(hs_running_stats* s, double d)
{
  double old_mean = s->mean;
  double old_sum = s->sum;

  if (++s->count == 1) {
    s->mean = d;
  } else {
    s->mean = old_mean + (d - old_mean) / s->count;
    s->sum = old_sum + (d - old_mean) * (d - s->mean);
  }
}


double hs_sd_running_stats(hs_running_stats* s)
{
  if (s->count < 2) return 0;
  return sqrt(s->sum / (s->count - 1));
}

