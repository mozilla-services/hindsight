/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight input implementation @file */

#include "hs_input.h"

#include <errno.h>
#include <luasandbox/util/heka_message.h>
#include <luasandbox/lauxlib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "hs_logger.h"
#include "hs_util.h"

static const char g_module[] = "input_reader";

bool hs_open_file(hs_input *hsi, const char *subdir, unsigned long long id)
{
  char fqfn[HS_MAX_PATH];
  int ret = snprintf(fqfn, sizeof(fqfn), "%s/%s/%llu.log", hsi->path, subdir,
                     id);
  if (ret < 0 || ret > (int)sizeof(fqfn) - 1) {
    hs_log(NULL, g_module, 0, "%s file: %llu.log: fully qualiifed path is"
           " greater than %zu", hsi->name, hsi->cp.id, sizeof(fqfn));
    exit(EXIT_FAILURE);
  }
  if (hsi->fn && strcmp(hsi->fn, fqfn) == 0) return true;

  FILE *fh = fopen(fqfn, "re");
  if (fh) {
    if (setvbuf(fh, NULL, _IONBF, 0)) {
      exit(EXIT_FAILURE);
    }

    if (hsi->cp.id == id && hsi->cp.offset) {
      hs_log(NULL, g_module, 7, "%s opened file: %s offset: %zu", hsi->name,
             fqfn,
             hsi->cp.offset);
      if (fseek(fh, hsi->cp.offset, SEEK_SET)) {
        hs_log(NULL, g_module, 2, "%s file: %s invalid offset: %zu error: %d",
               hsi->name,
               fqfn,
               hsi->cp.offset,
               ferror(fh));
      }
    } else {
      hs_log(NULL, g_module, 7, "%s opened file: %s", hsi->name, fqfn);
    }

    if (hsi->fh) {
      fclose(hsi->fh);
    }
    if (hsi->cp.id != id) {
      hsi->cp.id = id;
      hsi->cp.offset = 0;
    }
    if (ret >= (int)hsi->fn_size) {
      free(hsi->fn);
      hsi->fn_size = (size_t)(ret + 1);
      hsi->fn = malloc(hsi->fn_size);
      if (!hsi->fn) {
        hs_log(NULL, g_module, 2, "%s file: %s malloc failed", hsi->name,
               fqfn);
        exit(EXIT_FAILURE);
      }
    }
    strcpy(hsi->fn, fqfn);
    hsi->fh = fh;
    return true;
  }
  return false;
}


size_t hs_read_file(hs_input *hsi)
{
  lsb_input_buffer *ib = &hsi->ib;
  size_t need;
  if (ib->msglen) {
    need = ib->msglen + (size_t)ib->buf[ib->scanpos + 1] + LSB_HDR_FRAME_SIZE
        - (ib->readpos - ib->scanpos);
  } else {
    need = ib->scanpos + ib->size - ib->readpos;
  }

  size_t cnt = ib->size - ib->readpos;
  if (lsb_expand_input_buffer(ib, need)) {
    hs_log(NULL, g_module, 0, "%s buffer reallocation failed", hsi->name);
    exit(EXIT_FAILURE);
  }
  size_t nread = fread(ib->buf + ib->readpos,
                       1,
                       cnt,
                       hsi->fh);
  hsi->cp.offset += nread;
  ib->readpos += nread;
  if (cnt != nread) {
    clearerr(hsi->fh);
  }
  return nread;
}


void hs_init_input(hs_input *hsi, size_t max_message_size, const char *path,
                   const char *name)
{
  hsi->fh = NULL;
  hsi->fn = NULL;
  hsi->fn_size = 0;
  hsi->cp.id = 0;
  hsi->cp.offset = 0;
  if (strlen(path) > HS_MAX_PATH - 30) {
    hs_log(NULL, g_module, 0, "path too long");
    exit(EXIT_FAILURE);
  }

  hsi->path = malloc(strlen(path) + 1);
  if (!hsi->path) {
    hs_log(NULL, g_module, 0, "path malloc failed");
    exit(EXIT_FAILURE);
  }
  strcpy(hsi->path, path);

  hsi->name = malloc(strlen(name) + 1);
  if (!hsi->name) {
    hs_log(NULL, g_module, 0, "name malloc failed");
    exit(EXIT_FAILURE);
  }
  strcpy(hsi->name, name);

  lsb_init_input_buffer(&hsi->ib, max_message_size);
}


void hs_free_input(hs_input *hsi)
{
  if (hsi->fh) fclose(hsi->fh);
  hsi->fh = NULL;

  free(hsi->path);
  hsi->path = NULL;

  free(hsi->name);
  hsi->name = NULL;

  free(hsi->fn);
  hsi->fn = NULL;

  lsb_free_input_buffer(&hsi->ib);
}
