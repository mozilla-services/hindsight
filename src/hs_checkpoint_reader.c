/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight checkpoint_reader implementation @file */

#include "hs_checkpoint_reader.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <luasandbox/lauxlib.h>
#include <luasandbox/lualib.h>
#include <stdlib.h>
#include <string.h>

#include "hs_logger.h"
#include "hs_util.h"


static const char g_module[] = "checkpoint_reader";

static bool extract_id(const char* fn, size_t* id)
{
  size_t l = strlen(fn);
  size_t i = 0;
  for (; i < l && isdigit(fn[i]); ++i);
  if (i > 0 && i + 4 == l && strncmp(fn + i, ".log", 4) == 0) {
    *id = (size_t)strtoull(fn, NULL, 10);
    return true;
  }
  return false;
}


static size_t find_first_id(const char* path)
{
  struct dirent* entry;
  DIR* dp = opendir(path);
  if (dp == NULL) {
    exit(EXIT_FAILURE);
  }

  size_t file_id = ULLONG_MAX, current_id = 0;
  while ((entry = readdir(dp))) {
    if (extract_id(entry->d_name, &current_id)) {
      if (current_id < file_id) {
        file_id = current_id;
      }
    }
  }
  closedir(dp);
  return file_id == ULLONG_MAX ? 0 : file_id;
}


void hs_init_checkpoint_reader(hs_checkpoint_reader* cpr, const char* path)
{
  char fqfn[HS_MAX_PATH];
  if (!hs_get_fqfn(path, "hindsight.cp", fqfn, sizeof(fqfn))) {
    hs_log(g_module, 0, "checkpoint name exceeds the max length: %d",
           sizeof(fqfn));
    exit(EXIT_FAILURE);
  }

  cpr->values = luaL_newstate();
  if (!cpr->values) {
    hs_log(g_module, 0, "checkpoint_reader luaL_newstate failed");
    exit(EXIT_FAILURE);
  } else {
    lua_pushvalue(cpr->values, LUA_GLOBALSINDEX);
    lua_setglobal(cpr->values, "_G");
  }

  if (hs_file_exists(fqfn)) {
    if (luaL_dofile(cpr->values, fqfn)) {
      hs_log(g_module, 0, "loading %s failed: %s", fqfn,
             lua_tostring(cpr->values, -1));
      exit(EXIT_FAILURE);
    }
  }

  if (pthread_mutex_init(&cpr->lock, NULL)) {
    perror("checkpoint reader pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


void hs_free_checkpoint_reader(hs_checkpoint_reader* cpr)
{
  if (cpr->values) lua_close(cpr->values);
  cpr->values = NULL;
  pthread_mutex_destroy(&cpr->lock);
}


bool hs_load_checkpoint(lua_State* L, int idx, hs_ip_checkpoint* cp)
{
  size_t len;
  switch (lua_type(L, idx)) {
  case LUA_TSTRING:
    pthread_mutex_lock(&cp->lock);
    if (cp->type == HS_CP_NUMERIC) cp->value.s = NULL;
    cp->type = HS_CP_STRING;

    const char* tmp = lua_tolstring(L, idx, &len);
    cp->len = (unsigned)len;
    ++len;
    if (tmp && len <= HS_MAX_IP_CHECKPOINT) {
      if (len > cp->cap) {
        free(cp->value.s);
        cp->value.s = malloc(len);
        if (!cp->value.s) {
          cp->len = 0;
          cp->cap = 0;
          hs_log(g_module, 0, "malloc failed");
          pthread_mutex_unlock(&cp->lock);
          return false;
        }
        cp->cap = len;
      }
      memcpy(cp->value.s, tmp, len);
    } else {
      pthread_mutex_unlock(&cp->lock);
      return false;
    }
    pthread_mutex_unlock(&cp->lock);
    break;
  case LUA_TNUMBER:
    pthread_mutex_lock(&cp->lock);
    if (cp->type == HS_CP_STRING) {
      free(cp->value.s);
      cp->value.s = NULL;
      cp->len = 0;
      cp->cap = 0;
    }
    cp->type = HS_CP_NUMERIC;
    cp->value.d = lua_tonumber(L, idx);
    pthread_mutex_unlock(&cp->lock);
    break;
  case LUA_TNONE:
  case LUA_TNIL:
    break;
  default:
    return false;
  }
  return true;
}


void hs_lookup_checkpoint(hs_checkpoint_reader* cpr,
                          const char* key,
                          hs_ip_checkpoint* cp)
{
  pthread_mutex_lock(&cpr->lock);
  lua_getglobal(cpr->values, key);
  hs_load_checkpoint(cpr->values, -1, cp);
  lua_pop(cpr->values, 1);
  pthread_mutex_unlock(&cpr->lock);
}


void hs_update_checkpoint(hs_checkpoint_reader* cpr,
                          const char* key,
                          const hs_ip_checkpoint* cp)
{
  pthread_mutex_lock(&cpr->lock);
  switch (cp->type) {
  case HS_CP_STRING:
    lua_pushlstring(cpr->values, cp->value.s, cp->len);
    break;
  case HS_CP_NUMERIC:
    lua_pushnumber(cpr->values, cp->value.d);
    break;
  default:
    lua_pushnil(cpr->values);
    break;
  }
  lua_setglobal(cpr->values, key);
  pthread_mutex_unlock(&cpr->lock);
}


void hs_lookup_input_checkpoint(hs_checkpoint_reader* cpr,
                                const char* subdir,
                                const char* key,
                                const char* path,
                                size_t* id,
                                size_t* offset)
{
  const char* pos = NULL;
  pthread_mutex_lock(&cpr->lock);
  lua_pushfstring(cpr->values, "%s->%s", subdir, key);
  lua_gettable(cpr->values, LUA_GLOBALSINDEX);
  if (lua_type(cpr->values, -1) == LUA_TSTRING) {
    const char* tmp = lua_tostring(cpr->values, -1);
    if (tmp) {
      pos = strchr(tmp, ':');
      if (pos) {
        *id = strtoul(tmp, NULL, 10);
        *offset = strtoul(pos + 1, NULL, 10);
      }
    }
  }
  lua_pop(cpr->values, 1);
  pthread_mutex_unlock(&cpr->lock);

  if (!pos) {
    char fqfn[HS_MAX_PATH];
    if (!hs_get_fqfn(path, subdir, fqfn, sizeof(fqfn))) {
      hs_log(g_module, 0, "checkpoint name exceeds the max length: %d",
             sizeof(fqfn));
      exit(EXIT_FAILURE);
    }
    *id = find_first_id(fqfn);
  }
}


void hs_update_input_checkpoint(hs_checkpoint_reader* cpr,
                                const char* subdir,
                                const char* key,
                                size_t id,
                                size_t offset)
{
  pthread_mutex_lock(&cpr->lock);
  lua_pushfstring(cpr->values, "%s->%s", subdir, key);
  lua_pushfstring(cpr->values, "%d:%d", id, offset);
  lua_settable(cpr->values, LUA_GLOBALSINDEX);
  pthread_mutex_unlock(&cpr->lock);
}


void hs_output_checkpoints(hs_checkpoint_reader* cpr, FILE* fh)
{
  const char* key;
  pthread_mutex_lock(&cpr->lock);
  lua_pushnil(cpr->values);
  while (lua_next(cpr->values, LUA_GLOBALSINDEX) != 0) {
    if (lua_type(cpr->values, -2) == LUA_TSTRING) {
      key = lua_tostring(cpr->values, -2);
      switch (lua_type(cpr->values, -1)) {
      case LUA_TSTRING:
        fprintf(fh, "_G['%s'] = '", key);
        hs_output_lua_string(fh, lua_tostring(cpr->values, -1));
        fwrite("'\n", 2, 1, fh);
        break;
      case LUA_TNUMBER:
        fprintf(fh, "_G['%s'] = %.17g\n", key, lua_tonumber(cpr->values, -1));
        break;
      }
    }
    lua_pop(cpr->values, 1);
  }
  pthread_mutex_unlock(&cpr->lock);
}
