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


static int read_checkpoint(hs_config* cfg, hs_input* hsi)
{
  char fqfn[260];

  if (!hs_get_fqfn(cfg->output_path, "hindsight.cp", fqfn, sizeof(fqfn))) {
    exit(EXIT_FAILURE);
  }
  lua_State* L = luaL_newstate();
  if (!L) {
    hs_log(HS_APP_NAME, 0, "checkpoint luaL_newstate failed");
    exit(EXIT_FAILURE);
  } else {
    lua_pushvalue(L, LUA_GLOBALSINDEX);
    lua_setglobal(L, "_G");
  }

  bool found = false;
  if (hs_file_exists(fqfn)) {
    if (luaL_dofile(L, fqfn)) {
      hs_log(HS_APP_NAME, 0, "Loading %s failed: %s", fqfn,
             lua_tostring(L, -1));
      exit(EXIT_FAILURE);
    }
    lua_getglobal(L, cfg->input_path);
    if (lua_type(L, -1) == LUA_TTABLE) {
      lua_getfield(L, -1, "last_output_id");
      if (lua_type(L, -1) == LUA_TNUMBER) {
        hsi->id = (size_t)lua_tointeger(L, -1);
      } else {
        hs_log(HS_APP_NAME, 0, "Invalid checkpoint file: %s table: %s "
               "missing file_id", fqfn, cfg->input_path);
        exit(EXIT_FAILURE);
      }
      lua_pop(L, 1); // remove file_id
      lua_getfield(L, -1, "offset");
      if (lua_type(L, -1) == LUA_TNUMBER) {
        hsi->offset = (size_t)lua_tointeger(L, -1);
      } else {
        hs_log(HS_APP_NAME, 0, "Invalid checkpoint file: %s table: %s "
               "missing offset", fqfn, cfg->input_path);
        exit(EXIT_FAILURE);
      }
      lua_pop(L, 2); // remove offset and table
      found = true;
    }
  }

  if (!found) {
    hsi->id = find_first_id(cfg->input_path);
  }
  lua_close(L);
  return 0;
}


static int open_file(hs_input* hsi, size_t id)
{
  if (hsi->file[0] == 0) {
    int ret = snprintf(hsi->file, sizeof(hsi->file), "%s/%zu.log",
                       hsi->path,
                       id);
    if (ret < 0 || ret > (int)sizeof(hsi->file) - 1) {
      hs_log(HS_APP_NAME, 3, "%zu.log: fully qualiifed path is greater than %zu",
             hsi->id, sizeof(hsi->file));
      exit(EXIT_FAILURE);
    }
  }

  FILE* fh = fopen(hsi->file, "r");
  if (fh) {
    if (fseek(fh, hsi->offset, SEEK_SET)) {
      hs_log(HS_APP_NAME, 0, "Invalid offset: %zu file: %s", hsi->offset,
             hsi->file);
      exit(EXIT_FAILURE);
    }

    if (hsi->fh) {
      fclose(hsi->fh);
    }
    hsi->file[0] = 0;
    hsi->id = id;
    hsi->fh = fh;
    hsi->offset = 0;
  } else {
    sleep(1);
  }
  return 0;
}


void hs_init_input(hs_input* input)
{
  input->path[0] = 0;
  input->file[0] = 0;
  input->id = 0;
  input->offset = 0;
  input->fh = NULL;
}


void hs_free_input(hs_input* input)
{
  if (input->fh) fclose(input->fh);
  input->path[0] = 0;
  input->file[0] = 0;
  input->id = 0;
  input->offset = 0;
}


void* hs_read_input_thread(void* arg)
{
  hs_analysis_plugins* plugins = (hs_analysis_plugins*)arg;
  hs_config* cfg = plugins->cfg;

  hs_log(HS_APP_NAME, 6, "starting hs_read_input");

  if (strlen(cfg->input_path) > sizeof(plugins->input.path) - 1) {
    fprintf(stderr, "hs_read_input path too long\n");
    exit(EXIT_FAILURE);
  }
  strcpy(plugins->input.path, cfg->input_path);

  read_checkpoint(cfg, &plugins->input);
  bool reached_eof = false;
  plugins->msg = "todo remove";
  while (!plugins->stop) {
    if (plugins->input.fh) {
      hs_log(HS_APP_NAME, 6, "read here"); // todo remove
      // read
      // unframe/decode
      for (int i = 0; i < plugins->thread_cnt; ++i) {
        sem_post(&plugins->list[i].start);
      }
      // this creates a bottleneck of ~110-120K messages per second (tp x230)
      for (int i = 0; i < plugins->thread_cnt; ++i) {
        sem_wait(&plugins->list[i].finished);
      }
      sleep(1); // todo remove
    } else { // still waiting on the first file
      hs_log(HS_APP_NAME, 6, "waiting for the input file"); // todo remove
      open_file(&plugins->input, plugins->input.id);
    }

    if (reached_eof) {
      // see if the next file is there yet
      hs_log(HS_APP_NAME, 6, "looking for the next log"); // todo remove
      open_file(&plugins->input, plugins->input.id + 1);
    }
  }
  // signal shutdown
  plugins->msg = NULL;
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    sem_post(&plugins->list[i].start);
  }
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    sem_wait(&plugins->list[i].finished);
  }
  hs_log(HS_APP_NAME, 6, "exiting hs_read_input");
  pthread_exit(NULL);
}
