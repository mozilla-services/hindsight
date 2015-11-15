/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight configuration loader @file */
#define _GNU_SOURCE

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
  "os = {'getenv','exit','setlocale'},"
  "string = {'dump'},"
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
  static bool backpressure = false;
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
  switch (lua_type(L, 1)) {
  case LUA_TUSERDATA:
    {
      void* ud = luaL_checkudata(L, 1, mozsvc_heka_stream_reader);
      luaL_argcheck(L, ud != NULL, 1, "invalid userdata type");
      heka_stream_reader* hsr = (heka_stream_reader*)ud;
      if (hsr->msg.msg) {
        output_len = hsr->msg.msg_len;
        output = (const char*)hsr->msg.msg;
      } else {
        return luaL_error(L, "attempted to inject a nil message");
      }
    }
    break;
  case LUA_TSTRING:
    {
      hs_heka_message m;
      hs_init_heka_message(&m, 8);
      output = lua_tolstring(L, 1, &output_len);
      bool ok = hs_decode_heka_message(&m, (const unsigned char*)output,
                                       output_len);
      hs_free_heka_message(&m);
      if (!ok) {
        return luaL_error(L, "attempted to inject a invalid protobuf string");
      }
    }
    break;
  default:
    if (lsb_output_protobuf(lsb, 1, 0) != 0) {
      return luaL_error(L, "inject_message() could not encode protobuf - %s",
                        lsb_get_error(lsb));
    }
    output_len = 0;
    output = lsb_get_output(lsb, &output_len);
    break;
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
    p->plugins->output.cp.offset += bytes_written;
    bytes_written = 0;
    if (p->plugins->output.cp.offset >= p->plugins->cfg->output_size) {
      ++p->plugins->output.cp.id;
      hs_open_output_file(&p->plugins->output);
      if (p->plugins->cfg->backpressure
          && p->plugins->output.cp.id - p->plugins->output.min_cp_id
          > p->plugins->cfg->backpressure) {
        backpressure = true;
        hs_log(g_module, 4, "applying backpressure");
      }
    }
  }
  if (backpressure) {
    if (p->plugins->output.cp.id == p->plugins->output.min_cp_id) {
      backpressure = false;
      hs_log(g_module, 4, "releasing backpressure");
    }
  }
  pthread_mutex_unlock(&p->plugins->output.lock);
  if (backpressure) {
    usleep(100000); // throttle to 10 messages per second
  }
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


static hs_input_plugin* create_input_plugin(const hs_config* cfg,
                                            hs_sandbox_config* sbc)
{
  hs_input_plugin* p = calloc(1, sizeof(hs_input_plugin));
  if (!p) return NULL;
  p->list_index = -1;

  if (sem_init(&p->shutdown, 0, 1)) {
    free(p);
    hs_log(g_module, 3, "sem_init failed: %s/%s", sbc->dir,
           sbc->filename);
    return NULL;
  }
  if (sem_wait(&p->shutdown)) {
    free(p);
    hs_log(g_module, 3, "sem_wait failed: %s/%s", sbc->dir,
           sbc->filename);
    return NULL;
  }

  p->sb = hs_create_input_sandbox(p, cfg, sbc);
  if (!p->sb) {
    free(p);
    hs_log(g_module, 3, "lsb_create_custom failed: %s/%s", sbc->dir,
           sbc->filename);
    return NULL;
  }

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
  sem_destroy(&p->shutdown);
}


int hs_init_input_sandbox(hs_sandbox* sb, lua_CFunction im_fp)
{
  if (!im_fp) return -1;

  lsb_add_function(sb->lsb, im_fp, "inject_message");
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
  bool shutdown = false;

  hs_log(g_module, 6, "starting: %s", p->sb->name);
  while (true) {
    ret = process_message(p->sb->lsb, p);
    if (ret <= 0) {
      if (ret < 0) {
        const char* err = lsb_get_error(p->sb->lsb);
        if (strlen(err) > 0) {
          hs_log(g_module, 4, "file: %s received: %d %s", p->sb->name, ret,
                 lsb_get_error(p->sb->lsb));
        }
      }

      if (p->sb->ticker_interval == 0) break; // run once

      if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        hs_log(g_module, 3, "clock_gettime failed: %s", p->sb->name);
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
      }
      ts.tv_sec += p->sb->ticker_interval;
      if (!sem_timedwait(&p->shutdown, &ts)) {
        sem_post(&p->shutdown);
        shutdown = true;
        break; // shutting down
      }
      // poll
    } else {
      if (!sem_trywait(&p->shutdown)) {
        sem_post(&p->shutdown);
        shutdown = true;
        break; // shutting down
      }
      break; // exiting due to error
    }
  }

  // hold the current checkpoint in memory until we shutdown
  // to facilitate resuming where it left off
  hs_update_checkpoint(&p->plugins->cfg->cp_reader, p->sb->name, &p->cp);

  if (shutdown) {
    hs_log(g_module, 6, "shutting down: %s", p->sb->name);
  } else {
    hs_log(g_module, 6, "detaching: %s received: %d msg: %s",
           p->sb->name, ret, lsb_get_error(p->sb->lsb));
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
  }
  pthread_exit(NULL);
}


static void join_thread(hs_input_plugins* plugins, hs_input_plugin* p)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
    hs_log(g_module, 3, "%s clock_gettime failed", p->sb->name);
    ts.tv_sec = time(NULL);
    ts.tv_nsec = 0;
  }
  ts.tv_sec += 2;
  int ret = pthread_timedjoin_np(p->thread, NULL, &ts);
  if (ret) {
    if (ret == ETIMEDOUT) {
      // The plugin is blocked on a C function call so we are in a known state
      // but there is no guarantee cancelling the thread and interacting with
      // the Lua state at that point is safe.  The lsb_terminate limits the
      // interaction but lua_close will still be called. At this point I would
      // rather risk the core and have to restart the process instead of leaking
      // the memory or hanging on a plugin stop and having to kill the process
      // anyway (todo: more investigation)
      hs_log(g_module, 2, "%s join timed out, cancelling the thread",
             p->sb->name);
      pthread_cancel(p->thread);
      if (pthread_join(p->thread, NULL)) {
        hs_log(g_module, 2, "%s cancelled thread could not be joined",
               p->sb->name);
      }
      lsb_terminate(p->sb->lsb, "thread cancelled");
    } else {
      hs_log(g_module, 2, "%s thread could not be joined", p->sb->name);
      lsb_terminate(p->sb->lsb, "thread join error");
    }
  }
  free_input_plugin(p);
  free(p);
  --plugins->list_cnt;
}


static void remove_plugin(hs_input_plugins* plugins, int idx)
{
  hs_input_plugin* p = plugins->list[idx];
  plugins->list[idx] = NULL;
  sem_post(&p->shutdown);
  stop_sandbox(p->sb->lsb);
  join_thread(plugins, p);
}


static void remove_from_input_plugins(hs_input_plugins* plugins,
                                      const char* name)
{
  const size_t tlen = strlen(hs_input_dir) + 1;
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


static void add_plugin(hs_input_plugins* plugins, hs_input_plugin* p, int idx)
{
  plugins->list[idx] = p;
  p->list_index = idx;
  ++plugins->list_cnt;
}


static void add_to_input_plugins(hs_input_plugins* plugins, hs_input_plugin* p)
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
    hs_input_plugin** tmp = realloc(plugins->list,
                                    sizeof(hs_input_plugin*)
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

  hs_lookup_checkpoint(&p->plugins->cfg->cp_reader,
                       p->sb->name,
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


void hs_init_input_plugins(hs_input_plugins* plugins, hs_config* cfg)
{
  hs_init_output(&plugins->output, cfg->output_path, hs_input_dir);
  plugins->cfg = cfg;
  plugins->list_cnt = 0;
  plugins->list = NULL;
  plugins->list_cap = 0;
  if (pthread_mutex_init(&plugins->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


void hs_wait_input_plugins(hs_input_plugins* plugins)
{
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;

    hs_input_plugin* p = plugins->list[i];
    plugins->list[i] = NULL;
    join_thread(plugins, p);
  }
  pthread_mutex_unlock(&plugins->list_lock);
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
}


static void process_lua(hs_input_plugins* plugins, const char* lpath,
                        const char* rpath, DIR* dp)
{
  char lua_lpath[HS_MAX_PATH];
  char lua_rpath[HS_MAX_PATH];
  char cfg_lpath[HS_MAX_PATH];
  char cfg_rpath[HS_MAX_PATH];
  size_t tlen = strlen(hs_input_dir) + 1;

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

        hs_input_plugin* p = plugins->list[i];
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


void hs_load_input_plugins(hs_input_plugins* plugins, const hs_config* cfg,
                           bool dynamic)
{
  char lpath[HS_MAX_PATH];
  char rpath[HS_MAX_PATH];
  if (!hs_get_fqfn(cfg->load_path, hs_input_dir, lpath, sizeof(lpath))) {
    hs_log(g_module, 0, "load path too long");
    exit(EXIT_FAILURE);
  }
  if (!hs_get_fqfn(cfg->run_path, hs_input_dir, rpath, sizeof(rpath))) {
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
        remove_from_input_plugins(plugins, entry->d_name);
        break;
      case 1: // proceed to load
        break;
      default: // ignore
        continue;
      }
    }
    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(rpath, entry->d_name, &sbc, &cfg->ipd,
                               HS_SB_TYPE_INPUT)) {
      hs_input_plugin* p = create_input_plugin(cfg, &sbc);
      if (p) {
        p->plugins = plugins;

        int ret = hs_init_input_sandbox(p->sb, &inject_message);
        if (ret) {
          hs_log(g_module, 3, "lsb_init() file: %s received: %d %s",
                 p->sb->name,
                 ret,
                 lsb_get_error(p->sb->lsb));
          free_input_plugin(p);
          free(p);
          p = NULL;
          hs_free_sandbox_config(&sbc);
          continue;
        }
        add_to_input_plugins(plugins, p);
      }
      hs_free_sandbox_config(&sbc);
    }
  }
  closedir(dp);
}


void hs_stop_input_plugins(hs_input_plugins* plugins)
{
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;

    sem_post(&plugins->list[i]->shutdown);
    stop_sandbox(plugins->list[i]->sb->lsb);
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


hs_sandbox* hs_create_input_sandbox(void* parent,
                                    const hs_config* cfg,
                                    hs_sandbox_config* sbc)
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

  hs_sandbox* sb = hs_create_sandbox(parent, lsb_config, sbc);
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
