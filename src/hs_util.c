/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight util implementation @file */

#include "hs_util.h"

#include <stdio.h>
#include <string.h>
#include <sys/vfs.h>

bool hs_file_exists(const char *fn)
{
  FILE *fh = fopen(fn, "re");
  if (fh) {
    fclose(fh);
    return 1;
  }
  return 0;
}


int hs_get_fqfn(const char *path,
                const char *name,
                char *fqfn,
                size_t fqfn_len)
{
  int rv = snprintf(fqfn, fqfn_len, "%s/%s", path, name);
  return (rv < 0 || rv > (int)fqfn_len - 1);
}


int hs_output_lua_string(FILE *fh, const char *s)
{
  int rv = 1;
  size_t len = strlen(s);
  for (unsigned i = 0; i < len && rv == 1; ++i) {
    switch (s[i]) {
    case '\n':
      rv = fwrite("\\n", 2, 1, fh);
      break;
    case '\r':
      rv = fwrite("\\r", 2, 1, fh);
      break;
    case '"':
      rv = fwrite("\\\"", 2, 1, fh);
      break;
    case '\\':
      fwrite("\\\\", 2, 1, fh);
      break;
    default:
      rv = fwrite(s + i, 1, 1, fh);
      break;
    }
  }
  return rv == 1 ? 0 : 1;
}


bool hs_has_ext(const char *fn, const char *ext)
{
  size_t flen = strlen(fn);
  size_t elen = strlen(ext);
  if (flen <= elen) return false; // a fn with only an extension is invalid
  return strcmp(fn + flen - elen, ext) == 0 ? true : false;
}


unsigned hs_disk_free_ob(const char *path, unsigned ob_size)
{
  struct statfs buf;
  if (ob_size == 0 || statfs(path, &buf)) return 0;
  return buf.f_bsize * buf.f_bavail / ob_size;
}
