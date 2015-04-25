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
#include <unistd.h>

#include "hs_input.h"
#include "hs_logger.h"
#include "hs_message_matcher.h"
#include "hs_output.h"
#include "hs_sandbox.h"
#include "hs_util.h"

static const char* g_analysis = "analysis";
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
  hs_analysis_plugins* p = (hs_analysis_plugins*)lsb_get_parent(lsb);

  if (!p->matched || !p->msg) {
    lua_pushnil(lua);
    return 1;
  }
  return hs_read_message(lua, p->msg);
}


static int inject_message(lua_State* L)
{
  static char header[14];
  void* luserdata = lua_touserdata(L, lua_upvalueindex(1));
  if (NULL == luserdata) {
    luaL_error(L, "inject_message() invalid lightuserdata");
  }
  lua_sandbox* lsb = (lua_sandbox*)luserdata;
  hs_analysis_plugins* p = (hs_analysis_plugins*)lsb_get_parent(lsb);

  if (lsb_output_protobuf(lsb, 1, 0) != 0) {
    luaL_error(L, "inject_message() could not encode protobuf - %s",
               lsb_get_error(lsb));
  }

  size_t output_len = 0;
  const char* output = lsb_get_output(lsb, &output_len);

  pthread_mutex_lock(&p->output.lock);
  int len = hs_write_varint(header + 3, output_len);
  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  fwrite(header, 4 + len, 1, p->output.fh);
  fwrite(output, output_len, 1, p->output.fh);
  p->output.offset += 4 + len + output_len;
  if (p->output.offset >= (size_t)p->cfg->output_size) {
    ++p->output.id;
    hs_open_output_file(&p->output);
  }
  pthread_mutex_unlock(&p->output.lock);
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
                                    hs_sandbox* sb)
{
  int thread = 0;
  if (plugins->cfg->analysis_threads) {
    thread = cfg->thread % plugins->cfg->analysis_threads;
  }

  hs_analysis_thread* at = &plugins->list[thread];

  pthread_mutex_lock(&at->list_lock);
  // todo shrink it down if there are a lot of empty slots
  if (at->plugin_cnt < at->list_size) { // add to an empty slot
    for (int i = 0; i < at->list_size; ++i) {
      if (!at->list[i]) {
        at->list[i] = sb;
        ++at->plugin_cnt;
      }
    }
  } else { // expand the list
    ++at->list_size;
    // todo probably don't want to grow it by 1
    hs_sandbox** tmp = realloc(at->list,
                               sizeof(hs_sandbox) * at->list_size);
    if (tmp) {
      at->list = tmp;
      at->list[at->list_size - 1] = sb;
      ++at->plugin_cnt;
    } else {
      hs_log(HS_APP_NAME, 0, "plugins realloc failed");
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
  at->list_size = 0;
  at->plugin_cnt = 0;
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


static void free_analysis_thread(hs_analysis_thread* at)
{
  pthread_mutex_destroy(&at->list_lock);
  sem_destroy(&at->start);
  at->plugins = NULL;
  for (int i = 0; i < at->plugin_cnt; ++i) {
    hs_free_sandbox(at->list[i]);
    free(at->list[i]);
  }
  free(at->list);
  at->list = NULL;
  at->list_size = 0;
  at->plugin_cnt = 0;
  at->tid = 0;
}


static int init_sandbox(hs_sandbox* sb)
{
  lsb_add_function(sb->lsb, &read_message, "read_message");
  lsb_add_function(sb->lsb, &inject_message, "inject_message");
  lsb_add_function(sb->lsb, &inject_payload, "inject_payload");

  int ret = lsb_init(sb->lsb, sb->state);
  if (ret) {
    hs_log(sb->filename, 3, "lsb_init() received: %d %s", ret,
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
  hs_log(at->list[i]->filename, 3, "terminated: %s",
         lsb_get_error(at->list[i]->lsb));
  hs_free_sandbox(at->list[i]);
  free(at->list[i]);
  at->list[i] = NULL;
  --at->plugin_cnt;
}


bool analyze_message(hs_analysis_thread* at)
{
  if (at->plugins->msg) {
    hs_sandbox* sb = NULL;
    int ret;

    pthread_mutex_lock(&at->list_lock);
    for (int i = 0; i < at->plugin_cnt; ++i) {
      sb = at->list[i];
      if (!sb) continue;

      at->plugins->matched = false;
      ret = 0;
      if (hs_eval_message_matcher(sb->mm, at->plugins->msg)) {
        at->plugins->matched = true;
        ret = hs_process_message(sb->lsb);
        if (ret == 0) {
          // todo increment process message count
        } else if (ret < 0) {
          // todo increment process message failure count
        }
      }

      if (ret <= 0 && sb->ticker_interval
          && at->plugins->current_t >= sb->next_timer_event) {
        hs_log("analysis_thread", 7, "tid: %d plugin: %d running timer_event",
               at->tid, i); // todo remove
        ret = hs_timer_event(sb->lsb, at->plugins->current_t);
        sb->next_timer_event += sb->ticker_interval;
      }

      if (ret > 0) terminate_sandbox(at, i);
    }
    pthread_mutex_unlock(&at->list_lock);
  } else {
    pthread_mutex_lock(&at->list_lock);
    for (int i = 0; i < at->plugin_cnt; ++i) {
      if (!at->list[i]) continue;

      if (hs_timer_event(at->list[i]->lsb, at->plugins->current_t)) {
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

  hs_log("analysis_thread", 6, "starting thread [%d]", at->tid);
  bool stop = false;

  while (!stop) {
    if (sem_wait(&at->start)) {
      hs_log("analysis_thread", 3, "thread [%d] sem_wait error: %s", at->tid,
             strerror(errno));
      break;
    }

    stop = !analyze_message(at);

    if (sem_post(&at->plugins->finished)) {
      hs_log("analysis_thread", 3, "thread [%d] sem_post error: %s", at->tid,
             strerror(errno));
    }
    sched_yield();
  }
  hs_log("analysis_thread", 6, "exiting thread [%d]", at->tid);
  pthread_exit(NULL);
}


static void* input_thread(void* arg)
{
  hs_log(HS_APP_NAME, 6, "starting hs_analysis_read_input_thread");

  hs_heka_message msg;
  hs_init_heka_message(&msg, 8);

  hs_analysis_plugins* plugins = (hs_analysis_plugins*)arg;
  plugins->msg = NULL;

  hs_config* cfg = plugins->cfg;
  if (strlen(cfg->output_path) > sizeof(plugins->input.path) - 1) {
    fprintf(stderr, "hs_read_input path too long\n");
    exit(EXIT_FAILURE);
  }
  strcpy(plugins->input.path, cfg->output_path);
  hs_lookup_input_checkpoint(&cfg->cp_reader,
                             "analysis_input",
                             cfg->output_path,
                             "input",
                             &plugins->input.id,
                             &plugins->input.offset);

  size_t bytes_read = 0;
  size_t cnt = 0;
  while (!plugins->stop) {
    if (plugins->input.fh) {
      if (hs_find_message(&msg, &plugins->input)) {
        ++cnt; // todo remove
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
      } else {
        bytes_read = hs_read_file(&plugins->input);
      }

      if (!bytes_read) {
        // see if the next file is there yet
        //hs_log(HS_APP_NAME, 7, "analysis count %zu", cnt); // todo remove
        //hs_log(HS_APP_NAME, 7, "analysis looking for the next log"); // todo remove
        hs_open_file(&plugins->input, "input", plugins->input.id + 1);
      }
    } else { // still waiting on the first file
      //hs_log(HS_APP_NAME, 7, "analysis waiting for the input file"); // todo remove
      hs_open_file(&plugins->input, "input", plugins->input.id);
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

  hs_log(HS_APP_NAME, 6, "exiting hs_analysis_read_input_thread");
  pthread_exit(NULL);
}


void hs_init_analysis_plugins(hs_analysis_plugins* plugins,
                              hs_config* cfg,
                              hs_message_match_builder* mmb)
{
  hs_init_output(&plugins->output, cfg->output_path, g_analysis);
  hs_init_input(&plugins->input);

  plugins->thread_cnt = cfg->analysis_threads;
  plugins->cfg = cfg;
  plugins->stop = false;
  plugins->matched = false;
  plugins->msg = NULL;
  plugins->mmb = mmb;

  if (sem_init(&plugins->finished, 0, cfg->analysis_threads)) {
    perror("finished sem_init failed");
    exit(EXIT_FAILURE);
  }

  if (cfg->analysis_threads) {
    plugins->list = malloc(sizeof(hs_analysis_thread) * cfg->analysis_threads);
    for (int i = 0; i < cfg->analysis_threads; ++i) {
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

  sem_destroy(&plugins->finished);
  plugins->cfg = NULL;
  plugins->msg = NULL;
  plugins->thread_cnt = 0;
}


void hs_load_analysis_plugins(hs_analysis_plugins* plugins,
                              const hs_config* cfg,
                              const char* path)
{
  char dir[HS_MAX_PATH];
  if (!hs_get_fqfn(path, g_analysis, dir, sizeof(dir))) {
    hs_log(HS_APP_NAME, 0, "analysis load path too long");
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
      if (!hs_get_fqfn(dir, sbc.filename, fqfn, sizeof(fqfn))) {
        lua_close(L);
        hs_free_sandbox_config(&sbc);
        continue;
      }
      hs_sandbox* sb = hs_create_sandbox(plugins, fqfn, g_sb_template, &sbc,
                                         L);
      if (sb) {
        size_t len = strlen(entry->d_name);
        sb->filename = malloc(len + 1);
        memcpy(sb->filename, entry->d_name, len + 1);

        if (sbc.preserve_data) {
          len = strlen(fqfn);
          sb->state = malloc(len + 1);
          memcpy(sb->state, fqfn, len + 1);
          memcpy(sb->state + len - 3, "dat", 3);
        }

        sb->mm = hs_create_message_matcher(plugins->mmb, sbc.message_matcher);
        if (!sb->mm || init_sandbox(sb)) {
          if (!sb->mm) {
            hs_log(sb->filename, 3, "invalid message_matcher: %s",
                   sbc.message_matcher);
          }
          hs_free_sandbox(sb);
          free(sb);
          sb = NULL;
          lua_close(L);
          hs_free_sandbox_config(&sbc);
          continue;
        }
        add_to_analysis_plugins(&sbc, plugins, sb);
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
