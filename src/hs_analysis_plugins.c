/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight analysis plugin loader @file */

#include "hs_analysis_plugins.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <luasandbox.h>
#include <luasandbox/lauxlib.h>
#include <luasandbox_output.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hs_input.h"
#include "hs_logger.h"
#include "hs_message_matcher.h"
#include "hs_output.h"
#include "hs_sandbox.h"
#include "hs_util.h"

static const char g_module[] = "analysis_plugins";
static const char* g_sb_template = "{"
  "memory_limit = %u,"
  "instruction_limit = %u,"
  "output_limit = %u,"
  "path = [[%s]],"
  "cpath = [[%s]],"
  "remove_entries = {"
  "[''] = {'collectgarbage','coroutine','dofile','load','loadfile'"
  ",'loadstring','newproxy','print'},"
  "os = {'getenv','execute','exit','remove','rename','setlocale','tmpname'}"
  "},"
  "disable_modules = {io = 1}"
  "}";


static int read_message(lua_State* lua)
{
  void* luserdata = lua_touserdata(lua, lua_upvalueindex(1));
  if (NULL == luserdata) {
    return luaL_error(lua, "read_message() invalid lightuserdata");
  }
  lua_sandbox* lsb = (lua_sandbox*)luserdata;
  hs_analysis_plugin* p = (hs_analysis_plugin*)lsb_get_parent(lsb);

  if (!p->at->matched || !p->at->msg) {
    lua_pushnil(lua);
    return 1;
  }
  return hs_read_message(lua, p->at->msg);
}


static int inject_message(lua_State* L)
{
  static unsigned char header[14];
  void* luserdata = lua_touserdata(L, lua_upvalueindex(1));
  if (NULL == luserdata) {
    return luaL_error(L, "inject_message() invalid lightuserdata");
  }
  lua_sandbox* lsb = (lua_sandbox*)luserdata;
  hs_analysis_plugin* p = (hs_analysis_plugin*)lsb_get_parent(lsb);

  if (lua_type(L, 1) == LUA_TTABLE) {
    lua_pushstring(L, p->sb->name);
    lua_setfield(L, 1, "Logger");
    lua_pushstring(L, p->at->plugins->cfg->hostname);
    lua_setfield(L, 1, "Hostname");
    lua_pushinteger(L, p->at->plugins->cfg->pid);
    lua_setfield(L, 1, "Pid");
  }

  if (lsb_output_protobuf(lsb, 1, 0) != 0) {
    return luaL_error(L, "inject_message() could not encode protobuf - %s",
                      lsb_get_error(lsb));
  }

  size_t output_len = 0;
  const char* output = lsb_get_output(lsb, &output_len);

  pthread_mutex_lock(&p->at->plugins->output.lock);
  int len = hs_write_varint(header + 3, output_len);
  int tlen = 4 + len + output_len;
  ++p->sb->stats.im_cnt;
  p->sb->stats.im_bytes += tlen;

  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  fwrite(header, 4 + len, 1, p->at->plugins->output.fh);
  fwrite(output, output_len, 1, p->at->plugins->output.fh);
  p->at->plugins->output.cp.offset += tlen;
  if (p->at->plugins->output.cp.offset >= (size_t)p->at->plugins->cfg->output_size) {
    ++p->at->plugins->output.cp.id;
    hs_open_output_file(&p->at->plugins->output);
  }
  pthread_mutex_unlock(&p->at->plugins->output.lock);
  return 0;
}


static int inject_payload(lua_State* lua)
{
  static const char* default_type = "txt";
  static const char* func_name = "inject_payload";

  void* luserdata = lua_touserdata(lua, lua_upvalueindex(1));
  if (NULL == luserdata) {
    return luaL_error(lua, "%s invalid lightuserdata", func_name);
  }
  lua_sandbox* lsb = (lua_sandbox*)luserdata;

  int n = lua_gettop(lua);
  if (n > 2) {
    lsb_output(lsb, 3, n, 1);
    lua_pop(lua, n - 2);
  }
  size_t len = 0;
  const char* output = lsb_get_output(lsb, &len);
  if (!len) return 0;

  if (n > 0) {
    if (lua_type(lua, 1) != LUA_TSTRING) {
      return luaL_error(lua, "%s() payload_type argument must be a string",
                        func_name);
    }
  }

  if (n > 1) {
    if (lua_type(lua, 2) != LUA_TSTRING) {
      return luaL_error(lua, "%s() payload_name argument must be a string",
                        func_name);
    }
  }

  // build up a heka message table
  lua_createtable(lua, 0, 2); // message
  lua_createtable(lua, 0, 2); // Fields
  if (n > 0) {
    lua_pushvalue(lua, 1);
  } else {
    lua_pushstring(lua, default_type);
  }
  lua_setfield(lua, -2, "payload_type");

  if (n > 1) {
    lua_pushvalue(lua, 2);
    lua_setfield(lua, -2, "payload_name");
  }
  lua_setfield(lua, -2, "Fields");
  lua_pushstring(lua, "inject_payload");
  lua_setfield(lua, -2, "Type");
  lua_pushlstring(lua, output, len);
  lua_setfield(lua, -2, "Payload");
  lua_replace(lua, 1);

  // use inject_message to actually deliver it
  lua_getglobal(lua, "inject_message");
  lua_CFunction fp = lua_tocfunction(lua, -1);
  lua_pop(lua, 1); // remove function pointer
  if (fp) {
    fp(lua);
  } else {
    return luaL_error(lua, "%s() failed to call inject_message",
                      func_name);
  }
  return 0;
}


static void free_analysis_plugin(hs_analysis_plugin* p)
{
  if (!p->sb) return;

  hs_free_sandbox(p->sb);
  free(p->sb);
  p->sb = NULL;
  p->at = NULL;
}


static void remove_plugin(hs_analysis_thread* at, int idx)
{
  hs_log(g_module, 6, "analysis thread: %d stopping: %s", at->tid,
         at->list[idx]->sb->name);
  hs_analysis_plugin* p = at->list[idx];
  at->list[idx] = NULL;
  free_analysis_plugin(p);
  free(p);
  --at->list_cnt;
}


static void remove_from_analysis_plugins(hs_analysis_thread* at,
                                         const char* name)
{
  const size_t tlen = strlen(hs_analysis_dir) + 1;
  pthread_mutex_lock(&at->list_lock);
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;

    char* pos = at->list[i]->sb->name + tlen;
    if (strstr(name, pos) && strlen(pos) == strlen(name) - HS_EXT_LEN) {
      remove_plugin(at, i);
      break;
    }
  }
  pthread_mutex_unlock(&at->list_lock);
}


static void add_plugin(hs_analysis_thread* at, hs_analysis_plugin* p, int idx)
{
  hs_log(g_module, 6, "analysis thread: %d starting: %s", at->tid, p->sb->name);
  at->list[idx] = p;
  ++at->list_cnt;
}


static void add_to_analysis_plugins(const hs_sandbox_config* cfg,
                                    hs_analysis_plugins* plugins,
                                    hs_analysis_plugin* p)
{
  bool added = false;
  int idx = -1;
  int thread = cfg->thread % plugins->cfg->analysis_threads;
  hs_analysis_thread* at = &plugins->list[thread];
  p->at = at;

  pthread_mutex_lock(&at->list_lock);
  // todo shrink it down if there are a lot of empty slots
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) {
      idx = i;
    } else if (strcmp(at->list[i]->sb->name, p->sb->name) == 0) {
      idx = i;
      remove_plugin(at, idx);
      add_plugin(at, p, idx);
      added = true;
      break;
    }
  }
  if (!added && idx != -1) add_plugin(at, p, idx);

  if (idx == -1) {
    ++at->list_cap;
    // todo probably don't want to grow it by 1
    hs_analysis_plugin** tmp = realloc(at->list,
                                       sizeof(hs_analysis_plugin*)
                                       * at->list_cap);
    idx = at->list_cap - 1;
    if (tmp) {
      at->list = tmp;
      add_plugin(at, p, idx);
    } else {
      hs_log(g_module, 0, "plugins realloc failed");
      exit(EXIT_FAILURE);
    }
  }
  pthread_mutex_unlock(&at->list_lock);
}


static void init_analysis_thread(hs_analysis_plugins* plugins, int tid)
{
  hs_analysis_thread* at = &plugins->list[tid];
  at->plugins = plugins;
  at->list = NULL;
  at->msg = NULL;
  if (pthread_mutex_init(&at->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
  if (pthread_mutex_init(&at->cp_lock, NULL)) {
    perror("cp_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
  at->cp.id = 0;
  at->cp.offset = 0;
  at->current_t = 0;
  at->list_cap = 0;
  at->list_cnt = 0;
  at->tid = tid;
  at->matched = false;

  char name[255];
  int n = snprintf(name, sizeof name, "%s%d", hs_analysis_dir, tid);
  if (n < 0 || n >= (int)sizeof name) {
    hs_log(g_module, 0, "name exceeded the buffer length: %s%d",
           hs_analysis_dir, tid);
    exit(EXIT_FAILURE);
  }

  hs_init_input(&at->input, plugins->cfg->max_message_size,
                plugins->cfg->output_path, name);
}


static void free_analysis_thread(hs_analysis_thread* at)
{
  pthread_mutex_destroy(&at->cp_lock);
  pthread_mutex_destroy(&at->list_lock);
  at->plugins = NULL;
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;
    remove_plugin(at, i);
  }
  free(at->list);
  at->list = NULL;
  at->msg = NULL;
  at->cp.id = 0;
  at->cp.offset = 0;
  at->current_t = 0;
  at->list_cap = 0;
  at->list_cnt = 0;
  at->tid = 0;
  at->matched = false;

  hs_free_input(&at->input);
}


int hs_init_analysis_sandbox(hs_sandbox* sb, lua_CFunction im_fp)
{
  if (!im_fp) return -1;

  lsb_add_function(sb->lsb, &read_message, "read_message");
  lsb_add_function(sb->lsb, im_fp, "inject_message");
  lsb_add_function(sb->lsb, &inject_payload, "inject_payload");

  int ret = lsb_init(sb->lsb, sb->state);
  if (ret) return ret;

  lua_State* lua = lsb_get_lua(sb->lsb);
  // rename output to add_to_payload
  lua_getglobal(lua, "output");
  lua_setglobal(lua, "add_to_payload");
  lua_pushnil(lua);
  lua_setglobal(lua, "output");
  return 0;
}


static void terminate_sandbox(hs_analysis_thread* at, int i)
{
  hs_log(g_module, 3, "terminated: %s msg: %s", at->list[i]->sb->name,
         lsb_get_error(at->list[i]->sb->lsb));
  remove_plugin(at, i);
}


static void analyze_message(hs_analysis_thread* at)
{
  hs_sandbox* sb = NULL;
  bool sample = at->plugins->sample;
  int ret;

  pthread_mutex_lock(&at->list_lock);
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;
    sb = at->list[i]->sb;

    ret = 0;
    struct timespec ts, ts1;

    if (at->msg->msg) { // non idle/empty message
      if (sample) clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
      at->matched = hs_eval_message_matcher(sb->mm, at->msg);
      if (sample) {
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);
        hs_update_running_stats(&sb->stats.mm, hs_timespec_delta(&ts, &ts1));
      }
      if (at->matched) {
        ret = hs_process_message(sb->lsb, NULL);
        if (sample) {
          clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
          hs_update_running_stats(&sb->stats.pm, hs_timespec_delta(&ts1,
                                                                   &ts));
        }

        ++sb->stats.pm_cnt;
        if (ret < 0) {
          ++sb->stats.pm_failures;
        }
      }
    }

    if (ret <= 0 && sb->ticker_interval
        && at->current_t >= sb->next_timer_event) {
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
      ret = hs_timer_event(sb->lsb, at->current_t);
      clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);
      hs_update_running_stats(&sb->stats.te, hs_timespec_delta(&ts, &ts1));
      sb->next_timer_event = at->current_t + sb->ticker_interval;
    }

    if (ret > 0) terminate_sandbox(at, i);
  }
  pthread_mutex_unlock(&at->list_lock);
}


static void shutdown_timer_event(hs_analysis_thread* at)
{
  pthread_mutex_lock(&at->list_lock);
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;

    if (hs_timer_event(at->list[i]->sb->lsb, at->current_t)) {
      terminate_sandbox(at, i);
    }
  }
  pthread_mutex_unlock(&at->list_lock);
}


static hs_analysis_plugin* create_analysis_plugin(const hs_config* cfg,
                                                  hs_sandbox_config* sbc)
{
  hs_analysis_plugin* p = calloc(1, sizeof(hs_analysis_plugin));
  if (!p) return NULL;

  p->sb = hs_create_analysis_sandbox(p, cfg, sbc);
  if (!p->sb) {
    free(p);
    hs_log(g_module, 3, "lsb_create_custom failed: %s/%s", sbc->dir,
           sbc->filename);
    return NULL;
  }

  return p;
}


static void* input_thread(void* arg)
{
  hs_analysis_thread* at = (hs_analysis_thread*)arg;
  hs_log(g_module, 6, "starting input thread: %d", at->tid);

  hs_heka_message msg;
  hs_init_heka_message(&msg, 8);

  hs_config* cfg = at->plugins->cfg;
  hs_lookup_input_checkpoint(&cfg->cp_reader,
                             hs_input_dir,
                             at->input.name,
                             cfg->output_path,
                             &at->input.ib.cp);
  at->cp.id = at->input.ib.cp.id;
  at->cp.offset = at->input.ib.cp.offset;

  size_t bytes_read = 0;
#ifdef HINDSIGHT_CLI
  bool input_stop = false;
  while (!(at->plugins->stop && input_stop)) {
#else
  while (!at->plugins->stop) {
#endif
    if (at->input.fh) {
      if (hs_find_message(&msg, &at->input.ib)) {
        at->msg = &msg;
        at->current_t = time(NULL);
        analyze_message(at);

        // advance the checkpoint
        pthread_mutex_lock(&at->cp_lock);
        at->plugins->sample = false;
        at->cp.id = at->input.ib.cp.id;
        at->cp.offset = at->input.ib.cp.offset -
          (at->input.ib.readpos - at->input.ib.scanpos);
        pthread_mutex_unlock(&at->cp_lock);
      } else {
        bytes_read = hs_read_file(&at->input);
      }

      if (!bytes_read) {
#ifdef HINDSIGHT_CLI
        size_t cid = at->input.ib.cp.id;
#endif
        // see if the next file is there yet
        hs_open_file(&at->input, hs_input_dir, at->input.ib.cp.id + 1);
#ifdef HINDSIGHT_CLI
        if (cid == at->input.ib.cp.id && at->plugins->stop) {
          input_stop = true;
        }
#endif
      }
    } else { // still waiting on the first file
      hs_open_file(&at->input, hs_input_dir, at->input.ib.cp.id);
#ifdef HINDSIGHT_CLI
      if (!at->input.fh && at->plugins->stop) {
        input_stop = true;
      }
#endif
    }

    if (bytes_read || at->msg) {
      at->msg = NULL;
    } else {
      // trigger any pending timer events
      hs_clear_heka_message(&msg); // create an idle/empty message
      at->msg = &msg;
      at->current_t = time(NULL);
      analyze_message(at);
      at->msg = NULL;
      sleep(1);
    }
  }
  shutdown_timer_event(at);
  hs_free_heka_message(&msg);
  hs_log(g_module, 6, "exiting input_thread: %d", at->tid);
  pthread_exit(NULL);
}


void hs_init_analysis_plugins(hs_analysis_plugins* plugins,
                              hs_config* cfg,
                              hs_message_match_builder* mmb)
{
  hs_init_output(&plugins->output, cfg->output_path, hs_analysis_dir);

  plugins->thread_cnt = cfg->analysis_threads;
  plugins->cfg = cfg;
  plugins->stop = false;
  plugins->sample = false;
  plugins->mmb = mmb;

  plugins->list = malloc(sizeof(hs_analysis_thread) * cfg->analysis_threads);
  for (unsigned i = 0; i < cfg->analysis_threads; ++i) {
    init_analysis_thread(plugins, i);
  }
  plugins->threads = malloc(sizeof(pthread_t*) * (cfg->analysis_threads));
}


void hs_wait_analysis_plugins(hs_analysis_plugins* plugins)
{
  void* thread_result;
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    if (pthread_join(plugins->threads[i], &thread_result)) {
      hs_log(g_module, 3, "thread could not be joined");
    }
  }
  free(plugins->threads);
  plugins->threads = NULL;
}


void hs_free_analysis_plugins(hs_analysis_plugins* plugins)
{
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    free_analysis_thread(&plugins->list[i]);
  }
  free(plugins->list);
  plugins->list = NULL;

  hs_free_output(&plugins->output);

  plugins->cfg = NULL;
  plugins->thread_cnt = 0;
  plugins->sample = false;
}


static void process_lua(hs_analysis_plugins* plugins, const char* lpath,
                        const char* rpath, DIR* dp)
{
  char lua_lpath[HS_MAX_PATH];
  char lua_rpath[HS_MAX_PATH];
  char cfg_lpath[HS_MAX_PATH];
  char cfg_rpath[HS_MAX_PATH];
  size_t tlen = strlen(hs_analysis_dir) + 1;

  struct dirent* entry;
  while ((entry = readdir(dp))) {
    size_t nlen = strlen(entry->d_name);

    // move the Lua to the run directory
    if (nlen <= HS_EXT_LEN) continue;
    if (strcmp(entry->d_name + nlen - HS_EXT_LEN, hs_lua_ext) == 0) {
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

      for (int t = 0; t < plugins->thread_cnt; ++t) {
        // restart any plugins using this Lua code
        hs_analysis_thread* at = &plugins->list[t];
        pthread_mutex_lock(&at->list_lock);
        for (int i = 0; i < at->list_cap; ++i) {
          if (!at->list[i]) continue;

          hs_analysis_plugin* p = at->list[i];
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
        pthread_mutex_unlock(&at->list_lock);
      }
    }
  }
  rewinddir(dp);
}


static int get_thread_id(const char* lpath, const char* rpath, const char* name)
{
  size_t nlen = strlen(name);
  if (nlen <= HS_EXT_LEN) return -2;

  if (strcmp(name + nlen - HS_EXT_LEN, hs_cfg_ext) == 0) {
    int otid = -1, ntid = -2;
    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(lpath, name, &sbc, NULL, HS_SB_TYPE_ANALYSIS)) {
      ntid = sbc.thread;
      hs_free_sandbox_config(&sbc);
    }

    if (hs_load_sandbox_config(rpath, name, &sbc, NULL, HS_SB_TYPE_ANALYSIS)) {
      otid = sbc.thread;
      hs_free_sandbox_config(&sbc);
    } else {
      otid = ntid;
    }

    if (otid != ntid) { // mis-matched cfgs so remove the load .cfg
      char path[HS_MAX_PATH];
      if (!hs_get_fqfn(lpath, name, path, sizeof(path))) {
        hs_log(g_module, 0, "load off path too long");
        exit(EXIT_FAILURE);
      }
      if (unlink(path)) {
        hs_log(g_module, 3, "failed to delete: %s errno: %d", path, errno);
      }
      return -1;
    }
    return otid;
  } else if (strcmp(name + nlen - HS_EXT_LEN, hs_off_ext) == 0) {
    char cfg[HS_MAX_PATH];
    strcpy(cfg, name);
    strcpy(cfg + nlen - HS_EXT_LEN, hs_cfg_ext);

    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(rpath, cfg, &sbc, NULL, HS_SB_TYPE_ANALYSIS)) {
      hs_free_sandbox_config(&sbc);
      return sbc.thread;
    }

    // no config was found so remove the .off flag
    char path[HS_MAX_PATH];
    if (!hs_get_fqfn(lpath, name, path, sizeof(path))) {
      hs_log(g_module, 0, "load off path too long");
      exit(EXIT_FAILURE);
    }
    if (unlink(path)) {
      hs_log(g_module, 3, "failed to delete: %s errno: %d", path, errno);
    }
  }
  return -2;
}


void hs_load_analysis_plugins(hs_analysis_plugins* plugins,
                              const hs_config* cfg,
                              bool dynamic)
{
  char lpath[HS_MAX_PATH];
  char rpath[HS_MAX_PATH];
  if (!hs_get_fqfn(cfg->load_path, hs_analysis_dir, lpath, sizeof(lpath))) {
    hs_log(g_module, 0, "load path too long");
    exit(EXIT_FAILURE);
  }
  if (!hs_get_fqfn(cfg->run_path, hs_analysis_dir, rpath, sizeof(rpath))) {
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
      int tid = get_thread_id(lpath, rpath, entry->d_name);
      if (tid < 0) {
        if (tid == -1) {
          hs_log(g_module, 3, "plugin cannot be restarted on a different "
                 "thread: %s", entry->d_name);
        }
        continue;
      }

      tid %= plugins->thread_cnt;
      int ret = hs_process_load_cfg(lpath, rpath, entry->d_name);

      switch (ret) {
      case 0:
        remove_from_analysis_plugins(&plugins->list[tid], entry->d_name);
        break;
      case 1: // proceed to load
        break;
      default: // ignore
        continue;
      }
    }
    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(rpath, entry->d_name, &sbc, &cfg->apd,
                               HS_SB_TYPE_ANALYSIS)) {
      hs_analysis_plugin* p = create_analysis_plugin(cfg, &sbc);
      if (p) {
        p->sb->mm = hs_create_message_matcher(plugins->mmb,
                                              sbc.message_matcher);
        int ret = hs_init_analysis_sandbox(p->sb, &inject_message);
        if (!p->sb->mm || ret) {
          if (!p->sb->mm) {
            hs_log(g_module, 3, "%s invalid message_matcher: %s",
                   p->sb->name,
                   sbc.message_matcher);
          } else {
            hs_log(g_module, 3, "lsb_init: %s received: %d %s",
                   p->sb->name, ret, lsb_get_error(p->sb->lsb));
          }
          free_analysis_plugin(p);
          free(p);
          p = NULL;
          hs_free_sandbox_config(&sbc);
          continue;
        }
        add_to_analysis_plugins(&sbc, plugins, p);
      }
      hs_free_sandbox_config(&sbc);
    }
  }
  closedir(dp);
}


void hs_start_analysis_threads(hs_analysis_plugins* plugins)
{
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    if (pthread_create(&plugins->threads[i], NULL, input_thread,
                       (void*)&plugins->list[i])) {
      perror("hs_start_analysis_threads pthread_create failed");
      exit(EXIT_FAILURE);
    }
  }
}


hs_sandbox* hs_create_analysis_sandbox(void* parent,
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
