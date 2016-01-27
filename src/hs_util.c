/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight util implementation @file */

#include "hs_util.h"

#include <stdio.h>
#include <string.h>

bool hs_file_exists(const char *fn)
{
  FILE *fh = fopen(fn, "r");
  if (fh) {
    fclose(fh);
    return 1;
  }
  return 0;
}


bool hs_get_fqfn(const char *path,
                 const char *name,
                 char *fqfn,
                 size_t fqfn_len)
{
  int ret = snprintf(fqfn, fqfn_len, "%s/%s", path, name);
  if (ret < 0 || ret > (int)fqfn_len - 1) {
    return false;
  }
  return true;
}


void hs_output_lua_string(FILE *fh, const char *s)
{
  size_t len = strlen(s);
  for (unsigned i = 0; i < len; ++i) {
    switch (s[i]) {
    case '\n':
      fwrite("\\n", 2, 1, fh);
      break;
    case '\r':
      fwrite("\\r", 2, 1, fh);
      break;
    case '"':
      fwrite("\\\"", 2, 1, fh);
      break;
    case '\\':
      fwrite("\\\\", 2, 1, fh);
      break;
    default:
      fwrite(s + i, 1, 1, fh);
      break;
    }
  }
}


bool hs_has_ext(const char *fn, const char *ext)
{
  size_t flen = strlen(fn);
  size_t elen = strlen(ext);
  if (flen <= elen) return false; // a fn with only an extension is invalid
  return strcmp(fn + flen - elen, ext) == 0 ? true : false;
}
