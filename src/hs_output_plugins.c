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
  "os = {'getenv','exit','setlocale'},"
  "string = {'dump'},"
  "}"
  "}";

enum process_message_result {
  PMR_SENT = 0,
  PMR_FAIL = -1,
  PMR_SKIP = -2,
  PMR_RETRY = -3,
  PMR_BATCH = -4,
  PMR_ASYNC = -5
};

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


static void set_missing_headers(lua_State* lua, int idx,
                                const char* logger,
                                const char* hostname,
                                int pid)
{
  lua_getfield(lua, idx, HS_HEKA_LOGGER_KEY);
  int t = lua_type(lua, -1);
  lua_pop(lua, 1);
  if (t == LUA_TNIL) {
    lua_pushstring(lua, logger);
    lua_setfield(lua, 1, HS_HEKA_LOGGER_KEY);
  }

  lua_getfield(lua, idx, HS_HEKA_HOSTNAME_KEY);
  t = lua_type(lua, -1);
  lua_pop(lua, 1);
  if (t == LUA_TNIL) {
    lua_pushstring(lua, hostname);
    lua_setfield(lua, 1, HS_HEKA_HOSTNAME_KEY);
  }

  lua_getfield(lua, idx, HS_HEKA_PID_KEY);
  t = lua_type(lua, -1);
  lua_pop(lua, 1);
  if (t == LUA_TNIL) {
    lua_pushinteger(lua, pid);
    lua_setfield(lua, 1, HS_HEKA_PID_KEY);
  }
}


static int encode_message(lua_State* lua)
{
  int n = lua_gettop(lua);
  bool framed = false;

  switch (n) {
  case 2:
    luaL_checktype(lua, 2, LUA_TBOOLEAN);
    framed = lua_toboolean(lua, 2);
    // fall thru
  case 1:
    luaL_checktype(lua, 1, LUA_TTABLE);
    break;
  default:
    luaL_argerror(lua, n, "incorrect number of arguments");
  }

  lua_sandbox* lsb = lua_touserdata(lua, lua_upvalueindex(1));
  if (!lsb) {
    return luaL_error(lua, "encode_message() invalid upvalueindex");
  }
  hs_output_plugin* p = (hs_output_plugin*)lsb_get_parent(lsb);

  set_missing_headers(lua, 1, p->sb->name, p->plugins->cfg->hostname,
                      p->plugins->cfg->pid);

  if (lsb_output_protobuf(lsb, 1, 0) != 0) {
    return luaL_error(lua, "encode_message() could not encode protobuf - %s",
                      lsb_get_error(lsb));
  }
  size_t len = 0;
  const char* output = lsb_get_output(lsb, &len);

  if (framed) {
      unsigned char header[14] = "\x1e\x00\x08"; // up to 10 varint bytes
                                                 // and a \x1f
      int hlen = hs_write_varint(header + 3, len) + 1;
      header[1] = (char)hlen;
      header[hlen + 2] = '\x1f';
      luaL_Buffer b;
      luaL_buffinit(lua, &b);
      luaL_addlstring(&b, (char*)header, hlen + 3);
      luaL_addlstring(&b, output, len);
      luaL_pushresult(&b);
  } else {
    lua_pushlstring(lua, output, len);
  }
  return 1;
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


static void update_checkpoint(hs_output_plugin* p)
{
  pthread_mutex_lock(&p->cp_lock);
  p->cp.input.id = p->cur.input.id;
  p->cp.input.offset = p->cur.input.offset;
  p->cp.analysis.id = p->cur.analysis.id;
  p->cp.analysis.offset = p->cur.analysis.offset;
  pthread_mutex_unlock(&p->cp_lock);
}


static int batch_checkpoint_update(lua_State* lua)
{
  lua_sandbox* lsb = lua_touserdata(lua, lua_upvalueindex(1));
  if (!lsb) {
    return luaL_error(lua, "batch_checkpoint_update() invalid upvalueindex");
  }

  hs_output_plugin* p = (hs_output_plugin*)lsb_get_parent(lsb);
  if (p->batching) {
    update_checkpoint(p);
    p->batching = false;
  }
  return 0;
}


static hs_output_plugin* create_output_plugin(const hs_config* cfg,
                                              hs_sandbox_config* sbc)
{
  hs_output_plugin* p = calloc(1, sizeof(hs_output_plugin));
  if (!p) return NULL;
  p->list_index = -1;

  if (pthread_mutex_init(&p->cp_lock, NULL)) {
    perror("cp_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }

  p->sb = hs_create_output_sandbox(p, cfg, sbc);
  if (!p->sb) {
    free(p);
    hs_log(g_module, 3, "lsb_create_custom failed: %s/%s", sbc->dir,
           sbc->filename);
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
  } else {
    lsb_add_function(p->sb->lsb, &batch_checkpoint_update,
                     "batch_checkpoint_update");
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
    if (hs_timer_event(p->sb->lsb, time(NULL), true)) {
      hs_log(g_module, 3, "terminated: %s msg: %s", p->sb->name,
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
        case PMR_SENT:
          ++p->sb->stats.pm_cnt;
          p->batching = false;
          break;
        case PMR_FAIL:
          ++p->sb->stats.pm_cnt;
          ++p->sb->stats.pm_failures;
          break;
        case PMR_SKIP:
          ++p->sb->stats.pm_cnt;
          break;
        case PMR_RETRY:
          break;
        case PMR_ASYNC:
          if (!p->async_len) {
            lsb_terminate(p->sb->lsb, "cannot use async checkpointing without"
                          " a configured buffer");
            ret = 1;
          }
          // fall thru
        case PMR_BATCH:
          ++p->sb->stats.pm_cnt;
          p->batching = true;
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

        if (ret == PMR_FAIL) {
          const char* err = lsb_get_error(p->sb->lsb);
          if (strlen(err) > 0) {
            hs_log(g_module, 4, "file: %s received: %d %s", p->sb->name, ret,
                   err);
          }
        }
      }
    }

    // advance the checkpoint if not batching/async
    if (ret <= 0 && !p->batching) {
      update_checkpoint(p);
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
    te_ret = hs_timer_event(p->sb->lsb, current_t, false);
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);

    delta = hs_timespec_delta(&ts, &ts1);
    pthread_mutex_lock(&p->cp_lock);
    hs_update_running_stats(&p->sb->stats.te, delta);
    pthread_mutex_unlock(&p->cp_lock);

    p->sb->next_timer_event = current_t + p->sb->ticker_interval;
  }

  if (ret > 0 || te_ret > 0) {
    hs_log(g_module, 3, "terminated: %s msg: %s", p->sb->name,
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
  hs_log(g_module, 6, "starting: %s", p->sb->name);

  size_t bytes_read[2] = { 0 };
#ifdef HINDSIGHT_CLI
  bool input_stop = false, analysis_stop = false;
  while (!(p->stop && input_stop && analysis_stop)) {
#else
  while (!p->stop) {
#endif
    if (p->input.fh && !pim) {
      if (hs_find_message(&im, &p->input.ib, true)) {
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
        if (cid == p->input.ib.cp.id && p->stop) {
          input_stop = true;
        }
#endif
      }
    } else if (!p->input.fh) { // still waiting on the first file
      hs_open_file(&p->input, hs_input_dir, p->input.ib.cp.id);
#ifdef HINDSIGHT_CLI
      if (!p->input.fh && p->stop) {
        input_stop = true;
      }
#endif
    }

    if (p->analysis.fh && !pam) {
      if (hs_find_message(&am, &p->analysis.ib, true)) {
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
        if (cid == p->analysis.ib.cp.id && p->stop) {
          analysis_stop = true;
        }
#endif
      }
    } else if (!p->analysis.fh) { // still waiting on the first file
      hs_open_file(&p->analysis, hs_analysis_dir, p->analysis.ib.cp.id);
#ifdef HINDSIGHT_CLI
      if (!p->analysis.fh && p->stop) {
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
      if (ret == PMR_RETRY) {
        while (!p->stop && ret == PMR_RETRY) {
          const char* err = lsb_get_error(p->sb->lsb);
          hs_log(g_module, 7, "retry message %zu err: %s", p->sb->stats.pm_cnt,
                 err);
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

  // hold the current checkpoints in memory incase we restart it
  hs_update_input_checkpoint(&p->plugins->cfg->cp_reader,
                             hs_input_dir,
                             p->sb->name,
                             &p->cp.input);

  hs_update_input_checkpoint(&p->plugins->cfg->cp_reader,
                             hs_analysis_dir,
                             p->sb->name,
                             &p->cp.analysis);

  if (p->stop) {
    hs_log(g_module, 6, "shutting down: %s", p->sb->name);
  } else {
    hs_log(g_module, 6, "detaching: %s", p->sb->name);
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
  }
  pthread_exit(NULL);
}


static void remove_plugin(hs_output_plugins* plugins, int idx)
{
  hs_output_plugin* p = plugins->list[idx];
  plugins->list[idx] = NULL;
  p->stop = true;
  if (pthread_join(p->thread, NULL)) {
    hs_log(g_module, 3, "remove_plugin could not pthread_join");
  }
  free_output_plugin(p);
  free(p);
  --plugins->list_cnt;
}


static void remove_from_output_plugins(hs_output_plugins* plugins,
                                       const char* name)
{
  const size_t tlen = strlen(hs_output_dir) + 1;
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;

    char* pos = plugins->list[i]->sb->name + tlen;
    if (strstr(name, pos) && strlen(pos) == strlen(name) - HS_EXT_LEN) {
      remove_plugin(plugins, i);
      break;
    }
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


static void add_plugin(hs_output_plugins* plugins, hs_output_plugin* p, int idx)
{
  plugins->list[idx] = p;
  p->list_index = idx;
  ++plugins->list_cnt;
}

static void add_to_output_plugins(hs_output_plugins* plugins,
                                  hs_output_plugin* p)
{
  bool added = false;
  int idx = -1;
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) {
      idx = i;
    } else if (strcmp(plugins->list[i]->sb->name, p->sb->name) == 0) {
      idx = i;
      remove_plugin(plugins, idx);
      add_plugin(plugins, p, idx);
      added = true;
      break;
    }
  }
  if (!added && idx != -1) add_plugin(plugins, p, idx);

  if (idx == -1) {
    // todo probably don't want to grow it by 1
    ++plugins->list_cap;
    hs_output_plugin** tmp = realloc(plugins->list,
                                     sizeof(hs_output_plugin*)
                                     * plugins->list_cap);
    idx = plugins->list_cap - 1;

    if (tmp) {
      plugins->list = tmp;
      add_plugin(plugins, p, idx);
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
                             p->sb->name,
                             cfg->output_path,
                             &p->input.ib.cp);
  p->cur.input.id = p->cp.input.id = p->input.ib.cp.id;
  p->cur.input.offset = p->cp.input.offset = p->input.ib.cp.offset;

  hs_lookup_input_checkpoint(&cfg->cp_reader,
                             hs_analysis_dir,
                             p->sb->name,
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
  lsb_add_function(sb->lsb, &encode_message, "encode_message");

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
  if (pthread_mutex_init(&plugins->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


void hs_wait_output_plugins(hs_output_plugins* plugins)
{
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;

    hs_output_plugin* p = plugins->list[i];
    plugins->list[i] = NULL;
    if (pthread_join(p->thread, NULL)) {
      hs_log(g_module, 3, "thread could not be joined");
    }
    free_output_plugin(p);
    free(p);
    --plugins->list_cnt;
  }
  pthread_mutex_unlock(&plugins->list_lock);
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


static void process_lua(hs_output_plugins* plugins, const char* lpath,
                        const char* rpath, DIR* dp)
{
  char lua_lpath[HS_MAX_PATH];
  char lua_rpath[HS_MAX_PATH];
  char cfg_lpath[HS_MAX_PATH];
  char cfg_rpath[HS_MAX_PATH];
  size_t tlen = strlen(hs_output_dir) + 1;

  struct dirent* entry;
  while ((entry = readdir(dp))) {
    if (hs_has_ext(entry->d_name, hs_lua_ext)) {
      // move the Lua to the run directory
      if (!hs_get_fqfn(lpath, entry->d_name, lua_lpath, sizeof(lua_lpath))) {
        hs_log(g_module, 0, "load lua path too long");
        exit(EXIT_FAILURE);
      }
      if (!hs_get_fqfn(rpath, entry->d_name, lua_rpath, sizeof(lua_rpath))) {
        hs_log(g_module, 0, "run lua path too long");
        exit(EXIT_FAILURE);
      }
      if (rename(lua_lpath, lua_rpath)) {
        hs_log(g_module, 3, "failed to move: %s to %s errno: %d", lua_lpath,
               lua_rpath, errno);
        continue;
      }

      // restart any plugins using this Lua code
      pthread_mutex_lock(&plugins->list_lock);
      for (int i = 0; i < plugins->list_cap; ++i) {
        if (!plugins->list[i]) continue;

        hs_output_plugin* p = plugins->list[i];
        if (strcmp(lua_rpath, lsb_get_lua_file(p->sb->lsb)) == 0) {
          int ret = snprintf(cfg_lpath, HS_MAX_PATH, "%s/%s%s", lpath,
                             p->sb->name + tlen, hs_cfg_ext);
          if (ret < 0 || ret > HS_MAX_PATH - 1) {
            hs_log(g_module, 0, "load cfg path too long");
            exit(EXIT_FAILURE);
          }

          ret = snprintf(cfg_rpath, HS_MAX_PATH, "%s/%s%s", rpath,
                         p->sb->name + tlen, hs_cfg_ext);
          if (ret < 0 || ret > HS_MAX_PATH - 1) {
            hs_log(g_module, 0, "run cfg path too long");
            exit(EXIT_FAILURE);
          }

          // if no new cfg was provided, move the existing cfg to the load
          // directory
          if (!hs_file_exists(cfg_lpath)) {
            if (rename(cfg_rpath, cfg_lpath)) {
              hs_log(g_module, 3, "failed to move: %s to %s errno: %d",
                     cfg_rpath, cfg_lpath, errno);
            }
          }
        }
      }
      pthread_mutex_unlock(&plugins->list_lock);
    }
  }
  rewinddir(dp);
}


void hs_load_output_plugins(hs_output_plugins* plugins, const hs_config* cfg,
                            bool dynamic)
{
  char lpath[HS_MAX_PATH];
  char rpath[HS_MAX_PATH];
  if (!hs_get_fqfn(cfg->load_path, hs_output_dir, lpath, sizeof(lpath))) {
    hs_log(g_module, 0, "load path too long");
    exit(EXIT_FAILURE);
  }
  if (!hs_get_fqfn(cfg->run_path, hs_output_dir, rpath, sizeof(rpath))) {
    hs_log(g_module, 0, "run path too long");
    exit(EXIT_FAILURE);
  }

  const char* dir = dynamic ? lpath : rpath;
  DIR* dp = opendir(dir);
  if (dp == NULL) {
    hs_log(g_module, 0, "%s: %s", dir, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (dynamic) process_lua(plugins, lpath, rpath, dp);

  struct dirent* entry;
  while ((entry = readdir(dp))) {
    if (dynamic) {
      int ret = hs_process_load_cfg(lpath, rpath, entry->d_name);
      switch (ret) {
      case 0:
        remove_from_output_plugins(plugins, entry->d_name);
        break;
      case 1: // proceed to load
        break;
      default: // ignore
        continue;
      }
    }
    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(rpath, entry->d_name, &sbc, &cfg->opd,
                               HS_SB_TYPE_OUTPUT)) {
      hs_output_plugin* p = create_output_plugin(cfg, &sbc);
      if (p) {
        p->plugins = plugins;
        hs_init_input(&p->input, cfg->max_message_size, cfg->output_path,
                      p->sb->name);
        hs_init_input(&p->analysis, cfg->max_message_size, cfg->output_path,
                      p->sb->name);

        p->sb->mm = hs_create_message_matcher(plugins->mmb,
                                              sbc.message_matcher);
        int ret = hs_init_output_sandbox(p->sb);
        if (!p->sb->mm || ret) {
          if (!p->sb->mm) {
            hs_log(g_module, 3, "file: %s invalid message_matcher: %s",
                   p->sb->name, sbc.message_matcher);
          } else {
            hs_log(g_module, 3, "lsb_init() file: %s received: %d %s",
                   p->sb->name,
                   ret,
                   lsb_get_error(p->sb->lsb));
          }
          free_output_plugin(p);
          free(p);
          p = NULL;
          hs_free_sandbox_config(&sbc);
          continue;
        }
        add_to_output_plugins(plugins, p);
      }
      hs_free_sandbox_config(&sbc);
    }
  }
  closedir(dp);
}


void hs_stop_output_plugins(hs_output_plugins* plugins)
{
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;
    plugins->list[i]->stop = true;
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


hs_sandbox* hs_create_output_sandbox(void* parent,
                                     const hs_config* cfg,
                                     hs_sandbox_config* sbc)
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

  return hs_create_sandbox(parent, lsb_config, sbc);
}
