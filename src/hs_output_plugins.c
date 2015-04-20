/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight output plugin loader @file */

#include "hs_output_plugins.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <lauxlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hs_input.h"
#include "hs_logger.h"
#include "hs_message_matcher.h"
#include "hs_output.h"
#include "hs_sandbox.h"
#include "hs_util.h"
#include "lsb.h"
#include "lsb_output.h"

static const char g_module[] = "hs_output_plugins";
static const char g_output[] = "output";
static const char g_analysis[] = "analysis";
static const char g_input[] = "input";

static const char* g_sb_template = "{"
  "memory_limit = %u,"
  "instruction_limit = %u,"
  "output_limit = %u,"
  "path = [[%s]],"
  "cpath = [[%s]],"
  "remove_entries = {"
  "[''] = {'dofile','load','loadfile','loadstring','newproxy'},"
  "os = {'getenv','exit','setlocale'}"
  "}"
  "}";


static int read_message(lua_State* lua)
{
  void* luserdata = lua_touserdata(lua, lua_upvalueindex(1));
  if (NULL == luserdata) {
    luaL_error(lua, "read_message() invalid lightuserdata");
  }
  lua_sandbox* lsb = (lua_sandbox*)luserdata;
  hs_output_plugin* p = (hs_output_plugin*)lsb_get_parent(lsb);

  if (!p->matched || !p->msg) {
    lua_pushnil(lua);
    return 1;
  }
  return hs_read_message(lua, p->msg);
}


static hs_output_plugin* create_output_plugin(const char* file,
                                              const char* cfg_template,
                                              const hs_sandbox_config* cfg,
                                              lua_State* env)
{
  hs_output_plugin* p = calloc(1, sizeof(hs_output_plugin));
  if (!p) return NULL;
  p->list_index = -1;

  if (pthread_mutex_init(&p->cp_lock, NULL)) {
    perror("cp_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }

  hs_init_input(&p->input);
  hs_init_input(&p->analysis);

  p->sb = hs_create_sandbox(p, file, cfg_template, cfg, env);
  if (!p->sb) {
    free(p);
    hs_log(g_module, 3, "lsb_create_custom failed: %s", file);
    return NULL;
  }
  return p;
}


static void free_output_plugin(hs_output_plugin* p)
{
  hs_free_input(&p->analysis);
  hs_free_input(&p->input);
  hs_free_sandbox(p->sb);
  free(p->sb);
  p->sb = NULL;
  p->plugins = NULL;
  pthread_mutex_destroy(&p->cp_lock);
}


static void terminate_sandbox(hs_output_plugin* p)
{
  hs_log(g_module, 3, "file: %s terminated: %s", p->sb->filename,
         lsb_get_error(p->sb->lsb));
  hs_free_sandbox(p->sb);
  free(p->sb);
  p->sb = NULL;
}


static void* input_thread(void* arg)
{
  hs_log(g_module, 6, "starting input thread");

  hs_heka_message im, *pim = NULL;
  hs_init_heka_message(&im, 8);

  hs_heka_message am, *pam = NULL;
  hs_init_heka_message(&am, 8);

  hs_output_plugin* p = (hs_output_plugin*)arg;

  size_t bytes_read[2] = {0};
  while (!p->plugins->stop) {
    if (p->input.fh && !pim) {
      if (hs_find_message(&im, &p->input)) {
        pim = &im;
      } else {
        bytes_read[0] = hs_read_file(&p->input);
      }

      if (!bytes_read[0]) {
        hs_open_file(&p->input, g_input, p->input.id + 1);
      }
    } else if (!p->input.fh) { // still waiting on the first file
      hs_log(g_module, 7, "output waiting for the input file"); // todo remove
      hs_open_file(&p->input, g_input, p->input.id);
    }

    if (p->analysis.fh && !pam) {
      if (hs_find_message(&am, &p->analysis)) {
        pam = &am;
      } else {
        bytes_read[1] = hs_read_file(&p->analysis);
      }

      if (!bytes_read[1]) {
        hs_open_file(&p->analysis, g_analysis, p->analysis.id + 1);
      }
    } else if (!p->analysis.fh) { // still waiting on the first file
      hs_log(g_module, 7, "output waiting for the analysis file"); // todo remove
      hs_open_file(&p->analysis, g_analysis, p->analysis.id);
    }

    // if we have one send the oldest first
    if (pim) {
      if (pam) {
        if (pim->timestamp <= pam->timestamp) {
          p->msg = pim;
          pim = NULL;
        } else {
          p->msg = pam;
          pam = NULL;
        }
      } else {
        p->msg = pim;
        pim = NULL;
      }
    } else if (pam) {
      p->msg = pam;
      pam = NULL;
    }

    if (p->msg) {
      p->current_t = time(NULL);
      if (!hs_output_message(p)) break; // fatal error
      p->msg = NULL;
    } else if (!bytes_read[0] && !bytes_read[1]) {
      sleep(1);
    }
  }

  // signal shutdown
  p->msg = NULL;
  p->current_t = time(NULL);
  hs_output_message(p);

  // sandbox terminated, don't wait for the join to clean up
  // the most recent checkpoint can be lost under these conditions
  // TODO write the checkpoint back to the Lua table and hold it in memory
  if (!p->sb) {
    if (pthread_detach(p->thread)) {
      hs_log(g_module, 3, "thread could not be detached");
    }
    pthread_mutex_lock(&p->plugins->list_lock);
    p->plugins->list[p->list_index] = NULL;
    --p->plugins->list_len;
    pthread_mutex_unlock(&p->plugins->list_lock);
    free_output_plugin(p);
    free(p);
  }

  hs_free_heka_message(&am);
  hs_free_heka_message(&im);

  hs_log(g_module, 6, "exiting input_thread");
  pthread_exit(NULL);
}


static void add_to_output_plugins(hs_output_plugins* plugins,
                                  hs_output_plugin* p)
{
  pthread_mutex_lock(&plugins->list_lock);
  if (plugins->list_len < plugins->list_cap) {
    for (int i = 0; i < plugins->list_cap; ++i) {
      if (!plugins->list[i]) {
        plugins->list[i] = p;
        p->list_index = i;
        break;
      }
    }
  } else {
    // todo probably don't want to grow it by 1
    ++plugins->list_cap;
    hs_output_plugin** tmp = realloc(plugins->list,
                                     sizeof(hs_sandbox*) * plugins->list_cap);
    p->list_index = plugins->list_cap - 1;

    if (tmp) {
      plugins->list = tmp;
      plugins->list[p->list_index] = p;
      ++plugins->list_len;
    } else {
      hs_log(g_module, 0, "plugins realloc failed");
      exit(EXIT_FAILURE);
    }
  }
  assert(p->list_index >= 0);

  hs_config* cfg = p->plugins->cfg;
  if (strlen(cfg->output_path) > sizeof(p->input.path) - 1) {
    fprintf(stderr, "input_thread path too long\n");
    exit(EXIT_FAILURE);
  }
  strcpy(p->input.path, cfg->output_path);
  strcpy(p->analysis.path, cfg->output_path);

  // sync the output and read checkpoints
  // the read and output checkpoints can differ to allow for batching
  hs_lookup_input_checkpoint(&cfg->cp_reader, p->sb->filename, cfg->output_path,
                             g_input, &p->input.id, &p->input.offset);
  p->cp_id[0] = p->input.id;
  p->cp_offset[0] = p->input.offset;

  size_t len = strlen(p->sb->filename) + sizeof(g_analysis) + 2;
  char* tmp = malloc(len);
  if (!tmp) {
    fprintf(stderr, "input_thread tmp malloc failed\n");
    exit(EXIT_FAILURE);
  }
  snprintf(tmp, len, "%s %s", g_analysis, p->sb->filename);
  hs_lookup_input_checkpoint(&cfg->cp_reader, tmp, cfg->output_path,
                             g_analysis, &p->analysis.id, &p->analysis.offset);
  free(tmp);
  p->cp_id[1] = p->analysis.id;
  p->cp_offset[1] = p->analysis.offset;

  int ret = pthread_create(&p->thread, NULL, input_thread, (void*)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


static int init_sandbox(hs_sandbox* sb)
{
  lsb_add_function(sb->lsb, &read_message, "read_message");

  int ret = lsb_init(sb->lsb, sb->state);
  if (ret) {
    hs_log(g_module, 3, "lsb_init() file: %s received: %d %s", sb->filename,
           ret, lsb_get_error(sb->lsb));
    return ret;
  }

  lua_State* lua = lsb_get_lua(sb->lsb);
  // remove output function
  lua_pushnil(lua);
  lua_setglobal(lua, "output");

  return 0;
}


void hs_init_output_plugins(hs_output_plugins* plugins,
                            hs_config* cfg,
                            hs_message_match_builder* mmb)
{
  plugins->cfg = cfg;
  plugins->mmb = mmb;
  plugins->list = NULL;
  plugins->list_len = 0;
  plugins->list_cap = 0;
  plugins->stop = false;
  if (pthread_mutex_init(&plugins->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


void hs_wait_output_plugins(hs_output_plugins* plugins)
{
  void* thread_result;
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (plugins->list[i]) {
      int ret = pthread_join(plugins->list[i]->thread, &thread_result);
      if (ret) {
        perror("pthread_join failed");
      }
    }
  }
}


void hs_free_output_plugins(hs_output_plugins* plugins)
{
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (plugins->list[i]) {
      free_output_plugin(plugins->list[i]);
      free(plugins->list[i]);
      plugins->list[i] = NULL;
    }
  }
  free(plugins->list);

  pthread_mutex_destroy(&plugins->list_lock);
  plugins->list = NULL;
  plugins->cfg = NULL;
  plugins->list_len = 0;
  plugins->list_cap = 0;
}


void hs_load_output_plugins(hs_output_plugins* plugins,
                            const hs_config* cfg,
                            const char* path)
{
  char dir[HS_MAX_PATH];
  if (!hs_get_fqfn(path, g_output, dir, sizeof(dir))) {
    hs_log(HS_APP_NAME, 0, "output load path too long");
    exit(EXIT_FAILURE);
  }

  struct dirent* entry;
  DIR* dp = opendir(dir);
  if (dp == NULL) {
    hs_log(g_module, 0, "%s: %s", dir, strerror(errno));
    exit(EXIT_FAILURE);
  }

  char fqfn[HS_MAX_PATH];
  while ((entry = readdir(dp))) {
    if (!hs_get_config_fqfn(dir, entry->d_name, fqfn, sizeof(fqfn))) continue;
    hs_sandbox_config sbc;
    lua_State* L = hs_load_sandbox_config(fqfn, &sbc, &cfg->opd,
                                          HS_SB_TYPE_OUTPUT);
    if (L) {
      if (!hs_get_fqfn(dir, sbc.filename, fqfn, sizeof(fqfn))) {
        lua_close(L);
        hs_free_sandbox_config(&sbc);
        continue;
      }
      hs_output_plugin* p = create_output_plugin(fqfn, g_sb_template, &sbc, L);
      if (p) {
        p->plugins = plugins;

        size_t len = strlen(entry->d_name) + sizeof(g_output) + 2;
        p->sb->filename = malloc(len);
        snprintf(p->sb->filename, len, "%s/%s", g_output, entry->d_name);

        if (sbc.preserve_data) {
          len = strlen(fqfn);
          p->sb->state = malloc(len + 1);
          memcpy(p->sb->state, fqfn, len + 1);
          memcpy(p->sb->state + len - 3, "dat", 3);
        }

        p->sb->mm = hs_create_message_matcher(plugins->mmb, sbc.message_matcher);
        if (!p->sb->mm || init_sandbox(p->sb)) {
          if (!p->sb->mm) {
            hs_log(g_module, 3, "file: %s invalid message_matcher: %s",
                   p->sb->filename, sbc.message_matcher);
          }
          free_output_plugin(p);
          free(p);
          p = NULL;
          lua_close(L);
          hs_free_sandbox_config(&sbc);
          continue;
        }
        add_to_output_plugins(plugins, p);
      }
      lua_close(L);
    }
    hs_free_sandbox_config(&sbc);
  }
  closedir(dp);
}


bool hs_output_message(hs_output_plugin* p)
{
  if (p->msg) {
    int ret;
    p->matched = false;
    ret = 0;
    if (hs_eval_message_matcher(p->sb->mm, p->msg)) {
      p->matched = true;
      ret = hs_process_message(p->sb->lsb);
      if (ret <= 0) {
        switch (ret) {
        case 0:
          // todo increment process message count
          break;
        case -1:
          // todo increment process message failure count
          break;
          // default ignore
        }
        // advance the checkpoint
        pthread_mutex_lock(&p->cp_lock);
        p->cp_id[0] = p->input.id;
        p->cp_offset[0] = p->input.offset - (p->input.readpos - p->input.scanpos);
        p->cp_id[1] = p->analysis.id;
        p->cp_offset[1] = p->analysis.offset -
          (p->analysis.readpos - p->analysis.scanpos);
        pthread_mutex_unlock(&p->cp_lock);
      }
    } else {
      terminate_sandbox(p);
      return false;
    }

    if (p->sb->ticker_interval && p->current_t >= p->sb->next_timer_event) {
      hs_log(g_module, 7, "running timer_event: %s", p->sb->filename); // todo remove
      ret = hs_timer_event(p->sb->lsb, p->current_t);
      p->sb->next_timer_event += p->sb->ticker_interval;
    }
  } else {
    if (p->sb) {
      if (hs_timer_event(p->sb->lsb, p->current_t)) {
        terminate_sandbox(p);
      }
    }
    return false;
  }
  return true;
}
