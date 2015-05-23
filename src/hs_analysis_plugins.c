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
#include <semaphore.h>
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
    luaL_error(lua, "read_message() invalid lightuserdata");
  }
  lua_sandbox* lsb = (lua_sandbox*)luserdata;
  hs_analysis_plugin* p = (hs_analysis_plugin*)lsb_get_parent(lsb);

  if (!p->plugins->matched || !p->plugins->msg) {
    lua_pushnil(lua);
    return 1;
  }
  return hs_read_message(lua, p->plugins->msg);
}


static int inject_message(lua_State* L)
{
  static unsigned char header[14];
  void* luserdata = lua_touserdata(L, lua_upvalueindex(1));
  if (NULL == luserdata) {
    luaL_error(L, "inject_message() invalid lightuserdata");
  }
  lua_sandbox* lsb = (lua_sandbox*)luserdata;
  hs_analysis_plugin* p = (hs_analysis_plugin*)lsb_get_parent(lsb);

  if (lsb_output_protobuf(lsb, 1, 0) != 0) {
    luaL_error(L, "inject_message() could not encode protobuf - %s",
               lsb_get_error(lsb));
  }

  size_t output_len = 0;
  const char* output = lsb_get_output(lsb, &output_len);

  pthread_mutex_lock(&p->plugins->output.lock);
  int len = hs_write_varint(header + 3, output_len);
  int tlen = 4 + len + output_len;
  ++p->sb->stats.im_cnt;
  p->sb->stats.im_bytes += tlen;

  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  fwrite(header, 4 + len, 1, p->plugins->output.fh);
  fwrite(output, output_len, 1, p->plugins->output.fh);
  p->plugins->output.offset += tlen;
  if (p->plugins->output.offset >= (size_t)p->plugins->cfg->output_size) {
    ++p->plugins->output.id;
    hs_open_output_file(&p->plugins->output);
  }
  pthread_mutex_unlock(&p->plugins->output.lock);
  return 0;
}


static int inject_payload(lua_State* lua)
{
  static const char* default_type = "txt";
  static const char* func_name = "inject_payload";

  void* luserdata = lua_touserdata(lua, lua_upvalueindex(1));
  if (NULL == luserdata) {
    luaL_error(lua, "%s invalid lightuserdata", func_name);
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
      char err[LSB_ERROR_SIZE];
      size_t len = snprintf(err, LSB_ERROR_SIZE,
                            "%s() payload_type argument must be a string",
                            func_name);
      if (len >= LSB_ERROR_SIZE) {
        err[LSB_ERROR_SIZE - 1] = 0;
      }
      lsb_terminate(lsb, err);
      return 1;
    }
  }

  if (n > 1) {
    if (lua_type(lua, 2) != LUA_TSTRING) {
      char err[LSB_ERROR_SIZE];
      size_t len = snprintf(err, LSB_ERROR_SIZE,
                            "%s() payload_name argument must be a string",
                            func_name);
      if (len >= LSB_ERROR_SIZE) {
        err[LSB_ERROR_SIZE - 1] = 0;
      }
      lsb_terminate(lsb, err);
      return 1;
    }
  }

  // TODO set logger
  // build up a heka message table and then inject it
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
  lua_pushlstring(lua, output, len);
  lua_setfield(lua, -2, "Payload");
  lua_replace(lua, 1);
  inject_message(lua);
  return 0;
}


static void add_to_analysis_plugins(const hs_sandbox_config* cfg,
                                    hs_analysis_plugins* plugins,
                                    hs_analysis_plugin* p)
{
  int thread = 0;
  if (plugins->cfg->analysis_threads) {
    thread = cfg->thread % plugins->cfg->analysis_threads;
  }

  hs_analysis_thread* at = &plugins->list[thread];

  pthread_mutex_lock(&at->list_lock);
  // todo shrink it down if there are a lot of empty slots
  if (at->list_cnt < at->list_cap) { // add to an empty slot
    for (int i = 0; i < at->list_cap; ++i) {
      if (!at->list[i]) {
        at->list[i] = p;
        ++at->list_cnt;
      }
    }
  } else { // expand the list
    ++at->list_cap;
    // todo probably don't want to grow it by 1
    hs_analysis_plugin** tmp = realloc(at->list,
                                       sizeof(hs_analysis_plugin) * at->list_cap);
    if (tmp) {
      at->list = tmp;
      at->list[at->list_cap - 1] = p;
      ++at->list_cnt;
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
  at->list_cap = 0;
  at->list_cnt = 0;
  at->tid = tid;
  if (sem_init(&at->start, 0, 1)) {
    perror("start sem_init failed");
    exit(EXIT_FAILURE);
  }
  if (sem_wait(&at->start)) {
    perror("start sem_wait failed");
    exit(EXIT_FAILURE);
  }
  if (pthread_mutex_init(&at->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


static void free_analysis_plugin(hs_analysis_plugin* p)
{
  if (!p->sb) return;

  hs_free_sandbox(p->sb);
  free(p->sb);
  p->sb = NULL;
  p->plugins = NULL;
}


static void free_analysis_thread(hs_analysis_thread* at)
{
  pthread_mutex_destroy(&at->list_lock);
  sem_destroy(&at->start);
  at->plugins = NULL;
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;
    free_analysis_plugin(at->list[i]);
    free(at->list[i]);
    at->list[i] = NULL;
  }
  free(at->list);
  at->list = NULL;
  at->list_cap = 0;
  at->list_cnt = 0;
  at->tid = 0;
}


static int init_sandbox(hs_sandbox* sb)
{
  lsb_add_function(sb->lsb, &read_message, "read_message");
  lsb_add_function(sb->lsb, &inject_message, "inject_message");
  lsb_add_function(sb->lsb, &inject_payload, "inject_payload");

  int ret = lsb_init(sb->lsb, sb->state);
  if (ret) {
    hs_log(g_module, 3, "lsb_init: %s received: %d %s", sb->filename, ret,
           lsb_get_error(sb->lsb));
    return ret;
  }

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
  hs_log(g_module, 3, "terminated: %s msg: %s", at->list[i]->sb->filename,
         lsb_get_error(at->list[i]->sb->lsb));
  free_analysis_plugin(at->list[i]);
  free(at->list[i]);
  at->list[i] = NULL;
  --at->list_cnt;
}


bool analyze_message(hs_analysis_thread* at)
{
  if (at->plugins->msg) {
    hs_sandbox* sb = NULL;
    bool sample = at->plugins->sample;
    int ret;

    pthread_mutex_lock(&at->list_lock);
    for (int i = 0; i < at->list_cap; ++i) {
      if (!at->list[i]) continue;
      sb = at->list[i]->sb;

      ret = 0;
      struct timespec ts, ts1;
      if (sample) clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
      at->plugins->matched = hs_eval_message_matcher(sb->mm, at->plugins->msg);
      if (sample) {
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);
        hs_update_running_stats(&sb->stats.mm, hs_timespec_delta(&ts, &ts1));
      }
      if (at->plugins->matched) {
        ret = hs_process_message(sb->lsb);
        if (sample) {
          clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
          hs_update_running_stats(&sb->stats.pm, hs_timespec_delta(&ts1, &ts));
        }

        ++sb->stats.pm_cnt;
        if (ret < 0) {
          ++sb->stats.pm_failures;
        }
      }

      if (ret <= 0 && sb->ticker_interval
          && at->plugins->current_t >= sb->next_timer_event) {
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        ret = hs_timer_event(sb->lsb, at->plugins->current_t);
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts1);
        hs_update_running_stats(&sb->stats.te, hs_timespec_delta(&ts, &ts1));
        sb->next_timer_event = at->plugins->current_t + sb->ticker_interval;
      }

      if (ret > 0) terminate_sandbox(at, i);
    }
    pthread_mutex_unlock(&at->list_lock);
  } else {
    pthread_mutex_lock(&at->list_lock);
    for (int i = 0; i < at->list_cap; ++i) {
      if (!at->list[i]) continue;

      if (hs_timer_event(at->list[i]->sb->lsb, at->plugins->current_t)) {
        terminate_sandbox(at, i);
      }
    }
    pthread_mutex_unlock(&at->list_lock);
    return false;
  }
  return true;
}


static void* analysis_thread_function(void* arg)
{
  hs_analysis_thread* at = (hs_analysis_thread*)arg;

  hs_log(g_module, 6, "starting thread [%d]", at->tid);
  bool stop = false;

  while (!stop) {
    if (sem_wait(&at->start)) {
      hs_log(g_module, 3, "thread [%d] sem_wait error: %s", at->tid,
             strerror(errno));
      break;
    }

    stop = !analyze_message(at);

    if (sem_post(&at->plugins->finished)) {
      hs_log(g_module, 3, "thread [%d] sem_post error: %s", at->tid,
             strerror(errno));
    }
    sched_yield();
  }
  hs_log(g_module, 6, "exiting thread [%d]", at->tid);
  pthread_exit(NULL);
}


static hs_analysis_plugin* create_analysis_plugin(const char* file,
                                                  const hs_config* cfg,
                                                  const hs_sandbox_config* sbc,
                                                  lua_State* env)
{
  hs_analysis_plugin* p = calloc(1, sizeof(hs_analysis_plugin));
  if (!p) return NULL;

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

  p->sb = hs_create_sandbox(p, file, lsb_config, sbc, env);
  if (!p->sb) {
    free(p);
    hs_log(g_module, 3, "lsb_create_custom failed: %s", file);
    return NULL;
  }

  return p;
}


static void* input_thread(void* arg)
{
  hs_log(g_module, 6, "starting input thread");

  hs_heka_message msg;
  hs_init_heka_message(&msg, 8);

  hs_analysis_plugins* plugins = (hs_analysis_plugins*)arg;
  plugins->msg = NULL;

  hs_config* cfg = plugins->cfg;
  hs_lookup_input_checkpoint(&cfg->cp_reader,
                             hs_analysis_dir,
                             cfg->output_path,
                             hs_input_dir,
                             &plugins->input.ib.id,
                             &plugins->input.ib.offset);
  plugins->cp_id = plugins->input.ib.id;
  plugins->cp_offset = plugins->input.ib.offset;

  size_t bytes_read = 0;
  while (!plugins->stop) {
    if (plugins->input.fh) {
      if (hs_find_message(&msg, &plugins->input.ib)) {
        plugins->msg = &msg;
        plugins->current_t = time(NULL);

        if (plugins->thread_cnt) {
          for (int i = 0; i < plugins->thread_cnt; ++i) {
            sem_post(&plugins->list[i].start);
          }
          sched_yield();
          // the synchronization creates s a bottleneck of ~110-120K messages
          // per second on my Thinkpad x230.  Threads should be used only
          // when the amount of work done by each thread (>20us) outweighs the
          // synchronization overhead.
          for (int i = 0; i < plugins->thread_cnt; ++i) {
            sem_wait(&plugins->finished);
          }
        } else {
          analyze_message(&plugins->list[0]);
        }
        // advance the checkpoint
        pthread_mutex_lock(&plugins->cp_lock);
        plugins->sample = false;
        plugins->cp_id = plugins->input.ib.id;
        plugins->cp_offset = plugins->input.ib.offset -
          (plugins->input.ib.readpos - plugins->input.ib.scanpos);
        pthread_mutex_unlock(&plugins->cp_lock);
      } else {
        bytes_read = hs_read_file(&plugins->input);
      }

      if (!bytes_read) {
        // see if the next file is there yet
        hs_open_file(&plugins->input, hs_input_dir, plugins->input.ib.id + 1);
      }
    } else { // still waiting on the first file
      hs_open_file(&plugins->input, hs_input_dir, plugins->input.ib.id);
    }

    if (bytes_read || plugins->msg) {
      plugins->msg = NULL;
    } else {
      sleep(1);
    }
  }

  // signal shutdown
  plugins->msg = NULL;
  plugins->current_t = time(NULL);

  if (plugins->thread_cnt) {
    for (int i = 0; i < plugins->thread_cnt; ++i) {
      sem_post(&plugins->list[i].start);
    }
    for (int i = 0; i < plugins->thread_cnt; ++i) {
      sem_wait(&plugins->finished);
    }
  } else {
    analyze_message(&plugins->list[0]);
  }

  hs_free_heka_message(&msg);

  hs_log(g_module, 6, "exiting hs_analysis_read_input_thread");
  pthread_exit(NULL);
}


void hs_init_analysis_plugins(hs_analysis_plugins* plugins,
                              hs_config* cfg,
                              hs_message_match_builder* mmb)
{
  hs_init_output(&plugins->output, cfg->output_path, hs_analysis_dir);
  hs_init_input(&plugins->input, cfg->max_message_size, cfg->output_path);

  plugins->thread_cnt = cfg->analysis_threads;
  plugins->cfg = cfg;
  plugins->stop = false;
  plugins->matched = false;
  plugins->sample = false;
  plugins->msg = NULL;
  plugins->mmb = mmb;
  plugins->cp_id = 0;
  plugins->cp_offset = 0;

  if (sem_init(&plugins->finished, 0, cfg->analysis_threads)) {
    perror("finished sem_init failed");
    exit(EXIT_FAILURE);
  }

  if (pthread_mutex_init(&plugins->cp_lock, NULL)) {
    perror("cp_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }

  if (cfg->analysis_threads) {
    plugins->list = malloc(sizeof(hs_analysis_thread) * cfg->analysis_threads);
    for (unsigned i = 0; i < cfg->analysis_threads; ++i) {
      if (sem_wait(&plugins->finished)) {
        perror("finished sem_wait failed");
        exit(EXIT_FAILURE);
      }
      init_analysis_thread(plugins, i);
    }
  } else {
    plugins->list = malloc(sizeof(hs_analysis_thread));
    init_analysis_thread(plugins, 0);
  }

  // extra thread for the reader is added at the end
  plugins->threads = malloc(sizeof(pthread_t*) * (cfg->analysis_threads + 1));
}


void hs_wait_analysis_plugins(hs_analysis_plugins* plugins)
{
  void* thread_result;
  // <= collects the plugins and the reader thread
  for (int i = 0; i <= plugins->thread_cnt; ++i) {
    int ret = pthread_join(plugins->threads[i], &thread_result);
    if (ret) {
      perror("pthread_join failed");
    }
  }
  free(plugins->threads);
  plugins->threads = NULL;
}


void hs_free_analysis_plugins(hs_analysis_plugins* plugins)
{
  if (plugins->thread_cnt == 0) {
    free_analysis_thread(&plugins->list[0]);
  } else {
    for (int i = 0; i < plugins->thread_cnt; ++i) {
      free_analysis_thread(&plugins->list[i]);
    }
  }
  free(plugins->list);
  plugins->list = NULL;

  hs_free_input(&plugins->input);
  hs_free_output(&plugins->output);

  pthread_mutex_destroy(&plugins->cp_lock);
  sem_destroy(&plugins->finished);
  plugins->cfg = NULL;
  plugins->msg = NULL;
  plugins->thread_cnt = 0;
}


void hs_load_analysis_plugins(hs_analysis_plugins* plugins,
                              const hs_config* cfg,
                              const char* path)
{
  char lsb_config[1024 * 2];
  char dir[HS_MAX_PATH];
  if (!hs_get_fqfn(path, hs_analysis_dir, dir, sizeof(dir))) {
    hs_log(g_module, 0, "load path too long");
    exit(EXIT_FAILURE);
  }

  struct dirent* entry;
  DIR* dp = opendir(dir);
  if (dp == NULL) {
    exit(EXIT_FAILURE);
  }

  char fqfn[HS_MAX_PATH];
  while ((entry = readdir(dp))) {
    if (!hs_get_config_fqfn(dir, entry->d_name, fqfn, sizeof(fqfn))) continue;
    hs_sandbox_config sbc;
    lua_State* L = hs_load_sandbox_config(fqfn, &sbc, &cfg->apd,
                                          HS_SB_TYPE_ANALYSIS);
    if (L) {
      int ret = snprintf(lsb_config, sizeof(lsb_config), g_sb_template,
                         sbc.memory_limit,
                         sbc.instruction_limit,
                         sbc.output_limit,
                         cfg->io_lua_path,
                         cfg->io_lua_cpath);
      if (ret < 0 || ret > (int)sizeof(lsb_config) - 1 ||
          !hs_get_fqfn(dir, sbc.filename, fqfn, sizeof(fqfn))) {
        lua_close(L);
        hs_free_sandbox_config(&sbc);
        continue;
      }
      hs_analysis_plugin* p = create_analysis_plugin(fqfn, cfg, &sbc, L);
      if (p) {
        p->plugins = plugins;

        size_t len = strlen(entry->d_name) + strlen(hs_analysis_dir) + 2;
        p->sb->filename = malloc(len);
        snprintf(p->sb->filename, len, "%s/%s", hs_analysis_dir, entry->d_name);

        if (sbc.preserve_data) {
          len = strlen(fqfn);
          p->sb->state = malloc(len + 1);
          memcpy(p->sb->state, fqfn, len + 1);
          memcpy(p->sb->state + len - 3, "dat", 3);
        }

        p->sb->mm = hs_create_message_matcher(plugins->mmb,
                                              sbc.message_matcher);
        if (!p->sb->mm || init_sandbox(p->sb)) {
          if (!p->sb->mm) {
            hs_log(g_module, 3, "%s invalid message_matcher: %s",
                   p->sb->filename,
                   sbc.message_matcher);
          }
          free_analysis_plugin(p);
          free(p);
          p = NULL;
          lua_close(L);
          hs_free_sandbox_config(&sbc);
          continue;
        }
        add_to_analysis_plugins(&sbc, plugins, p);
      }
      lua_close(L);
    }
    hs_free_sandbox_config(&sbc);
  }
  closedir(dp);
}


void hs_start_analysis_input(hs_analysis_plugins* plugins, pthread_t* t)
{
  pthread_attr_t attr;
  if (pthread_attr_init(&attr)) {
    perror("hs_read_input pthread_attr_init failed");
    exit(EXIT_FAILURE);
  }

  if (pthread_attr_setschedpolicy(&attr, SCHED_FIFO)) {
    perror("hs_start_analysis_input pthread_attr_setschedpolicy failed");
    exit(EXIT_FAILURE);
  }

  struct sched_param sp;
  sp.sched_priority = sched_get_priority_min(SCHED_FIFO);
  if (pthread_attr_setschedparam(&attr, &sp)) {
    perror("hs_start_analysis_threads pthread_attr_setschedparam failed");
    exit(EXIT_FAILURE);
  }

  if (pthread_create(t, &attr, input_thread, (void*)plugins)) {
    perror("hs_read_input pthread_create failed");
    exit(EXIT_FAILURE);
  }
}


void hs_start_analysis_threads(hs_analysis_plugins* plugins)
{
  pthread_attr_t attr;
  if (pthread_attr_init(&attr)) {
    perror("hs_start_analysis_threads pthread_attr_init failed");
    exit(EXIT_FAILURE);
  }
  if (pthread_attr_setschedpolicy(&attr, SCHED_FIFO)) {
    perror("hs_start_analysis_threads pthread_attr_setschedpolicy failed");
    exit(EXIT_FAILURE);
  }

  struct sched_param sp;
  sp.sched_priority = sched_get_priority_min(SCHED_FIFO);
  if (pthread_attr_setschedparam(&attr, &sp)) {
    perror("hs_start_analysis_threads pthread_attr_setschedparam failed");
    exit(EXIT_FAILURE);
  }

  for (int i = 0; i < plugins->thread_cnt; ++i) {
    if (pthread_create(&plugins->threads[i], &attr, analysis_thread_function,
                       (void*)&plugins->list[i])) {
      perror("hs_start_analysis_threads pthread_create failed");
      exit(EXIT_FAILURE);
    }
  }
}
