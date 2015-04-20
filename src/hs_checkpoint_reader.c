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
#include <lauxlib.h>
#include <lualib.h>
#include <stdlib.h>
#include <string.h>

#include "hs_logger.h"
#include "hs_util.h"


static const char g_module[] = "hs_checkpoint_writer";


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


size_t find_first_id(const char* path)
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


void hs_init_checkpoint_reader(hs_checkpoint_reader* cp, const char* path)
{
  char fqfn[HS_MAX_PATH];
  if (!hs_get_fqfn(path, "hindsight.cp", fqfn, sizeof(fqfn))) {
    hs_log(g_module, 0, "checkpoint name exceeds the max length: %d",
           sizeof(fqfn));
    exit(EXIT_FAILURE);
  }

  cp->values = luaL_newstate();
  if (!cp->values) {
    hs_log(g_module, 0, "checkpoint_reader luaL_newstate failed");
    exit(EXIT_FAILURE);
  } else {
    lua_pushvalue(cp->values, LUA_GLOBALSINDEX);
    lua_setglobal(cp->values, "_G");
  }

  if (hs_file_exists(fqfn)) {
    if (luaL_dofile(cp->values, fqfn)) {
      hs_log(g_module, 0, "loading %s failed: %s", fqfn,
             lua_tostring(cp->values, -1));
      exit(EXIT_FAILURE);
    }
  }
}


void hs_free_checkpoint_reader(hs_checkpoint_reader* cp)
{
  if (cp->values) lua_close(cp->values);
  cp->values = NULL;
}


void hs_lookup_checkpoint(hs_checkpoint_reader* cp,
                          const char* key, char** s,
                          unsigned* scap)
{
  lua_getglobal(cp->values, key);
  if (lua_type(cp->values, -1) == LUA_TSTRING) {
    size_t len;
    const char* tmp = lua_tolstring(cp->values, -1, &len);
    ++len;
    if (tmp && len <= HS_MAX_PATH) {
      if (len > *scap) {
        free(*s);
        *s = malloc(len);
        *scap = len;
        if (!*s) {
          hs_log(g_module, 0, "checkpoint_reader malloc failed");
          exit(EXIT_FAILURE);
        }
      }
      memcpy(*s, tmp, len);
    }
  } // else leave it unchanged
  lua_pop(cp->values, 1);
}


void hs_lookup_input_checkpoint(hs_checkpoint_reader* cp,
                                const char* key,
                                const char* path,
                                const char* subdir,
                                size_t* id,
                                size_t* offset)
{
  char fqfn[HS_MAX_PATH];
  if (!hs_get_fqfn(path, subdir, fqfn, sizeof(fqfn))) {
    hs_log(g_module, 0, "checkpoint name exceeds the max length: %d",
           sizeof(fqfn));
    exit(EXIT_FAILURE);
  }

  char *cp_string = NULL;
  unsigned cp_cap = 0;
  hs_lookup_checkpoint(cp, key, &cp_string, &cp_cap);
  if (cp_string) {
    char* pos = strchr(cp_string, ':');
    if (pos) {
      *pos = 0;
      *id = strtoul(cp_string, NULL, 10);
      *offset = strtoul(pos+1, NULL, 10);
    } else {
      *id = find_first_id(fqfn);
    }
  } else {
    *id = find_first_id(fqfn);
  }
  free(cp_string);
}
