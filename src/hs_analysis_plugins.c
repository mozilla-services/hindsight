/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight configuration loader @file */

#include "hs_analysis_plugins.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <lauxlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>

#include "hs_input.h"
#include "hs_logger.h"
#include "hs_output.h"
#include "hs_sandbox.h"
#include "hs_util.h"
#include "lsb.h"
#include "lsb_output.h"


static const char* analysis_config = "{"
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


static void write_analysis_checkpoints(hs_analysis_plugins* plugins)
{

  hs_output* output = &plugins->output;
  if (fseek(output->cp.fh, 0, SEEK_SET)) {
    hs_log(HS_APP_NAME, 3, "checkpoint fseek() error: %d",
           ferror(output->cp.fh));
    return;
  }

  fprintf(output->cp.fh, "last_output_id = %zu\n", output->cp.id);
  fprintf(output->cp.fh, "input_id = %zu\n", plugins->input.id);
  long pos = 0;
  if (plugins->input.fh) pos = ftell(plugins->input.fh);
  fprintf(output->cp.fh, "input_offset = %zu\n", pos);
  fflush(output->cp.fh);
}


static int inject_message(lua_State* L)
{
  static size_t bytes_written = 0;
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

  const char* written_data;
  size_t written_data_len = 0;
  written_data = lsb_get_output(lsb, &written_data_len);

  pthread_mutex_lock(&p->lock);
  int len = hs_write_varint(header + 3, written_data_len);
  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  fwrite(header, 4 + len, 1, p->output.fh);
  fwrite(written_data, written_data_len, 1, p->output.fh);
  bytes_written += 4 + len + written_data_len;
  if (bytes_written > BUFSIZ) {
    fflush(p->output.fh);
    write_analysis_checkpoints(p);
    p->output.cp.offset += bytes_written;
    bytes_written = 0;
    if (p->output.cp.offset >= (size_t)p->cfg->output_size) {
      ++p->output.cp.id;
      hs_open_output_file(&p->output, p->cfg->output_path);
    }
  }
  pthread_mutex_unlock(&p->lock);
  return 0;
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
    if (!at->plugins->msg) {
      stop = true;
    } else {
      //hs_log("analysis_thread", 6, "thread [%d] ... process message", at->tid); // todo remove
      // loop through plugins
      // run matcher
      // if match process message
      // end loop
    }
    if (sem_post(&at->plugins->finished)) {
      hs_log("analysis_thread", 3, "thread [%d] sem_post error: %s", at->tid,
             strerror(errno));
    }
  }
  hs_log("analysis_thread", 6, "exiting thread [%d]", at->tid);
  pthread_exit(NULL);
}


static void add_to_analysis_plugins(const hs_sandbox_config* cfg,
                                    hs_analysis_plugins* plugins,
                                    hs_sandbox* p)
{
  int thread = cfg->thread % plugins->cfg->threads;

  hs_analysis_thread* at = &plugins->list[thread];

  pthread_mutex_lock(&at->plugins->lock);
  ++at->plugin_cnt;
  hs_sandbox** tmp = realloc(at->list,
                             sizeof(hs_sandbox) * at->plugin_cnt); // todo probably don't want to grow it by 1
  if (tmp) {
    at->list = tmp;
    at->list[at->plugin_cnt - 1] = p;
  } else {
    hs_log(HS_APP_NAME, 0, "plugins realloc failed");
    exit(EXIT_FAILURE);
  }
  pthread_mutex_unlock(&at->plugins->lock);
}


static void init_analysis_thread(hs_analysis_plugins* plugins, int tid)
{
  hs_analysis_thread* at = &plugins->list[tid];
  at->plugins = plugins;
  at->list = NULL;
  at->plugin_cnt = 0;
  at->tid = tid;
  if (sem_init(&at->start, 0, 1)) {
    perror("start sem_init failed");
    exit(EXIT_FAILURE);
  }
  sem_wait(&at->start);
}


static void free_analysis_thread(hs_analysis_thread* at)
{
  sem_destroy(&at->start);
  at->plugins = NULL;
  for (int i = 0; i < at->plugin_cnt; ++i) {
    hs_free_sandbox(at->list[i]);
    free(at->list[i]);
  }
  free(at->list);
  at->list = NULL;
  at->plugin_cnt = 0;
  at->tid = 0;
}


static int init_sandbox(hs_sandbox* sb)
{
  int ret = lsb_init(sb->lsb, sb->state);
  if (ret) {
    hs_log(sb->filename, 3, "lsb_init() received: %d %s", ret,
           lsb_get_error(sb->lsb));
    return ret;
  }
  lsb_add_function(sb->lsb, &inject_message, "inject_message");

  return 0;
}


void hs_init_analysis_plugins(hs_analysis_plugins* plugins, hs_config* cfg,
                              pthread_mutex_t* shutdown)
{
  hs_init_output(&plugins->output, cfg->output_path);
  hs_init_input(&plugins->input);
  hs_init_message_match_builder(&plugins->mmb, cfg->sbc.module_path);

  plugins->thread_cnt = cfg->threads;
  plugins->cfg = cfg;
  plugins->shutdown = shutdown;
  plugins->stop = false;
  plugins->msg = NULL;

  if (pthread_mutex_init(plugins->shutdown, NULL)) {
    perror("shutdown pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
  pthread_mutex_lock(plugins->shutdown);

  if (pthread_mutex_init(&plugins->lock, NULL)) {
    perror("lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }

  if (sem_init(&plugins->finished, 0, cfg->threads)) {
    perror("finished sem_init failed");
    exit(EXIT_FAILURE);
  }

  plugins->list = malloc(sizeof(hs_analysis_thread) * cfg->threads);
  for (int i = 0; i < cfg->threads; ++i) {
    sem_wait(&plugins->finished);
    init_analysis_thread(plugins, i);
  }

  plugins->threads = malloc(sizeof(pthread_t*) * (cfg->threads + 1));
}


void hs_free_analysis_plugins(hs_analysis_plugins* plugins)
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

  if (plugins->output.fh) fflush(plugins->output.fh);
  write_analysis_checkpoints(plugins);

  for (int i = 0; i < plugins->thread_cnt; ++i) {
    free_analysis_thread(&plugins->list[i]);
  }
  free(plugins->list);
  plugins->list = NULL;

  hs_free_message_match_builder(&plugins->mmb);
  hs_free_input(&plugins->input);
  hs_free_output(&plugins->output);

  pthread_mutex_destroy(&plugins->lock);
  pthread_mutex_destroy(plugins->shutdown);
  sem_destroy(&plugins->finished);
  plugins->cfg = NULL;
  plugins->thread_cnt = 0;
  plugins->msg = NULL;
}


void hs_load_analysis_plugins(hs_analysis_plugins* plugins,
                              const hs_config* cfg,
                              const char* path)
{
  struct dirent* entry;
  DIR* dp = opendir(path);
  if (dp == NULL) {
    hs_log(HS_APP_NAME, 0, "%s: %s", path, strerror(errno));
    exit(EXIT_FAILURE);
  }

  char fqfn[260];
  while ((entry = readdir(dp))) {
    if (!hs_get_config_fqfn(path, entry->d_name, fqfn, sizeof(fqfn))) continue;
    hs_sandbox_config sbc;
    lua_State* L = hs_load_sandbox_config(fqfn, &sbc, &cfg->sbc,
                                          HS_MODE_ANALYSIS);
    if (L) {
      if (!hs_get_fqfn(path, sbc.filename, fqfn, sizeof(fqfn))) {
        lua_close(L);
        hs_free_sandbox_config(&sbc);
        continue;
      }
      hs_sandbox* sb = hs_create_sandbox(plugins, fqfn, analysis_config, &sbc,
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

        sb->mm = hs_create_message_matcher(&plugins->mmb, sbc.message_matcher);
        if (!sb->mm || init_sandbox(sb)) {
          if (!sb->mm) {
            hs_log(entry->d_name, 3, "invalid message_matcher: %s",
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
  return;
}


void hs_start_analysis_input(hs_analysis_plugins* plugins, pthread_t* t)
{
  int ret = pthread_create(t,
                           NULL,
                           hs_read_input_thread,
                           (void*)plugins);
  if (ret) {
    perror("hs_read_input pthread_create failed");
    exit(EXIT_FAILURE);
  }
}


void hs_start_analysis_threads(hs_analysis_plugins* plugins)
{
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    int ret = pthread_create(&plugins->threads[i],
                             NULL,
                             analysis_thread_function,
                             (void*)&plugins->list[i]);
    if (ret) {
      perror("hs_read_input pthread_create failed");
      exit(EXIT_FAILURE);
    }
  }
}
