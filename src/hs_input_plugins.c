/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight configuration loader @file */

#include "hs_input_plugins.h"

#include <assert.h>
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

static const char g_input[] = "input";
static const char g_module[] = "hs_input_plugins";

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
    lua_pushnil(lua);
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

  size_t output_len = 0;
  const char* output = lsb_get_output(lsb, &output_len);

  switch (lua_type(L, 2)) {
  case LUA_TSTRING:
    {
      size_t len;
      const char* cp = lua_tolstring(L, 2, &len);
      ++len;
      if (cp) {
        if (len > HS_MAX_PATH) {
          luaL_error(L, "inject_message() checkpoint exceeds %d bytes",
                     HS_MAX_PATH);
        } else if (len > p->cp_cap) {
          free(p->cp_string);
          // allocate a little extra room for growth
          p->cp_cap = len + 8 <= HS_MAX_PATH ? len + 8 : len;
          p->cp_string = malloc(p->cp_cap);
          if (!p->cp_string) {
            p->cp_cap = 0;
            luaL_error(L, "inject_message() checkpoint malloc failed");
          }
        }
        memcpy(p->cp_string, cp, len);
      }
    }
    break;
  case LUA_TNONE:
  case LUA_TNIL:
    break;
  default:
    return luaL_error(L, "inject_message() only accepts string checkpoints");
  }

  pthread_mutex_lock(&p->plugins->output.lock);
  int len = write_varint(header + 3, output_len);
  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  fwrite(header, 4 + len, 1, p->plugins->output.fh);
  fwrite(output, output_len, 1, p->plugins->output.fh);
  bytes_written += 4 + len + output_len;
  if (bytes_written > BUFSIZ) {
    p->plugins->output.offset += bytes_written;
    bytes_written = 0;
    if (p->plugins->output.offset >= (size_t)p->plugins->cfg->output_size) {
      ++p->plugins->output.id;
      hs_open_output_file(&p->plugins->output);
    }
  }
  pthread_mutex_unlock(&p->plugins->output.lock);
  return 0;
}


static hs_input_plugin* create_input_plugin(const char* file,
                                            const char* cfg_template,
                                            const hs_sandbox_config* cfg,
                                            lua_State* env)
{
  hs_input_plugin* p = calloc(1, sizeof(hs_input_plugin));
  if (!p) return NULL;
  p->list_index = -1;

  if (pthread_mutex_init(&p->cp_lock, NULL)) {
    perror("cp_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }

  p->sb = hs_create_sandbox(p, file, cfg_template, cfg, env);
  if (!p->sb) {
    free(p);
    hs_log(g_module, 3, "lsb_create_custom failed: %s", file);
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
  pthread_mutex_destroy(&p->cp_lock);
}


static int init_input_plugin(hs_input_plugin* p)
{
  lsb_add_function(p->sb->lsb, &inject_message, "inject_message");

  int ret = lsb_init(p->sb->lsb, p->sb->state);
  if (ret) {
    hs_log(g_module, 3, "lsb_init() file: %s received: %d %s",
           p->sb->filename, ret, lsb_get_error(p->sb->lsb));
    return ret;
  }

  return 0;
}


static void* input_thread(void* arg)
{
  hs_input_plugin* p = (hs_input_plugin*)arg;
  struct timespec ts;
  int ret = 0;
  bool run_once = p->sb->ticker_interval == 0;

  hs_log(g_module, 6, "starting: %s", p->sb->filename);
  while (true) {
    ret = process_message(p->sb->lsb, p);
    if (ret <= 0) {
      if (ret < 0) {
        const char* err = lsb_get_error(p->sb->lsb);
        if (strlen(err) > 0) {
          hs_log(g_module, 4, "file: %s received: %d %s", p->sb->filename, ret,
                 lsb_get_error(p->sb->lsb));
        }
      }
      if (run_once) break; // run once

      if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        hs_log(g_module, 3, "clock_gettime failed: %s", p->sb->filename);
        break;
      }
      ts.tv_sec += p->sb->ticker_interval;
      if (!sem_timedwait(p->plugins->shutdown, &ts)) {
        sem_post(p->plugins->shutdown);
        break; // shutting down
      }
      // poll
    } else {
      const char* err = lsb_get_error(p->sb->lsb);
      if (!strcmp(err, "shutting down")) {
        hs_log(g_module, 2, "file: %s received: %d %s", p->sb->filename, ret,
               err);
      }
      break;
    }
  }
  hs_log(g_module, 6, "exiting file: %s received: %d msg: %s", p->sb->filename,
         ret, lsb_get_error(p->sb->lsb));

  // sandbox terminated or running once, don't wait for the join to clean up
  // the most recent checkpoint can be lost under these conditions
  // TODO write the checkpoint back to the Lua table and hold it in memory
  if (!p->sb && run_once) {
    pthread_mutex_lock(&p->plugins->list_lock);
    if (pthread_detach(p->thread)) {
      hs_log(g_module, 3, "thread could not be detached");
    }
    p->plugins->list[p->list_index] = NULL;
    --p->plugins->list_len;
    pthread_mutex_unlock(&p->plugins->list_lock);
    free_input_plugin(p);
    free(p);
  }
  pthread_exit(NULL);
}


static void add_to_input_plugins(hs_input_plugins* plugins, hs_input_plugin* p)
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
    hs_input_plugin** tmp = realloc(plugins->list,
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

  hs_lookup_checkpoint(&p->plugins->cfg->cp_reader,
                       p->sb->filename,
                       &p->cp_string,
                       &p->cp_cap);

  int ret = pthread_create(&p->thread,
                           NULL,
                           input_thread,
                           (void*)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


void hs_init_input_plugins(hs_input_plugins* plugins,
                           hs_config* cfg,
                           sem_t* shutdown)
{
  hs_init_output(&plugins->output, cfg->output_path, g_input);
  plugins->cfg = cfg;
  plugins->shutdown = shutdown;
  plugins->list_len = 0;
  plugins->list = NULL;
  plugins->list_cap = 0;
  if (pthread_mutex_init(&plugins->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


void hs_wait_input_plugins(hs_input_plugins* plugins)
{
  void* thread_result;
  for (int i = 0; i < plugins->list_len; ++i) {
    if (plugins->list[i]) {
      int ret = pthread_join(plugins->list[i]->thread, &thread_result);
      if (ret) {
        perror("pthread_join failed");
      }
    }
  }
}


void hs_free_input_plugins(hs_input_plugins* plugins)
{
  for (int i = 0; i < plugins->list_len; ++i) {
    if (plugins->list[i]) {
      free_input_plugin(plugins->list[i]);
      free(plugins->list[i]);
      plugins->list[i] = NULL;
    }
  }

  free(plugins->list);
  plugins->list = NULL;

  pthread_mutex_destroy(&plugins->list_lock);
  hs_free_output(&plugins->output);
  plugins->cfg = NULL;
  plugins->list_len = 0;
  plugins->list_cap = 0;
}


void hs_load_input_plugins(hs_input_plugins* plugins, const hs_config* cfg,
                           const char* path)
{
  char dir[HS_MAX_PATH];
  if (!hs_get_fqfn(path, g_input, dir, sizeof(dir))) {
    hs_log(HS_APP_NAME, 0, "input load path too long");
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
    lua_State* L = hs_load_sandbox_config(fqfn, &sbc, &cfg->ipd,
                                          HS_SB_TYPE_INPUT);
    if (L) {
      if (!hs_get_fqfn(dir, sbc.filename, fqfn, sizeof(fqfn))) {
        lua_close(L);
        hs_free_sandbox_config(&sbc);
        continue;
      }
      hs_input_plugin* p = create_input_plugin(fqfn, g_sb_template, &sbc, L);
      if (p) {
        p->plugins = plugins;

        size_t len = strlen(entry->d_name) + sizeof(g_input) + 2;
        p->sb->filename = malloc(len);
        snprintf(p->sb->filename, len, "%s/%s", g_input, entry->d_name);

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
}


void hs_stop_input_plugins(hs_input_plugins* plugins)
{
  for (int i = 0; i < plugins->list_len; ++i) {
    stop_sandbox(plugins->list[i]->sb->lsb);
  }
}
