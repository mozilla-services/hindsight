/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight util implementation @file */

#include "hs_util.h"

#include <stdio.h>
#include <string.h>

#define MAX_VARINT_BYTES 10

bool hs_file_exists(const char* fn)
{
  FILE* fh = fopen(fn, "r");
  if (fh) {
    fclose(fh);
    return 1;
  }
  return 0;
}


bool hs_get_fqfn(const char* path,
                 const char* name,
                 char* fqfn,
                 size_t fqfn_len)
{
  int ret = snprintf(fqfn, fqfn_len, "%s/%s", path, name);
  if (ret < 0 || ret > (int)fqfn_len - 1) {
    return false;
  }
  return true;
}


void hs_output_lua_string(FILE* fh, const char* s)
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


int hs_write_varint(unsigned char* buf, unsigned long long i)
{
  int pos = 0;
  if (i == 0) {
    buf[pos] = 0;
    return 1;
  }

  while (i) {
    buf[pos++] = (i & 0x7F) | 0x80;
    i >>= 7;
  }
  buf[pos - 1] &= 0x7F; // end the varint
  return pos;
}


unsigned const char*
hs_read_varint(unsigned const char* p, unsigned const char* e, long long* vi)
{
  *vi = 0;
  unsigned i, shift = 0;
  for (i = 0; p != e && i < MAX_VARINT_BYTES; i++) {
    *vi |= ((unsigned long long)p[i] & 0x7f) << shift;
    shift += 7;
    if ((p[i] & 0x80) == 0) break;
  }
  if (i == MAX_VARINT_BYTES) {
    return NULL;
  }
  return p + i + 1;
}


double hs_timespec_delta(const struct timespec* s, const struct timespec* e)
{
  double delta;
  if (e->tv_nsec - s->tv_nsec < 0) {
    delta = e->tv_sec - s->tv_sec - 1 + (e->tv_nsec - s->tv_nsec) / -1e9;
  } else {
    delta = e->tv_sec - s->tv_sec + (e->tv_nsec - s->tv_nsec) / 1e9;
  }
  return delta;
}


bool hs_has_ext(const char* fn, const char* ext)
{
  size_t flen = strlen(fn);
  size_t elen = strlen(ext);
  if (flen <= elen) return false; // a fn with only an extension is invalid
  return strcmp(fn + flen - elen, ext) == 0 ? true : false;
}
