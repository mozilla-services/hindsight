/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight input implementation @file */

#include "hs_input.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <lauxlib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hs_analysis_plugins.h"
#include "hs_heka_message.h"
#include "hs_logger.h"
#include "hs_util.h"


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
    hs_log(HS_APP_NAME, 0, "%s: %s", path, strerror(errno));
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


static int read_checkpoint(hs_config* cfg, hs_analysis_plugins* plugins)
{
  bool found = false;
  lua_State* L = plugins->output.cp.values;
  hs_input* hsi = &plugins->input;

  lua_getglobal(L, "input_id");
  if (lua_type(L, -1) == LUA_TNUMBER) {
    hsi->id = (size_t)lua_tointeger(L, -1);
    found = true;
  }
  lua_pop(L, 1); // remove input_id

  lua_getglobal(L, "input_offset");
  if (found && lua_type(L, -1) == LUA_TNUMBER) {
    hsi->offset = (size_t)lua_tointeger(L, -1);
  }
  lua_pop(L, 1); // remove input_offset

  if (!found) {
    hsi->id = find_first_id(cfg->input_path);
  }
  return 0;
}


static int open_file(hs_input* hsi, size_t id)
{
  char path[260];
  int ret = snprintf(path, sizeof(path), "%s/%zu.log", hsi->path, id);
  if (ret < 0 || ret > (int)sizeof(path) - 1) {
    hs_log(HS_APP_NAME, 0, "%zu.log: fully qualiifed path is greater than %zu",
           hsi->id, sizeof(path));
    exit(EXIT_FAILURE);
  }
  if (strcmp(hsi->file, path) == 0) return 0;

  FILE* fh = fopen(path, "r");
  if (fh) {
    if (setvbuf(fh, NULL, _IONBF, 0)) {
      hs_log(HS_APP_NAME, 0, "%s: %s", path, strerror(errno));
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
    strcpy(hsi->file, path);
    hsi->id = id;
    hsi->fh = fh;
    hsi->offset = 0;
  } else {
    sleep(1);
  }
  return 0;
}


static size_t decode_header(unsigned char* buf, size_t len)
{
  if (*buf != 0x08) return 0;

  unsigned char* p = buf;
  if (p && p < buf + len - 1) {
    long long vi;
    if (hs_read_varint(p + 1, buf + len, &vi)) {
      if (vi > HS_MIN_MSG_LEN && vi <= HS_MAX_MSG_LEN) {
        return vi;
      }
    }
  }
  return 0;
}


static bool find_message(hs_heka_message* m, hs_input* hsi)
{
  if (hsi->readpos == hsi->scanpos) return false;

  unsigned char* p = memchr(&hsi->buf[hsi->scanpos], 0x1e,
                            hsi->readpos - hsi->scanpos);
  if (p) {
    if (p != hsi->buf + hsi->scanpos) {
      hs_log(HS_APP_NAME, 4, "discarded bytes: %zu offset %zu",
             p - hsi->buf + hsi->scanpos,
             ftell(hsi->fh) - (hsi->readpos - (hsi->scanpos + 2)));
    }
    hsi->scanpos = p - hsi->buf;

    if (hsi->readpos - hsi->scanpos < 2) {
      return false; // header length is not buf
    }

    size_t hlen = hsi->buf[hsi->scanpos + 1];
    size_t hend = hsi->scanpos + hlen + 3;
    if (hend > hsi->readpos) return false; // header is not in buf
    if (hsi->buf[hend - 1] != 0x1f) return false; // invalid header termination

    if (!hsi->msglen) {
      hsi->msglen = decode_header(&hsi->buf[hsi->scanpos + 2], hlen);
    }

    if (hsi->msglen) {
      size_t mend = hend + hsi->msglen;
      if (mend > hsi->readpos) return false; // message is not in buf

      if (hs_decode_heka_message(m, &hsi->buf[hend], hsi->msglen)) {
        hsi->scanpos = mend;
        hsi->msglen = 0;
      } else {
        hs_log(HS_APP_NAME, 4, "message decode failure file: %s offset %zu",
               hsi->file,
               ftell(hsi->fh) - (hsi->readpos - hend));
        ++hsi->scanpos;
      }
    } else {
      hs_log(HS_APP_NAME, 4,
             "message header decode failure file: %s offset %lld",
             hsi->file,
             ftell(hsi->fh) - (hsi->readpos - (hsi->scanpos + 2)));
      ++hsi->scanpos;
    }
  } else {
    hs_log(HS_APP_NAME, 4, "discarded bytes: %zu offset %zu",
           hsi->readpos - hsi->scanpos,
           ftell(hsi->fh) - (hsi->readpos - (hsi->scanpos + 2)));
    hsi->scanpos = hsi->readpos = 0;
  }
  return true;
}


static size_t read_file(hs_input* hsi)
{
  if (hsi->scanpos != 0) { // shift the message left
    if (hsi->scanpos == hsi->readpos) {
      hsi->scanpos = hsi->readpos = 0;
    } else {
      memmove(hsi->buf, hsi->buf + hsi->scanpos, hsi->readpos - hsi->scanpos);
      hsi->readpos = hsi->readpos - hsi->scanpos;
      hsi->scanpos = 0;
    }
  }

  if (hsi->msglen + HS_MAX_HDR_LEN > hsi->bufsize) { // expand the buffer
    size_t newsize = hsi->bufsize * 2;
    if (newsize > HS_MAX_MSG_LEN) {
      newsize = HS_MAX_MSG_LEN;
    }
    hs_log(HS_APP_NAME, 7, "expand buf from: %zu to: %zu", hsi->bufsize, newsize);
    unsigned char* tmp = realloc(hsi->buf, newsize);
    if (tmp) {
      hsi->buf = tmp;
      hsi->bufsize = newsize;
    } else {
      hs_log(HS_APP_NAME, 0, "input buffer reallocation failed");
      exit(EXIT_FAILURE);
    }
  }

  size_t nread = fread(hsi->buf + hsi->readpos, 1, hsi->bufsize - hsi->readpos, hsi->fh);
  hsi->readpos += nread;
  return nread;
}


void hs_init_input(hs_input* input)
{
  input->path[0] = 0;
  input->file[0] = 0;
  input->bufsize = BUFSIZ;
  input->buf = malloc(input->bufsize);
  if (!input->buf) {
    hs_log(HS_APP_NAME, 0, "input buffer allocation failed");
    exit(EXIT_FAILURE);
  }
  input->id = 0;
  input->offset = 0;
  input->readpos = 0;
  input->scanpos = 0;
  input->msglen = 0;
  input->fh = NULL;
}


void hs_free_input(hs_input* input)
{
  if (input->fh) fclose(input->fh);
  input->path[0] = 0;
  input->file[0] = 0;
  free(input->buf);
  input->bufsize = 0;
  input->id = 0;
  input->offset = 0;
  input->readpos = 0;
  input->scanpos = 0;
  input->msglen = 0;
}


void* hs_read_input_thread(void* arg)
{
  hs_heka_message msg;
  hs_init_heka_message(&msg, 8);

  hs_analysis_plugins* plugins = (hs_analysis_plugins*)arg;
  hs_config* cfg = plugins->cfg;

  hs_log(HS_APP_NAME, 6, "starting hs_read_input");

  if (strlen(cfg->input_path) > sizeof(plugins->input.path) - 1) {
    fprintf(stderr, "hs_read_input path too long\n");
    exit(EXIT_FAILURE);
  }
  strcpy(plugins->input.path, cfg->input_path);

  read_checkpoint(cfg, plugins);
  size_t bytes_read = 0;
  plugins->msg = &msg;
  size_t cnt = 0;
  while (!plugins->stop) {
    if (plugins->input.fh) {
      if (find_message(&msg, &plugins->input)) {
        ++cnt;
//        for (int i = 0; i < plugins->thread_cnt; ++i) {
//          sem_post(&plugins->list[i].start);
//        }
//        // this creates a bottleneck of ~110-120K messages per second (tp x230)
//        for (int i = 0; i < plugins->thread_cnt; ++i) {
//          sem_wait(&plugins->finished);
//        }
      } else {
        bytes_read = read_file(&plugins->input);
      }

      if (!bytes_read) {
        // see if the next file is there yet
        hs_log(HS_APP_NAME, 7, "count %zu", cnt); // todo remove
        hs_log(HS_APP_NAME, 7, "looking for the next log"); // todo remove
        open_file(&plugins->input, plugins->input.id + 1);
      }
//      sleep(1); // todo remove
    } else { // still waiting on the first file
      hs_log(HS_APP_NAME, 7, "waiting for the input file"); // todo remove
      open_file(&plugins->input, plugins->input.id);
    }

  }
  // signal shutdown
  plugins->msg = NULL;
  hs_free_heka_message(&msg);
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    sem_post(&plugins->list[i].start);
  }
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    sem_wait(&plugins->finished);
  }
  hs_log(HS_APP_NAME, 6, "exiting hs_read_input");
  pthread_exit(NULL);
}
