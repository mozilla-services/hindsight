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


int hs_open_file(hs_input* hsi, const char* subdir, size_t id)
{
  char path[HS_MAX_PATH];
  int ret = snprintf(path, sizeof(path), "%s/%s/%zu.log", hsi->path, subdir,
                     id);
  if (ret < 0 || ret > (int)sizeof(path) - 1) {
    hs_log(HS_APP_NAME, 0, "%zu.log: fully qualiifed path is greater than %zu",
           hsi->id, sizeof(path));
    exit(EXIT_FAILURE);
  }
  if (strcmp(hsi->file, path) == 0) return 0;

  FILE* fh = fopen(path, "r");
  if (fh) {
    if (setvbuf(fh, NULL, _IONBF, 0)) {
      exit(EXIT_FAILURE);
    }

    if (hsi->id == id && hsi->offset) {
      hs_log(HS_APP_NAME, 6, "opened input file: %s offset: %zu", path,
             hsi->offset);
      if (fseek(fh, hsi->offset, SEEK_SET)) {
        hs_log(HS_APP_NAME, 2, "invalid offset: %zu file: %s error: %d",
               hsi->offset,
               hsi->file,
               ferror(fh));
      }
    } else {
      hs_log(HS_APP_NAME, 7, "opened input file: %s", path);
    }

    if (hsi->fh) {
      fclose(hsi->fh);
    }
    pthread_mutex_lock(&hsi->lock);
    if (hsi->id != id) {
      hsi->id = id;
      hsi->offset = 0;
    }
    strcpy(hsi->file, path);
    hsi->fh = fh;
    pthread_mutex_unlock(&hsi->lock);
  }
  return 0;
}


size_t hs_read_file(hs_input* hsi)
{
  if (hsi->scanpos != 0) { // shift the message left
    if (hsi->scanpos == hsi->readpos) {
      pthread_mutex_lock(&hsi->lock);
      hsi->scanpos = hsi->readpos = 0;
      pthread_mutex_unlock(&hsi->lock);
    } else {
      memmove(hsi->buf, hsi->buf + hsi->scanpos, hsi->readpos - hsi->scanpos);
      pthread_mutex_lock(&hsi->lock);
      hsi->readpos = hsi->readpos - hsi->scanpos;
      hsi->scanpos = 0;
      pthread_mutex_unlock(&hsi->lock);
    }
  }

  if (hsi->msglen + HS_MAX_HDR_LEN > hsi->bufsize) { // expand the buffer
    size_t newsize = hsi->bufsize * 2;
    if (newsize > HS_MAX_MSG_LEN) {
      newsize = HS_MAX_MSG_LEN;
    }
    hs_log(HS_APP_NAME, 7, "expand buf from: %zu to: %zu", hsi->bufsize,
           newsize);
    unsigned char* tmp = realloc(hsi->buf, newsize);
    if (tmp) {
      hsi->buf = tmp;
      hsi->bufsize = newsize;
    } else {
      hs_log(HS_APP_NAME, 0, "input buffer reallocation failed");
      exit(EXIT_FAILURE);
    }
  }

  size_t nread = fread(hsi->buf + hsi->readpos, 1, hsi->bufsize - hsi->readpos,
                       hsi->fh);
  hsi->offset += nread;
  hsi->readpos += nread;
  return nread;
}


void hs_init_input(hs_input* input)
{
  input->fh = NULL;
  input->path[0] = 0;
  input->file[0] = 0;
  input->bufsize = BUFSIZ;
  input->id = 0;
  input->offset = 0;
  input->readpos = 0;
  input->scanpos = 0;
  input->msglen = 0;
  input->buf = malloc(input->bufsize);
  if (!input->buf) {
    hs_log(HS_APP_NAME, 0, "input buffer allocation failed");
    exit(EXIT_FAILURE);
  }
  if (pthread_mutex_init(&input->lock, NULL)) {
    perror("input lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


void hs_free_input(hs_input* input)
{
  if (input->fh) fclose(input->fh);
  input->path[0] = 0;
  input->file[0] = 0;
  free(input->buf);
  input->buf = NULL;
  input->bufsize = 0;
  input->id = 0;
  input->offset = 0;
  input->readpos = 0;
  input->scanpos = 0;
  input->msglen = 0;
  pthread_mutex_destroy(&input->lock);
}
