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
  "[''] = {'dofile','load','loadfile','loadstring','newproxy','print'},"
  "os = {'getenv','exit','setlocale'}"
  "}"
  "}";


static int read_message(lua_State* lua)
{
  lua_sandbox* lsb = lua_touserdata(lua, lua_upvalueindex(1));
  if (!lsb) {
    return luaL_error(lua, "read_message() invalid upvalueindex");
  }
  hs_output_plugin* p = (hs_output_plugin*)lsb_get_parent(lsb);

  if (!p->matched || !p->msg) {
    lua_pushnil(lua);
    return 1;
  }
  return hs_read_message(lua, p->msg);
}


static int async_checkpoint_update(lua_State* lua)
{
  lua_sandbox* lsb = lua_touserdata(lua, lua_upvalueindex(1));
  if (!lsb) {
    return luaL_error(lua, "async_checkpoint_update() invalid upvalueindex");
  }

  luaL_checktype(lua, 1, LUA_TLIGHTUSERDATA);
  luaL_checktype(lua, 2, LUA_TNUMBER);

  size_t sequence_id = (size_t)lua_touserdata(lua, 1);
  size_t failures = lua_tonumber(lua, 2);

  hs_output_plugin* p = (hs_output_plugin*)lsb_get_parent(lsb);
  int i = sequence_id % p->async_len;
  pthread_mutex_lock(&p->cp_lock);
  if (p->async_cp[i].input.id >= p->cp.input.id
      && p->async_cp[i].input.offset > p->cp.input.offset) {
    p->cp.input.id = p->async_cp[i].input.id;
    p->cp.input.offset = p->async_cp[i].input.offset;
  }
  if (p->async_cp[i].analysis.id >= p->cp.analysis.id
      && p->async_cp[i].analysis.offset > p->cp.analysis.offset) {
    p->cp.analysis.id = p->async_cp[i].analysis.id;
    p->cp.analysis.offset = p->async_cp[i].analysis.offset;
  }
  p->sb->stats.pm_failures += failures;
  pthread_mutex_unlock(&p->cp_lock);
  return 0;
}


static hs_output_plugin* create_output_plugin(const char* file,
                                              const hs_config* cfg,
                                              const hs_sandbox_config* sbc,
                                              lua_State* env)
{
  hs_output_plugin* p = calloc(1, sizeof(hs_output_plugin));
  if (!p) return NULL;
  p->list_index = -1;

  if (pthread_mutex_init(&p->cp_lock, NULL)) {
    perror("cp_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }

  p->sb = hs_create_output_sandbox(p, file, cfg, sbc, env);
  if (!p->sb) {
    free(p);
    hs_log(g_module, 3, "lsb_create_custom failed: %s", file);
    return NULL;
  }

  if (sbc->async_buffer_size > 0) {
    p->async_len = sbc->async_buffer_size;
    p->async_cp = calloc(p->async_len, sizeof(hs_checkpoint_pair));
    if (!p->async_cp) {
      hs_free_sandbox(p->sb);
      free(p);
      hs_log(g_module, 3, "failed allocating the async buffer");
      return NULL;
    }
    lsb_add_function(p->sb->lsb, &async_checkpoint_update,
                     "async_checkpoint_update");
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

  free(p->async_cp);
  p->async_cp = NULL;
  p->async_len = 0;

  pthread_mutex_destroy(&p->cp_lock);
}


static void shutdown_timer_event(hs_output_plugin* p)
{
  if (lsb_get_state(p->sb->lsb) != LSB_TERMINATED) {
    if (hs_timer_event(p->sb->lsb, time(NULL))) {
      hs_log(g_module, 3, "terminated: %s msg: %s", p->sb->filename,
             lsb_get_error(p->sb->lsb));
    }
  }
}


static int output_message(hs_output_plugin* p)
{
  int ret = 0, te_ret = 0;
  bool sample = p->sample;
  time_t current_t = time(NULL);
  p->matched = false;

  struct timespec ts, ts1;
  double delta = 0;
  size_t sequence_id = 0;

  if (p->msg->msg) { // non idle/empty message
    double mmdelta = 0;
    if (sample) clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    p->matched = hs_eval_message_matcher(p->sb->mm, p->msg);
    if (sample) {
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);
      mmdelta = hs_timespec_delta(&ts, &ts1);
    }
    if (p->matched) {
      if (p->async_len) {
        sequence_id = p->sb->stats.pm_cnt + 1;
        int i = sequence_id % p->async_len;
        p->async_cp[i].input.id = p->cur.input.id;
        p->async_cp[i].input.offset = p->cur.input.offset;
        p->async_cp[i].analysis.id = p->cur.analysis.id;
        p->async_cp[i].analysis.offset = p->cur.analysis.offset;
      }
      ret = hs_process_message(p->sb->lsb, (void*)(sequence_id));
      if (sample) {
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        delta = hs_timespec_delta(&ts1, &ts);
      }
      if (ret <= 0) {
        pthread_mutex_lock(&p->cp_lock);
        switch (ret) {
        case 0: // sent
          ++p->sb->stats.pm_cnt;
          p->batching = false;
          break;
        case -1: // failure
          ++p->sb->stats.pm_cnt;
          ++p->sb->stats.pm_failures;
          break;
        case -2: // skip
          ++p->sb->stats.pm_cnt;
          break;
        case -5: // async update
          if (!p->async_len) {
            lsb_terminate(p->sb->lsb, "cannot use async checkpointing without"
                          " a configured buffer");
            ret = 1;
          }
          // fall thru
        case -3: // batching
          ++p->sb->stats.pm_cnt;
          p->batching = true;
          break;
        case -4: // retry
          break;
        default:
          lsb_terminate(p->sb->lsb, "invalid process_message return status");
          ret = 1;
          break;
        }

        // update the stats
        if (sample) {
          hs_update_running_stats(&p->sb->stats.mm, mmdelta);
          hs_update_running_stats(&p->sb->stats.pm, delta);
          p->sample = false;
          mmdelta = 0;
        }
        p->sb->stats.cur_memory = lsb_usage(p->sb->lsb, LSB_UT_MEMORY,
                                            LSB_US_CURRENT);
        p->sb->stats.max_memory = lsb_usage(p->sb->lsb, LSB_UT_MEMORY,
                                            LSB_US_MAXIMUM);
        p->sb->stats.max_output = lsb_usage(p->sb->lsb, LSB_UT_OUTPUT,
                                            LSB_US_MAXIMUM);
        p->sb->stats.max_instructions = lsb_usage(p->sb->lsb,
                                                  LSB_UT_INSTRUCTION,
                                                  LSB_US_MAXIMUM);
        pthread_mutex_unlock(&p->cp_lock);
      }
    }

    // advance the checkpoint if not batching/async
    if (ret <= 0 && !p->batching) {
      pthread_mutex_lock(&p->cp_lock);
      p->cp.input.id = p->cur.input.id;
      p->cp.input.offset = p->cur.input.offset;
      p->cp.analysis.id = p->cur.analysis.id;
      p->cp.analysis.offset = p->cur.analysis.offset;
      pthread_mutex_unlock(&p->cp_lock);
    }

    if (mmdelta) {
      pthread_mutex_lock(&p->cp_lock);
      hs_update_running_stats(&p->sb->stats.mm, mmdelta);
      p->sample = false;
      pthread_mutex_unlock(&p->cp_lock);
    }
  }

  if (ret <= 0 && p->sb->ticker_interval
      && current_t >= p->sb->next_timer_event) {
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    te_ret = hs_timer_event(p->sb->lsb, current_t);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);

    delta = hs_timespec_delta(&ts, &ts1);
    pthread_mutex_lock(&p->cp_lock);
    hs_update_running_stats(&p->sb->stats.te, delta);
    pthread_mutex_unlock(&p->cp_lock);

    p->sb->next_timer_event = current_t + p->sb->ticker_interval;
  }

  if (ret > 0 || te_ret > 0) {
    hs_log(g_module, 3, "terminated: %s msg: %s", p->sb->filename,
           lsb_get_error(p->sb->lsb));
    return 1;
  }
  return ret;
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
#ifdef HINDSIGHT_CLI
  bool input_stop = false, analysis_stop = false;
  while (!(p->plugins->stop && input_stop && analysis_stop)) {
#else
  while (!p->plugins->stop) {
#endif
    if (p->input.fh && !pim) {
      if (hs_find_message(&im, &p->input.ib)) {
        pim = &im;
      } else {
        bytes_read[0] = hs_read_file(&p->input);
      }

      if (!bytes_read[0]) {
#ifdef HINDSIGHT_CLI
        size_t cid = p->input.ib.cp.id;
#endif
        // see if the next file is there yet
        hs_open_file(&p->input, hs_input_dir, p->input.ib.cp.id + 1);
#ifdef HINDSIGHT_CLI
        if (cid == p->input.ib.cp.id && p->plugins->stop) {
          input_stop = true;
        }
#endif
      }
    } else if (!p->input.fh) { // still waiting on the first file
      hs_open_file(&p->input, hs_input_dir, p->input.ib.cp.id);
#ifdef HINDSIGHT_CLI
      if (!p->input.fh && p->plugins->stop) {
        input_stop = true;
      }
#endif
    }

    if (p->analysis.fh && !pam) {
      if (hs_find_message(&am, &p->analysis.ib)) {
        pam = &am;
      } else {
        bytes_read[1] = hs_read_file(&p->analysis);
      }

      if (!bytes_read[1]) {
#ifdef HINDSIGHT_CLI
        size_t cid = p->analysis.ib.cp.id;
#endif
        // see if the next file is there yet
        hs_open_file(&p->analysis, hs_analysis_dir, p->analysis.ib.cp.id + 1);
#ifdef HINDSIGHT_CLI
        if (cid == p->analysis.ib.cp.id && p->plugins->stop) {
          analysis_stop = true;
        }
#endif
      }
    } else if (!p->analysis.fh) { // still waiting on the first file
      hs_open_file(&p->analysis, hs_analysis_dir, p->analysis.ib.cp.id);
#ifdef HINDSIGHT_CLI
      if (!p->analysis.fh && p->plugins->stop) {
        analysis_stop = true;
      }
#endif
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
        p->cur.input.id = p->input.ib.cp.id;
        p->cur.input.offset = p->input.ib.cp.offset -
          (p->input.ib.readpos - p->input.ib.scanpos);
      } else {
        pam = NULL;
        p->cur.analysis.id = p->analysis.ib.cp.id;
        p->cur.analysis.offset = p->analysis.ib.cp.offset -
          (p->analysis.ib.readpos - p->analysis.ib.scanpos);
      }
      int ret = output_message(p);
      if (ret == -4) {
        while (!p->plugins->stop && ret == -4) {
          hs_log(g_module, 7, "retry message %zu", p->sb->stats.pm_cnt);
          sleep(1);
          ret = output_message(p);
        }
      }
      if (ret > 0) {
        break; // fatal error
      }
      p->msg = NULL;
    } else if (!bytes_read[0] && !bytes_read[1]) {
      // trigger any pending timer events
      hs_clear_heka_message(&im); // create an idle/empty message
      p->msg = &im;
      output_message(p);
      p->msg = NULL;
      sleep(1);
    }
  }

  shutdown_timer_event(p);
  hs_free_heka_message(&am);
  hs_free_heka_message(&im);

  hs_log(g_module, 6, "detaching: %s", p->sb->filename);

  // hold the current checkpoints in memory incase we restart it
  hs_update_input_checkpoint(&p->plugins->cfg->cp_reader,
                             hs_input_dir,
                             p->sb->filename,
                             &p->cp.input);

  hs_update_input_checkpoint(&p->plugins->cfg->cp_reader,
                             hs_analysis_dir,
                             p->sb->filename,
                             &p->cp.analysis);

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
  // sync the output and read checkpoints
  // the read and output checkpoints can differ to allow for batching
  hs_lookup_input_checkpoint(&cfg->cp_reader,
                             hs_input_dir,
                             p->sb->filename,
                             cfg->output_path,
                             &p->input.ib.cp);
  p->cur.input.id = p->cp.input.id = p->input.ib.cp.id;
  p->cur.input.offset = p->cp.input.offset = p->input.ib.cp.offset;

  hs_lookup_input_checkpoint(&cfg->cp_reader,
                             hs_analysis_dir,
                             p->sb->filename,
                             cfg->output_path,
                             &p->analysis.ib.cp);
  p->cur.analysis.id = p->cp.analysis.id = p->analysis.ib.cp.id;
  p->cur.analysis.offset = p->cp.analysis.offset = p->analysis.ib.cp.offset;

  int ret = pthread_create(&p->thread, NULL, input_thread, (void*)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
}


int hs_init_output_sandbox(hs_sandbox* sb)
{
  lsb_add_function(sb->lsb, &read_message, "read_message");

  int ret = lsb_init(sb->lsb, sb->state);
  if (ret) return ret;

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

        hs_init_input(&p->input, cfg->max_message_size, cfg->output_path,
                      p->sb->filename);
        hs_init_input(&p->analysis, cfg->max_message_size, cfg->output_path,
                      p->sb->filename);

        if (sbc.preserve_data) {
          len = strlen(fqfn);
          p->sb->state = malloc(len + 1);
          memcpy(p->sb->state, fqfn, len + 1);
          memcpy(p->sb->state + len - 3, "dat", 3);
        }

        p->sb->mm = hs_create_message_matcher(plugins->mmb,
                                              sbc.message_matcher);
        int ret = hs_init_output_sandbox(p->sb);
        if (!p->sb->mm || ret) {
          if (!p->sb->mm) {
            hs_log(g_module, 3, "file: %s invalid message_matcher: %s",
                   p->sb->filename, sbc.message_matcher);
          } else {
            hs_log(g_module, 3, "lsb_init() file: %s received: %d %s",
                   p->sb->filename,
                   ret,
                   lsb_get_error(p->sb->lsb));
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


hs_sandbox* hs_create_output_sandbox(void* parent,
                                     const char* file,
                                     const hs_config* cfg,
                                     const hs_sandbox_config* sbc,
                                     lua_State* env)
{
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

  return hs_create_sandbox(parent, file, lsb_config, sbc, env);
}
