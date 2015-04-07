/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight output implementation @file */

#include "hs_output.h"

#include <errno.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>

#include "hs_logger.h"
#include "hs_util.h"

static void init_checkpoint(hs_checkpoint* cp, const char* path)
{
  char fqfn[260];
  if (!hs_get_fqfn(path, "hindsight.cp", fqfn, sizeof(fqfn))) {
    exit(EXIT_FAILURE);
  }
  cp->offset = 0;
  cp->id = 0;

  cp->values = luaL_newstate();
  if (!cp->values) {
    hs_log(HS_APP_NAME, 0, "checkpoint luaL_newstate failed");
    exit(EXIT_FAILURE);
  } else {
    lua_pushvalue(cp->values, LUA_GLOBALSINDEX);
    lua_setglobal(cp->values, "_G");
  }

  if (hs_file_exists(fqfn)) {
    if (luaL_dofile(cp->values, fqfn)) {
      hs_log(HS_APP_NAME, 0, "loading %s failed: %s", fqfn,
             lua_tostring(cp->values, -1));
      exit(EXIT_FAILURE);
    }
  }

  lua_getglobal(cp->values, "last_output_id");
  if (lua_type(cp->values, -1) == LUA_TNUMBER) {
    cp->id = (size_t)lua_tonumber(cp->values, 1);
  }
  lua_pop(cp->values, 1);

  cp->fh = fopen(fqfn, "wb");
  if (!cp->fh) {
    hs_log(HS_APP_NAME, 0, "%s: %s", fqfn, strerror(errno));
    exit(EXIT_FAILURE);
  }
}


static void free_checkpoint(hs_checkpoint* cp)
{
  if (cp->fh) fclose(cp->fh);
  cp->fh = NULL;

  if (cp->values) lua_close(cp->values);
  cp->values = NULL;

  cp->offset = 0;
  cp->id = 0;
}


void hs_init_output(hs_output* output, const char* path)
{
  output->fh = NULL;
  init_checkpoint(&output->cp, path);
  hs_open_output_file(output, path);
}


void hs_free_output(hs_output* output)
{
  if (output->fh) fclose(output->fh);
  output->fh = NULL;

  free_checkpoint(&output->cp);
}


void hs_open_output_file(hs_output* output, const char* path)
{
  static char fqfn[260];
  if (output->fh) {
    fclose(output->fh);
    output->fh = NULL;
  }
  int ret = snprintf(fqfn, sizeof(fqfn), "%s/%zu.log", path, output->cp.id);
  if (ret < 0 || ret > (int)sizeof(fqfn) - 1) {
    hs_log(HS_APP_NAME, 0, "output filename exceeds %zu", sizeof(fqfn));
    exit(EXIT_FAILURE);
  }
  output->fh = fopen(fqfn, "ab+");
  if (!output->fh) {
    hs_log(HS_APP_NAME, 0, "%s: %s", fqfn, strerror(errno));
    exit(EXIT_FAILURE);
  } else {
    fseek(output->fh, 0, SEEK_END);
    output->cp.offset = ftell(output->fh);
  }
}
