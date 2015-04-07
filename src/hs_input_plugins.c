/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight configuration loader @file */

#include "hs_input_plugins.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <lauxlib.h>
#include <lua.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hs_logger.h"
#include "hs_sandbox.h"
#include "hs_util.h"
#include "lsb.h"
#include "lsb_output.h"


static const char* input_config = "{"
  "memory_limit = %u,"
  "instruction_limit = %u,"
  "output_limit = %u,"
  "path = [[%s]],"
  "cpath = [[%s]],"
  "remove_entries = {"
  "[''] = {'collectgarbage','coroutine','dofile','load','loadfile'"
  ",'loadstring','newproxy','print'},"
  "os = {'getenv','exit','setlocale'}"
  "}"
  "}";


static void stop_hook(lua_State* L, lua_Debug* ar)
{
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);
  luaL_error(L, "shutting down");
}


static void stop_sandbox(lua_sandbox* lsb)
{
  lua_State* lua = lsb_get_lua(lsb);
  lua_sethook(lua, stop_hook, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}


static int process_message(lua_sandbox* lsb, hs_input_plugin* p)
{
  static const char* func_name = "process_message";
  lua_State* lua = lsb_get_lua(lsb);
  if (!lua) return 1;

  if (lsb_pcall_setup(lsb, func_name)) return 1;

  if (p->cp_string) {
    lua_pushstring(lua, p->cp_string);
  } else {
    lua_pushnumber(lua, p->cp_offset);
  }

  if (lua_pcall(lua, 1, 2, 0) != 0) {
    char err[LSB_ERROR_SIZE];
    int len = snprintf(err, LSB_ERROR_SIZE, "%s() %s", func_name,
                       lua_tostring(lua, -1));
    if (len >= LSB_ERROR_SIZE || len < 0) {
      err[LSB_ERROR_SIZE - 1] = 0;
    }
    lsb_terminate(lsb, err);
    return 1;
  }

  if (!lua_isnumber(lua, 1)) {
    char err[LSB_ERROR_SIZE];
    int len = snprintf(err, LSB_ERROR_SIZE,
                       "%s() must return a numeric error code", func_name);
    if (len >= LSB_ERROR_SIZE || len < 0) {
      err[LSB_ERROR_SIZE - 1] = 0;
    }
    lsb_terminate(lsb, err);
    return 1;
  }

  int status = (int)lua_tointeger(lua, 1);
  switch (lua_type(lua, 2)) {
  case LUA_TNIL:
    lsb_set_error(lsb, NULL);
    break;
  case LUA_TSTRING:
    lsb_set_error(lsb, lua_tostring(lua, 2));
    break;
  default:
    {
      char err[LSB_ERROR_SIZE];
      int len = snprintf(err, LSB_ERROR_SIZE,
                         "%s() must return a nil or string error message",
                         func_name);
      if (len >= LSB_ERROR_SIZE || len < 0) {
        err[LSB_ERROR_SIZE - 1] = 0;
      }
      lsb_terminate(lsb, err);
      return 1;
    }
    break;
  }
  lua_pop(lua, 2);

  lsb_pcall_teardown(lsb);

  return status;
}


static int write_varint(char* buf, unsigned long long i)
{
  int pos = 0;
  if (i == 0) {
    buf[pos] = 0;
    return 1;
  }

  while (i) {
    buf[pos++] = (i & 0x7F) | 0x80;
    i >>= 7;
  }
  buf[pos - 1] &= 0x7F; // end the varint
  return pos;
}


static void write_input_checkpoints(hs_input_plugins* plugins)
{

  hs_output* output = &plugins->output;
  if (fseek(output->cp.fh, 0, SEEK_SET)) {
    hs_log(HS_APP_NAME, 3, "checkpoint fseek() error: %d",
           ferror(output->cp.fh));
    return;
  }

  fprintf(output->cp.fh, "last_output_id = %zu\n", output->cp.id);
  for (int i = 0; i < plugins->plugin_cnt; ++i) {
    hs_input_plugin* p = plugins->list[i];
    if (p->cp_string) {
      fprintf(output->cp.fh, "_G[\"%s\"] = \"", p->sb->filename);
      hs_output_lua_string(output->cp.fh, p->cp_string);
      fwrite("\"\n", 2, 1, output->cp.fh);
    } else if (p->cp_offset) {
      fprintf(output->cp.fh, "_G[\"%s\"] = %lld\n", p->sb->filename,
              p->cp_offset);
    }
  }
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
  hs_input_plugin* p = (hs_input_plugin*)lsb_get_parent(lsb);

  if (lsb_output_protobuf(lsb, 1, 0) != 0) {
    luaL_error(L, "inject_message() could not encode protobuf - %s",
               lsb_get_error(lsb));
  }

  const char* written_data;
  size_t written_data_len = 0;
  written_data = lsb_get_output(lsb, &written_data_len);

  switch (lua_type(L, 2)) {
  case LUA_TNUMBER:
    p->cp_offset = (long long)lua_tointeger(L, 2);
    break;
  case LUA_TSTRING:
    {
      size_t len;
      const char* cp = lua_tolstring(L, 2, &len);
      if (cp) {
        if (len > 255) {
          luaL_error(L, "inject_message() checkpoint exceeds 255 bytes");
        } else if (len > p->cp_capacity) {
          free(p->cp_string);
          p->cp_capacity = len + 8;
          p->cp_string = malloc(p->cp_capacity);
          if (!p->cp_string) {
            p->cp_capacity = 0;
            luaL_error(L, "inject_message() checkpoint malloc failed");
          }
        }
        memcpy(p->cp_string, cp, len + 1);
      }
    }
    break;
  default:
    luaL_error(L, "inject_message() only accepts numeric or string"
               " checkpoints");
    break;
  }

  pthread_mutex_lock(&p->plugins->lock);
  int len = write_varint(header + 3, written_data_len);
  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  fwrite(header, 4 + len, 1, p->plugins->output.fh);
  fwrite(written_data, written_data_len, 1, p->plugins->output.fh);
  bytes_written += 4 + len + written_data_len;
  if (bytes_written > BUFSIZ) {
    fflush(p->plugins->output.fh);
    write_input_checkpoints(p->plugins);
    p->plugins->output.cp.offset += bytes_written;
    bytes_written = 0;
    if (p->plugins->output.cp.offset >= (size_t)p->plugins->cfg->output_size) {
      ++p->plugins->output.cp.id;
      hs_open_output_file(&p->plugins->output, p->plugins->cfg->output_path);
    }
  }
  pthread_mutex_unlock(&p->plugins->lock);
  return 0;
}


static hs_input_plugin* create_input_plugin(const char* file,
                                            const char* cfg_template,
                                            const hs_sandbox_config* cfg,
                                            lua_State* env)
{
  hs_input_plugin* p = calloc(1, sizeof(hs_input_plugin));
  if (!p) return NULL;

  p->sb = hs_create_sandbox(p, file, cfg_template, cfg, env);
  if (!p->sb) {
    free(p);
    hs_log(file, 3, "lsb_create_custom failed");
    return NULL;
  }
  return p;
}


static void free_input_plugin(hs_input_plugin* p)
{
  hs_free_sandbox(p->sb);
  free(p->sb);
  p->sb = NULL;

  p->plugins = NULL;
  free(p->cp_string);
  p->cp_string = NULL;
}


static int init_input_plugin(hs_input_plugin* p)
{
  int ret = lsb_init(p->sb->lsb, p->sb->state);
  if (ret) {
    hs_log(p->sb->filename, 3, "lsb_init() received: %d %s", ret,
           lsb_get_error(p->sb->lsb));
    return ret;
  }
  lsb_add_function(p->sb->lsb, &inject_message, "inject_message");

  return 0;
}


static void* input_thread_function(void* arg)
{
  hs_input_plugin* p = (hs_input_plugin*)arg;
  struct timespec ts;
  int ret = 0;

  hs_log(p->sb->filename, 6, "starting");
  while (true) {
    ret = process_message(p->sb->lsb, p);
    if (ret <= 0) {
      if (ret < 0) {
        const char* err = lsb_get_error(p->sb->lsb);
        if (strlen(err) > 0) {
          hs_log(p->sb->filename, 4, "received: %d %s", ret,
                 lsb_get_error(p->sb->lsb));
        }
      }
      if (p->sb->ticker_interval == 0) break; // run once

      if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        hs_log(p->sb->filename, 3, "clock_gettime failed");
        break;
      }
      ts.tv_sec += p->sb->ticker_interval;
      if (!pthread_mutex_timedlock(p->plugins->shutdown, &ts)) {
        pthread_mutex_unlock(p->plugins->shutdown);
        break; // shutting down
      }
      // poll
    } else {
      const char* err = lsb_get_error(p->sb->lsb);
      if (!strcmp(err, "shutting down")) {
        hs_log(p->sb->filename, 2, "received: %d %s", ret, err);
      }
      break;
    }
  }
  hs_log(p->sb->filename, 6, "exiting received: %d msg: %s", ret,
         lsb_get_error(p->sb->lsb));
  pthread_exit(NULL);
}


static void lookup_checkpoint(lua_State* L, hs_input_plugin* p)
{
  lua_getglobal(L, p->sb->filename);
  switch (lua_type(L, -1)) {
  case LUA_TNUMBER:
    p->cp_offset = lua_tonumber(L, -1);
    break;
  case LUA_TSTRING:
    {
      size_t len;
      const char* cp = lua_tolstring(L, -1, &len);
      p->cp_capacity = len + 8;
      p->cp_string = malloc(p->cp_capacity);
      if (!p->cp_string) {
        hs_log(HS_APP_NAME, 0, "checkpoint malloc failed");
        exit(EXIT_FAILURE);
      }
      memcpy(p->cp_string, cp, len + 1);
    }
    break;
  }
  lua_pop(L, 1);
}


static void add_to_input_plugins(hs_input_plugins* plugins, hs_input_plugin* p)
{
  pthread_mutex_lock(&plugins->lock);
  ++plugins->plugin_cnt;
  hs_input_plugin** htmp = realloc(plugins->list,
                                   sizeof(hs_sandbox*) * plugins->plugin_cnt); // todo probably don't want to grow it by 1
  if (htmp) {
    plugins->list = htmp;
    plugins->list[plugins->plugin_cnt - 1] = p;
  } else {
    hs_log(HS_APP_NAME, 0, "plugins realloc failed");
    exit(EXIT_FAILURE);
  }

  pthread_t* ptmp = realloc(plugins->threads,
                            sizeof(pthread_t) * plugins->plugin_cnt); // todo probably don't want to grow it by 1
  if (ptmp) {
    plugins->threads = ptmp;
  } else {
    hs_log(HS_APP_NAME, 0, "thread realloc failed");
    exit(EXIT_FAILURE);
  }

  lookup_checkpoint(plugins->output.cp.values, p);

  int ret = pthread_create(&(plugins->threads[plugins->plugin_cnt - 1]),
                           NULL,
                           input_thread_function,
                           (void*)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
  pthread_mutex_unlock(&plugins->lock);
}


void hs_init_input_plugins(hs_input_plugins* plugins, hs_config* cfg,
                           pthread_mutex_t* shutdown)
{
  plugins->plugin_cnt = 0;
  plugins->cfg = cfg;
  plugins->shutdown = shutdown;

  if (pthread_mutex_init(plugins->shutdown, NULL)) {
    perror("shutdown pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
  pthread_mutex_lock(plugins->shutdown);

  if (pthread_mutex_init(&plugins->lock, NULL)) {
    perror("lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }

  hs_init_output(&plugins->output, cfg->output_path);

  plugins->list = NULL;
  plugins->threads = NULL;
}


void hs_free_input_plugins(hs_input_plugins* plugins)
{
  void* thread_result;
  for (int i = 0; i < plugins->plugin_cnt; ++i) {
    int ret = pthread_join(plugins->threads[i], &thread_result);
    if (ret) {
      perror("pthread_join failed");
    }
  }
  free(plugins->threads);
  plugins->threads = NULL;

  if (plugins->output.fh) fflush(plugins->output.fh);
  write_input_checkpoints(plugins);

  for (int i = 0; i < plugins->plugin_cnt; ++i) {
    free_input_plugin(plugins->list[i]);
    free(plugins->list[i]);
  }
  free(plugins->list);
  plugins->list = NULL;

  hs_free_output(&plugins->output);

  pthread_mutex_destroy(&plugins->lock);
  pthread_mutex_destroy(plugins->shutdown);
  plugins->cfg = NULL;
  plugins->plugin_cnt = 0;
}


void hs_load_input_plugins(hs_input_plugins* plugins, const hs_config* cfg,
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
    lua_State* L = hs_load_sandbox_config(fqfn, &sbc, &cfg->sbc, HS_MODE_INPUT);
    if (L) {
      if (!hs_get_fqfn(path, sbc.filename, fqfn, sizeof(fqfn))) {
        lua_close(L);
        hs_free_sandbox_config(&sbc);
        continue;
      }
      hs_input_plugin* p = create_input_plugin(fqfn, input_config, &sbc, L);
      if (p) {
        p->plugins = plugins;

        size_t len = strlen(entry->d_name);
        p->sb->filename = malloc(len + 1);
        memcpy(p->sb->filename, entry->d_name, len + 1);

        if (sbc.preserve_data) {
          len = strlen(fqfn);
          p->sb->state = malloc(len + 1);
          memcpy(p->sb->state, fqfn, len + 1);
          memcpy(p->sb->state + len - 3, "dat", 3);
        }

        if (init_input_plugin(p)) {
          free_input_plugin(p);
          free(p);
          p = NULL;
          lua_close(L);
          hs_free_sandbox_config(&sbc);
          continue;
        }
        add_to_input_plugins(plugins, p);
      }
      lua_close(L);
    }
    hs_free_sandbox_config(&sbc);
  }

  closedir(dp);
  return;
}


void hs_stop_input_plugins(hs_input_plugins* plugins)
{
  for (int i = 0; i < plugins->plugin_cnt; ++i) {
    stop_sandbox(plugins->list[i]->sb->lsb);
  }
}
