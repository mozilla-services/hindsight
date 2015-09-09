/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight input implementation @file */

#include "hs_input.h"

#include <errno.h>
#include <luasandbox/lauxlib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "hs_heka_message.h"
#include "hs_logger.h"
#include "hs_util.h"

static const char g_module[] = "input_reader";

int hs_open_file(hs_input* hsi, const char* subdir, size_t id)
{
  char path[HS_MAX_PATH];
  int ret = snprintf(path, sizeof(path), "%s/%s/%zu.log", hsi->path, subdir,
                     id);
  if (ret < 0 || ret > (int)sizeof(path) - 1) {
    hs_log(g_module, 0, "%s file: %zu.log: fully qualiifed path is"
           " greater than %zu", hsi->name, hsi->ib.cp.id, sizeof(path));
    exit(EXIT_FAILURE);
  }
  if (hsi->ib.name && strcmp(hsi->ib.name, path) == 0) return 0;

  FILE* fh = fopen(path, "r");
  if (fh) {
    if (setvbuf(fh, NULL, _IONBF, 0)) {
      exit(EXIT_FAILURE);
    }

    if (hsi->ib.cp.id == id && hsi->ib.cp.offset) {
      hs_log(g_module, 7, "%s opened file: %s offset: %zu", hsi->name,
             path,
             hsi->ib.cp.offset);
      if (fseek(fh, hsi->ib.cp.offset, SEEK_SET)) {
        hs_log(g_module, 2, "%s file: %s invalid offset: %zu error: %d",
               hsi->name,
               path,
               hsi->ib.cp.offset,
               ferror(fh));
      }
    } else {
      hs_log(g_module, 7, "%s opened file: %s", hsi->name, path);
    }

    if (hsi->fh) {
      fclose(hsi->fh);
    }
    if (hsi->ib.cp.id != id) {
      hsi->ib.cp.id = id;
      hsi->ib.cp.offset = 0;
    }
    if (ret >= (int)hsi->ib.namesize) {
      free(hsi->ib.name);
      hsi->ib.namesize = (size_t)(ret + 1);
      hsi->ib.name = malloc(hsi->ib.namesize);
      if (!hsi->ib.name) {
        hs_log(g_module, 2, "%s file: %s malloc failed", hsi->name,
               path);
        exit(EXIT_FAILURE);
      }
    }
    strcpy(hsi->ib.name, path);
    hsi->fh = fh;
  }
  return 0;
}


size_t hs_read_file(hs_input* hsi)
{
  if (!hs_expand_input_buffer(&hsi->ib, 0)) {
    hs_log(g_module, 0, "%s buffer reallocation failed", hsi->name);
    exit(EXIT_FAILURE);
  }
  size_t nread = fread(hsi->ib.buf + hsi->ib.readpos,
                       1,
                       hsi->ib.bufsize - hsi->ib.readpos,
                       hsi->fh);
  hsi->ib.cp.offset += nread;
  hsi->ib.readpos += nread;
  return nread;
}


void hs_init_input(hs_input* hsi, size_t max_message_size, const char* path,
                   const char* name)
{
  hsi->fh = NULL;
  if (strlen(path) > HS_MAX_PATH - 30) {
    hs_log(g_module, 0, "path too long");
    exit(EXIT_FAILURE);
  }

  hsi->path = malloc(strlen(path) + 1);
  if (!hsi->path) {
    hs_log(g_module, 0, "path malloc failed");
    exit(EXIT_FAILURE);
  }
  strcpy(hsi->path, path);

  hsi->name = malloc(strlen(name) + 1);
  if (!hsi->name) {
    hs_log(g_module, 0, "name malloc failed");
    exit(EXIT_FAILURE);
  }
  strcpy(hsi->name, name);

  hs_init_input_buffer(&hsi->ib, max_message_size);
}


void hs_free_input(hs_input* hsi)
{
  if (hsi->fh) fclose(hsi->fh);
  hsi->fh = NULL;

  free(hsi->path);
  hsi->path = NULL;

  free(hsi->name);
  hsi->name = NULL;

  hs_free_input_buffer(&hsi->ib);
}


void hs_init_input_buffer(hs_input_buffer* b, size_t max_message_size)
{
  b->name = NULL;
  b->namesize = 0;
  b->bufsize = BUFSIZ;
  if (max_message_size < 1024) {
    b->max_message_size = 1024;
  } else {
    b->max_message_size = max_message_size;
  }
  b->cp.id = 0;
  b->cp.offset = 0;
  b->readpos = 0;
  b->scanpos = 0;
  b->msglen = 0;
  b->buf = malloc(b->bufsize);
  if (!b->buf) {
    hs_log(g_module, 0, "buffer allocation failed");
    exit(EXIT_FAILURE);
  }
}


void hs_free_input_buffer(hs_input_buffer* b)
{
  free(b->buf);
  b->buf = NULL;
  b->bufsize = 0;

  free(b->name);
  b->name = NULL;
  b->namesize = 0;

  b->cp.id = 0;
  b->cp.offset = 0;
  b->readpos = 0;
  b->scanpos = 0;
  b->msglen = 0;
}


/**
 * Hacker's Delight - Henry S. Warren, Jr. page 48
 *
 * @param x
 *
 * @return size_t Least power of 2 greater than or equal to x
 */
static size_t clp2(size_t x)
{
  x = x - 1;
  x = x | (x >> 1);
  x = x | (x >> 2);
  x = x | (x >> 4);
  x = x | (x >> 8);
# if SIZE_MAX > 65536
    x = x | (x >> 16);
# endif
# if SIZE_MAX > 4294967296
    x = x | (x >> 32);
# endif
  return x + 1;
}


bool hs_expand_input_buffer(hs_input_buffer* b, size_t len)
{
  if (b->scanpos != 0) { // shift the message left
    if (b->scanpos == b->readpos) {
      b->scanpos = b->readpos = 0;
    } else {
      memmove(b->buf, b->buf + b->scanpos, b->readpos - b->scanpos);
      b->readpos = b->readpos - b->scanpos;
      b->scanpos = 0;
    }
  }

  if (b->msglen + HS_MAX_HDR_SIZE > b->bufsize
      || b->readpos + len > b->bufsize) {
    size_t newsize;
    if (b->msglen + HS_MAX_HDR_SIZE > b->readpos + len) {
      newsize = b->msglen + HS_MAX_HDR_SIZE;
    } else {
      newsize = b->readpos + len;
    }

    size_t max_buffer = b->max_message_size + HS_MAX_HDR_SIZE;
    if (newsize > max_buffer) return false;

    newsize = clp2(newsize);
    if (newsize > max_buffer) newsize = max_buffer;

    hs_log("input_buffer", 7, "expand buffer\tname:%s\tfrom:%zu\tto:%zu",
           b->name,
           b->bufsize,
           newsize);
    unsigned char* tmp = realloc(b->buf, newsize);
    if (tmp) {
      b->buf = tmp;
      b->bufsize = newsize;
    } else {
      return false;
    }
  }
  return true;
}
