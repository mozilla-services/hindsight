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
#include <luasandbox.h>
#include <luasandbox/lauxlib.h>
#include <luasandbox/lua.h>
#include <luasandbox_output.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hs_logger.h"
#include "hs_sandbox.h"
#include "hs_heka_stream_reader.h"
#include "hs_util.h"

static const char g_module[] = "input_plugins";

static const char* g_sb_template = "{"
  "memory_limit = %u,"
  "instruction_limit = %u,"
  "output_limit = %u,"
  "path = [[%s]],"
  "cpath = [[%s]],"
  "max_message_size = %u,"
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

  switch (p->cp.type) {
  case HS_CP_STRING:
    lua_pushlstring(lua, p->cp.value.s, p->cp.len);
    break;
  case HS_CP_NUMERIC:
    lua_pushnumber(lua, p->cp.value.d);
    break;
  default:
    lua_pushnil(lua);
    break;
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


static int inject_message(lua_State* L)
{
  static size_t bytes_written = 0;
  static unsigned char header[14];
  void* luserdata = lua_touserdata(L, lua_upvalueindex(1));
  if (NULL == luserdata) {
    return luaL_error(L, "inject_message() invalid lightuserdata");
  }
  lua_sandbox* lsb = (lua_sandbox*)luserdata;
  hs_input_plugin* p = (hs_input_plugin*)lsb_get_parent(lsb);

  size_t output_len;
  const char* output;
  if (lua_type(L, 1) == LUA_TUSERDATA) {
    void* ud = luaL_checkudata(L, 1, mozsvc_heka_stream_reader);
    luaL_argcheck(L, ud != NULL, 1, "invalid userdata type");
    heka_stream_reader* hsr = (heka_stream_reader*)ud;
    if (hsr->msg.msg) {
      output_len = hsr->msg.msg_len;
      output = (const char*)hsr->msg.msg;
    } else {
      return luaL_error(L, "attempted to inject a nil message");
    }
  } else {
    if (lsb_output_protobuf(lsb, 1, 0) != 0) {
      return luaL_error(L, "inject_message() could not encode protobuf - %s",
                        lsb_get_error(lsb));
    }
    output_len = 0;
    output = lsb_get_output(lsb, &output_len);
  }
  if (!hs_load_checkpoint(L, 2, &p->cp)) {
    return luaL_error(L, "inject_message() only accepts numeric"
                      " or string checkpoints < %d", HS_MAX_IP_CHECKPOINT);
  }

  p->sb->stats.cur_memory = lsb_usage(lsb, LSB_UT_MEMORY, LSB_US_CURRENT);
  p->sb->stats.max_memory = lsb_usage(lsb, LSB_UT_MEMORY, LSB_US_MAXIMUM);
  p->sb->stats.max_output = lsb_usage(lsb, LSB_UT_OUTPUT, LSB_US_MAXIMUM);
  p->sb->stats.max_instructions = lsb_usage(lsb, LSB_UT_INSTRUCTION,
                                            LSB_US_MAXIMUM);

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
  bytes_written += tlen;
  if (bytes_written > BUFSIZ) {
    p->plugins->output.offset += bytes_written;
    bytes_written = 0;
    if (p->plugins->output.offset >= p->plugins->cfg->output_size) {
      ++p->plugins->output.id;
      hs_open_output_file(&p->plugins->output);
    }
  }
  pthread_mutex_unlock(&p->plugins->output.lock);
  return 0;
}


static void init_ip_checkpoint(hs_ip_checkpoint* cp)
{
  if (pthread_mutex_init(&cp->lock, NULL)) {
    perror("cp lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
  cp->type = HS_CP_NONE;
  cp->len = 0;
  cp->cap = 0;
  cp->value.d = 0;
}


static void free_ip_checkpoint(hs_ip_checkpoint* cp)
{
  cp->type = HS_CP_NONE;
  if (cp->type == HS_CP_STRING) {
    free(cp->value.s);
    cp->value.s = NULL;
  }
  pthread_mutex_destroy(&cp->lock);
}


static hs_input_plugin* create_input_plugin(const char* file,
                                            const hs_config* cfg,
                                            const hs_sandbox_config* sbc,
                                            lua_State* env)
{
  hs_input_plugin* p = calloc(1, sizeof(hs_input_plugin));
  if (!p) return NULL;
  p->list_index = -1;

  p->sb = hs_create_input_sandbox(p, file, cfg, sbc, env);
  if (!p->sb) {
    free(p);
    hs_log(g_module, 3, "lsb_create_custom failed: %s", file);
    return NULL;
  }

  // preload the Heka stream reader module
  lua_State* L = lsb_get_lua(p->sb->lsb);
  luaL_findtable(L, LUA_REGISTRYINDEX, "_PRELOADED", 1);
  lua_pushstring(L, "heka_stream_reader");
  lua_pushcfunction(L, luaopen_heka_stream_reader);
  lua_rawset(L, -3);
  lua_pop(L, 1); // remove the preloaded table

  init_ip_checkpoint(&p->cp);
  return p;
}


static void free_input_plugin(hs_input_plugin* p)
{
  hs_free_sandbox(p->sb);
  free(p->sb);
  p->sb = NULL;
  p->plugins = NULL;
  free_ip_checkpoint(&p->cp);
}


int hs_init_input_plugin(hs_sandbox* sb)
{
  if (!sb->im_fp) return -1;

  lsb_add_function(sb->lsb, sb->im_fp, "inject_message");
  // inject_payload is intentionally excluded from input plugins
  // you can construct whatever you need with inject_message
  int ret = lsb_init(sb->lsb, sb->state);
  if (ret) return ret;

  lua_State* lua = lsb_get_lua(sb->lsb);
  // remove output function
  lua_pushnil(lua);
  lua_setglobal(lua, "output");

  return 0;
}


static void* input_thread(void* arg)
{
  hs_input_plugin* p = (hs_input_plugin*)arg;
  struct timespec ts;
  int ret = 0;

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

      if (p->sb->ticker_interval == 0) break; // run once

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
      break;
    }
  }

  hs_log(g_module, 6, "detaching: %s received: %d msg: %s",
         p->sb->filename, ret, lsb_get_error(p->sb->lsb));

  // hold the current checkpoint in memory until we shutdown
  // to facilitate resuming where it left off
  hs_update_checkpoint(&p->plugins->cfg->cp_reader, p->sb->filename, &p->cp);

  pthread_mutex_lock(&p->plugins->list_lock);
  hs_input_plugins* plugins = p->plugins;
  plugins->list[p->list_index] = NULL;
  if (pthread_detach(p->thread)) {
    hs_log(g_module, 3, "thread could not be detached");
  }
  free_input_plugin(p);
  free(p);
  --plugins->list_cnt;
  pthread_mutex_unlock(&plugins->list_lock);
  pthread_exit(NULL);
}


static void add_to_input_plugins(hs_input_plugins* plugins, hs_input_plugin* p)
{
  pthread_mutex_lock(&plugins->list_lock);
  if (plugins->list_cnt < plugins->list_cap) {
    for (int i = 0; i < plugins->list_cap; ++i) {
      if (!plugins->list[i]) {
        plugins->list[i] = p;
        p->list_index = i;
        ++plugins->list_cnt;
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
      ++plugins->list_cnt;
    } else {
      hs_log(g_module, 0, "plugins realloc failed");
      exit(EXIT_FAILURE);
    }
  }
  pthread_mutex_unlock(&plugins->list_lock);
  assert(p->list_index >= 0);

  hs_lookup_checkpoint(&p->plugins->cfg->cp_reader,
                       p->sb->filename,
                       &p->cp);

  int ret = pthread_create(&p->thread,
                           NULL,
                           input_thread,
                           (void*)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
}


void hs_init_input_plugins(hs_input_plugins* plugins,
                           hs_config* cfg,
                           sem_t* shutdown)
{
  hs_init_output(&plugins->output, cfg->output_path, hs_input_dir);
  plugins->cfg = cfg;
  plugins->shutdown = shutdown;
  plugins->list_cnt = 0;
  plugins->list = NULL;
  plugins->list_cap = 0;
  plugins->stop = false;
  if (pthread_mutex_init(&plugins->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


void hs_wait_input_plugins(hs_input_plugins* plugins)
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


void hs_free_input_plugins(hs_input_plugins* plugins)
{
  free(plugins->list);
  plugins->list = NULL;

  pthread_mutex_destroy(&plugins->list_lock);
  hs_free_output(&plugins->output);
  plugins->cfg = NULL;
  plugins->list_cnt = 0;
  plugins->list_cap = 0;
  plugins->stop = false;
}


void hs_load_input_plugins(hs_input_plugins* plugins, const hs_config* cfg,
                           const char* path)
{
  char dir[HS_MAX_PATH];
  if (!hs_get_fqfn(path, hs_input_dir, dir, sizeof(dir))) {
    hs_log(g_module, 0, "input load path too long");
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
      hs_input_plugin* p = create_input_plugin(fqfn, cfg, &sbc, L);
      if (p) {
        p->plugins = plugins;
        p->sb->im_fp = &inject_message;

        size_t len = strlen(entry->d_name) + strlen(hs_input_dir) + 2;
        p->sb->filename = malloc(len);
        snprintf(p->sb->filename, len, "%s/%s", hs_input_dir, entry->d_name);

        if (sbc.preserve_data) {
          len = strlen(fqfn);
          p->sb->state = malloc(len + 1);
          memcpy(p->sb->state, fqfn, len + 1);
          memcpy(p->sb->state + len - 3, "dat", 3);
        }

        int ret = hs_init_input_plugin(p->sb);
        if (ret) {
          hs_log(g_module, 3, "lsb_init() file: %s received: %d %s",
                 p->sb->filename,
                 ret,
                 lsb_get_error(p->sb->lsb));
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
  plugins->stop = true;
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (plugins->list[i]) {
      stop_sandbox(plugins->list[i]->sb->lsb);
    }
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


hs_sandbox* hs_create_input_sandbox(void* parent,
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
                     cfg->io_lua_cpath,
                     cfg->max_message_size);

  if (ret < 0 || ret > (int)sizeof(lsb_config) - 1) {
    return NULL;
  }

  hs_sandbox* sb = hs_create_sandbox(parent, file, lsb_config, sbc, env);
  if (!sb) return NULL;

  // preload the Heka stream reader module
  lua_State* L = lsb_get_lua(sb->lsb);
  luaL_findtable(L, LUA_REGISTRYINDEX, "_PRELOADED", 1);
  lua_pushstring(L, "heka_stream_reader");
  lua_pushcfunction(L, luaopen_heka_stream_reader);
  lua_rawset(L, -3);
  lua_pop(L, 1); // remove the preloaded table
  return sb;
}
