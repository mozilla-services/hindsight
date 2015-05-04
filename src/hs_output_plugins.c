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
#include <luasandbox.h>
#include <luasandbox/lauxlib.h>
#include <luasandbox_output.h>
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

static const char g_module[] = "output_plugins";
static const char g_output[] = "output";

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
                                              const hs_config* cfg,
                                              const hs_sandbox_config* sbc,
                                              lua_State* env)
{
  hs_output_plugin* p = calloc(1, sizeof(hs_output_plugin));
  if (!p) return NULL;
  p->list_index = -1;

  char lsb_config[1024 * 2];
  int ret = snprintf(lsb_config, sizeof(lsb_config), g_sb_template,
                     sbc->memory_limit,
                     sbc->instruction_limit,
                     sbc->output_limit,
                     cfg->io_lua_path,
                     cfg->io_lua_cpath);

  if (ret < 0 || ret > (int)sizeof(lsb_config) - 1) {
    return NULL;
  }

  if (pthread_mutex_init(&p->cp_lock, NULL)) {
    perror("cp_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }

  hs_init_input(&p->input);
  hs_init_input(&p->analysis);

  p->sb = hs_create_sandbox(p, file, lsb_config, sbc, env);
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


static bool output_message(hs_output_plugin* p)
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
        p->cp_id[0] = p->cur_id[0];
        p->cp_offset[0] = p->cur_offset[0];
        p->cp_id[1] = p->cur_id[1];
        p->cp_offset[1] = p->cur_offset[1];
        pthread_mutex_unlock(&p->cp_lock);
      }
    } else {
      hs_log(g_module, 3, "terminated: %s msg: %s", p->sb->filename,
             lsb_get_error(p->sb->lsb));
      return false;
    }

    if (p->sb->ticker_interval && p->current_t >= p->sb->next_timer_event) {
      ret = hs_timer_event(p->sb->lsb, p->current_t);
      p->sb->next_timer_event += p->sb->ticker_interval;
    }
  } else {
    if (p->sb) {
      if (hs_timer_event(p->sb->lsb, p->current_t)) {
        hs_log(g_module, 3, "terminated: %s msg: %s", p->sb->filename,
               lsb_get_error(p->sb->lsb));
      }
    }
    return false;
  }
  return true;
}


static void* input_thread(void* arg)
{
  hs_heka_message im, *pim = NULL;
  hs_init_heka_message(&im, 8);

  hs_heka_message am, *pam = NULL;
  hs_init_heka_message(&am, 8);

  hs_output_plugin* p = (hs_output_plugin*)arg;
  hs_log(g_module, 6, "starting: %s", p->sb->filename);

  size_t bytes_read[2] = { 0 };
  while (!p->plugins->stop) {
    if (p->input.fh && !pim) {
      if (hs_find_message(&im, &p->input)) {
        pim = &im;
      } else {
        bytes_read[0] = hs_read_file(&p->input);
      }

      if (!bytes_read[0]) {
        hs_open_file(&p->input, hs_input_dir, p->input.id + 1);
      }
    } else if (!p->input.fh) { // still waiting on the first file
      hs_open_file(&p->input, hs_input_dir, p->input.id);
    }

    if (p->analysis.fh && !pam) {
      if (hs_find_message(&am, &p->analysis)) {
        pam = &am;
      } else {
        bytes_read[1] = hs_read_file(&p->analysis);
      }

      if (!bytes_read[1]) {
        hs_open_file(&p->analysis, hs_analysis_dir, p->analysis.id + 1);
      }
    } else if (!p->analysis.fh) { // still waiting on the first file
      hs_open_file(&p->analysis, hs_analysis_dir, p->analysis.id);
    }

    // if we have one send the oldest first
    if (pim) {
      if (pam) {
        if (pim->timestamp <= pam->timestamp) {
          p->msg = pim;
        } else {
          p->msg = pam;
        }
      } else {
        p->msg = pim;
      }
    } else if (pam) {
      p->msg = pam;
    }

    if (p->msg) {
      if (p->msg == pim) {
        pim = NULL;
        p->cur_id[0] = p->input.id;
        p->cur_offset[0] = p->input.offset -
          (p->input.readpos - p->input.scanpos);
      } else {
        pam = NULL;
        p->cur_id[1] = p->analysis.id;
        p->cur_offset[1] = p->analysis.offset -
          (p->analysis.readpos - p->analysis.scanpos);
      }
      p->current_t = time(NULL);
      if (!output_message(p)) break; // fatal error
      p->msg = NULL;
    } else if (!bytes_read[0] && !bytes_read[1]) {
      sleep(1);
    }
  }

  // signal shutdown
  p->msg = NULL;
  p->current_t = time(NULL);
  output_message(p);
  hs_free_heka_message(&am);
  hs_free_heka_message(&im);

  hs_log(g_module, 6, "detaching: %s", p->sb->filename);

  // hold the current checkpoints in memory incase we restart it
  hs_update_input_checkpoint(&p->plugins->cfg->cp_reader,
                             p->sb->filename,
                             hs_input_dir,
                             p->cp_id[0],
                             p->cp_offset[0]);

  hs_update_input_checkpoint(&p->plugins->cfg->cp_reader,
                             p->sb->filename,
                             hs_analysis_dir,
                             p->cp_id[1],
                             p->cp_offset[1]);

  pthread_mutex_lock(&p->plugins->list_lock);
  hs_output_plugins* plugins = p->plugins;
  plugins->list[p->list_index] = NULL;
  if (pthread_detach(p->thread)) {
    hs_log(g_module, 3, "thread could not be detached");
  }
  free_output_plugin(p);
  free(p);
  --plugins->list_cnt;
  pthread_mutex_unlock(&plugins->list_lock);
  pthread_exit(NULL);
}


static void add_to_output_plugins(hs_output_plugins* plugins,
                                  hs_output_plugin* p)
{
  pthread_mutex_lock(&plugins->list_lock);
  if (plugins->list_cnt < plugins->list_cap) {
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
      ++plugins->list_cnt;
    } else {
      hs_log(g_module, 0, "plugins realloc failed");
      exit(EXIT_FAILURE);
    }
  }
  pthread_mutex_unlock(&plugins->list_lock);
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
                             hs_input_dir, &p->input.id, &p->input.offset);
  p->cur_id[0] = p->cp_id[0] = p->input.id;
  p->cur_offset[0] = p->cp_offset[0] = p->input.offset;

  hs_lookup_input_checkpoint(&cfg->cp_reader, p->sb->filename, cfg->output_path,
                             hs_analysis_dir, &p->analysis.id, &p->analysis.offset);
  p->cur_id[1] = p->cp_id[1] = p->analysis.id;
  p->cur_offset[1] = p->cp_offset[1] = p->analysis.offset;

  int ret = pthread_create(&p->thread, NULL, input_thread, (void*)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
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
  plugins->list_cnt = 0;
  plugins->list_cap = 0;
  plugins->stop = false;
  if (pthread_mutex_init(&plugins->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


void hs_wait_output_plugins(hs_output_plugins* plugins)
{
  while (true) {
    if (plugins->list_cnt == 0) {
      pthread_mutex_lock(&plugins->list_lock);
      pthread_mutex_unlock(&plugins->list_lock);
      return;
    }
    usleep(100000);
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
  plugins->list_cnt = 0;
  plugins->list_cap = 0;
}


void hs_load_output_plugins(hs_output_plugins* plugins,
                            const hs_config* cfg,
                            const char* path)
{
  char dir[HS_MAX_PATH];
  if (!hs_get_fqfn(path, g_output, dir, sizeof(dir))) {
    hs_log(g_module, 0, "load path too long");
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
      hs_output_plugin* p = create_output_plugin(fqfn, cfg, &sbc, L);
      if (p) {
        p->plugins = plugins;

        size_t len = strlen(entry->d_name) + sizeof(g_output) + 1;
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
