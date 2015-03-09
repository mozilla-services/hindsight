/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight configuration loader @file */

#include "hindsight_sandbox_loader.h"

#include <ctype.h>
#include <dirent.h>
#include <lauxlib.h>
#include <lua.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static void stop_hook(lua_State* L, lua_Debug* ar)
{
  (void)ar;  /* unused arg. */
  lua_sethook(L, NULL, 0, 0);
  luaL_error(L, "shutting down");
}


static int process_message(lua_sandbox* lsb, hs_plugin* p)
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


static int inject_message(lua_State* L)
{
  static size_t bytes_written = 0;
  static char header[14];
  void* luserdata = lua_touserdata(L, lua_upvalueindex(1));
  if (NULL == luserdata) {
    luaL_error(L, "inject_message() invalid lightuserdata");
  }
  lua_sandbox* lsb = (lua_sandbox*)luserdata;
  hs_plugin* p = (hs_plugin*)lsb_get_parent(lsb);

  if (lsb_output_protobuf(lsb, 1, 0) != 0) {
    luaL_error(L, "inject_message() could not encode protobuf - %s",
               lsb_get_error(p->lsb));
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
  fwrite(header, 4 + len, 1, p->plugins->output_fh);
  fwrite(written_data, written_data_len, 1, p->plugins->output_fh);
  bytes_written += 4 + len + written_data_len;
  if (bytes_written > BUFSIZ) {
    hs_write_checkpoint(p->plugins);
    p->plugins->output_offset += bytes_written;
    bytes_written = 0;
    if (p->plugins->output_offset >= (size_t)p->plugins->hs_cfg->output_size) {
      ++p->plugins->output_id;
      hs_open_output_file(p->plugins);
    }
  }
  pthread_mutex_unlock(&p->plugins->lock);
  return 0;
}


static int read_config(lua_State* L)
{
  luaL_checkstring(L, 1);
  luaL_argcheck(L, lua_gettop(L) == 1, 0, "too many arguments");
  lua_getfield(L, LUA_ENVIRONINDEX, lua_tostring(L, 1));
  return 1;
}


static int expand_path(const char* path, int n, const char* fmt, char* opath)
{
  // not an exhaustive check, just making sure the Lua markers are accounted for
  for (int i = 0; (path[i]); ++i) {
    if (path[i] == '\'' || path[i] == ';' || path[i] == '?'
        || !isprint(path[i])) {
      return 1;
    }
  }
  int result = snprintf(opath, n, fmt, path);
  if (result < 0 || result > n - 1) {
    return 1;
  }
  return 0;
}


static hs_plugin* create_plugin(const char* file,
                                const char* cfg_template,
                                const sandbox_config* cfg)
{

  char config[1024 * 2];
  char lpath[260] = { 0 };
  char cpath[260] = { 0 };

  if (cfg->module_path) {
#if defined(_WIN32)
    if (expand_path(cfg->module_path, sizeof(lpath), "%s\\?.lua", lpath)) {
      return NULL;
    }
    if (expand_path(cfg->module_path, sizeof(cpath), "%s\\?.dll", cpath)) {
      return NULL;
    }
#else
    if (expand_path(cfg->module_path, sizeof(lpath), "%s/?.lua", lpath)) {
      return NULL;
    }
    if (expand_path(cfg->module_path, sizeof(cpath), "%s/?.so", cpath)) {
      return NULL;
    }
#endif
  }

  int ret = snprintf(config, sizeof(config), cfg_template,
                     cfg->memory_limit,
                     cfg->instruction_limit,
                     cfg->output_limit,
                     lpath,
                     cpath);

  if (ret < 0 || ret > (int)sizeof(config) - 1) {
    return NULL;
  }

  hs_plugin* p = calloc(1, sizeof(hs_plugin));
  if (!p) return NULL;

  p->lsb = lsb_create_custom(p, file, config);
  if (!p->lsb) {
    free(p);
    fprintf(stderr, "lsb_create_custom failed\n%s\n", config);
    return NULL;
  }

  return p;
}


static int init_plugin(hs_plugin* p)
{
  int ret = lsb_init(p->lsb, p->state);
  if (ret) {
    fprintf(stderr, "lsb_init() received: %d %s\n", ret, lsb_get_error(p->lsb));
    return ret;
  }
  lsb_add_function(p->lsb, &inject_message, "inject_message");

  return 0;
}


static void* input_thread_function(void* arg)
{
  hs_plugin* p = (hs_plugin*)arg;
  fprintf(stderr, "starting %s\n", p->filename);
  int ret = process_message(p->lsb, p);
  fprintf(stderr, "exiting %s received: %d %s\n", p->filename, ret,
          lsb_get_error(p->lsb));
  pthread_exit(NULL);
}


static void populate_environment(lua_State* cfg,
                                 lua_State* sb,
                                 sandbox_config* sbc)
{
  lua_pushcclosure(sb, read_config, 0);
  lua_newtable(sb);

  // load the user provided configuration variables
  lua_pushnil(cfg);
  while (lua_next(cfg, LUA_GLOBALSINDEX) != 0) {
    int kt = lua_type(cfg, -2);
    int vt = lua_type(cfg, -1);
    switch (kt) {
    case LUA_TSTRING:
      switch (vt) {
      case LUA_TSTRING:
        {
          size_t len;
          const char* tmp = lua_tolstring(cfg, -1, &len);
          if (tmp) {
            lua_pushlstring(sb, tmp, len);
            lua_setfield(sb, -2, lua_tostring(cfg, -2));
          }
        }
        break;
      case LUA_TNUMBER:
        lua_pushnumber(sb, lua_tonumber(cfg, -1));
        lua_setfield(sb, -2, lua_tostring(cfg, -2));
        break;
      case LUA_TBOOLEAN:
        lua_pushboolean(sb, lua_tonumber(cfg, -1));
        lua_setfield(sb, -2, lua_tostring(cfg, -2));
        break;
      default:
        fprintf(stderr, "skipping config value type: %s\n",
                lua_typename(cfg, vt));
        break;
      }
      break;
    default:
      fprintf(stderr, "skipping config key type: %s\n",
              lua_typename(cfg, kt));
      break;
    }
    lua_pop(cfg, 1);
  }

  // load the known configuration variables
  lua_pushinteger(sb, sbc->output_limit);
  lua_setfield(sb, -2, "output_limit");
  lua_pushinteger(sb, sbc->memory_limit);
  lua_setfield(sb, -2, "memory_limit");
  lua_pushinteger(sb, sbc->instruction_limit);
  lua_setfield(sb, -2, "instruction_limit");
  lua_pushboolean(sb, sbc->preserve_data);
  lua_setfield(sb, -2, "preserve_data");
  lua_pushstring(sb, sbc->module_path);
  lua_setfield(sb, -2, "module_path");
  lua_pushstring(sb, sbc->filename);
  lua_setfield(sb, -2, "filename");

  // add the table as the environment for the read_config function
  lua_setfenv(sb, -2);
  lua_setglobal(sb, "read_config");
}


static bool get_config_fqfn(const char* path,
                            const char* name,
                            char* fqfn,
                            size_t fqfn_len)
{
  static const size_t ext_size = 4;
  size_t len = strlen(name);

  if (len <= ext_size) return false;
  if (strcmp(".cfg", name + len - ext_size)) return false;

  int ret = snprintf(fqfn, fqfn_len, "%s/%s", path, name);
  if (ret < 0 || ret > (int)fqfn_len - 1) {
    fprintf(stderr, "%s: fully qualiifed path is greater than %zu\n", name,
            fqfn_len);
    return false;
  }

  return true;
}


static const char* get_config_template(const hindsight_config* cfg)
{
  const char* cfg_template;

  switch (cfg->mode) {
  case HS_MODE_INPUT:
  case HS_MODE_OUTPUT:
    cfg_template = input_config;
    break;
  default:
    cfg_template = analysis_config;
    break;
  }

  return cfg_template;
}


static void lookup_checkpoint(lua_State* L, hs_plugin* p)
{
  lua_getglobal(L, p->filename);
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
        fprintf(stderr, "checkpoint malloc failed\n");
        exit(EXIT_FAILURE);
      }
      memcpy(p->cp_string, cp, len + 1);
    }
    break;
  }
  lua_pop(L, 1);
}


static void add_to_plugins(hs_plugins* plugins, hs_plugin* p)
{
  pthread_mutex_lock(&plugins->lock);
  ++plugins->cnt;
  hs_plugin** htmp = realloc(plugins->list,
                             sizeof(hs_plugin*) * plugins->cnt); // todo probably don't want to grow it by 1
  if (htmp) {
    plugins->list = htmp;
    plugins->list[plugins->cnt - 1] = p;
  } else {
    fprintf(stderr, "plugins realloc failed\n");
    exit(EXIT_FAILURE);
  }

  pthread_t* ptmp = realloc(plugins->threads,
                            sizeof(pthread_t) * plugins->cnt); // todo probably don't want to grow it by 1
  if (ptmp) {
    plugins->threads = ptmp;
  } else {
    fprintf(stderr, "thread realloc failed\n");
    exit(EXIT_FAILURE);
  }

  lookup_checkpoint(plugins->cp_values, p);

  // if input/output start thread
  int ret = pthread_create(&(plugins->threads[plugins->cnt - 1]),
                           NULL,
                           input_thread_function,
                           (void*)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
  // if analysis add to correct thread pool
  pthread_mutex_unlock(&plugins->lock);
}


void hs_stop_sandbox(lua_sandbox* lsb)
{
  lua_State* lua = lsb_get_lua(lsb);
  lua_sethook(lua, stop_hook, LUA_MASKCALL | LUA_MASKRET | LUA_MASKCOUNT, 1);
}


void hs_load_sandboxes(const char* path, const hindsight_config* cfg,
                       hs_plugins* plugins)
{
  struct dirent* entry;
  DIR* dp;

  dp = opendir(path);
  if (dp == NULL) {
    fprintf(stderr, "%s: ", path);
    perror(NULL);
    exit(EXIT_FAILURE);
  }

  char fqfn[260];
  while ((entry = readdir(dp))) {
    if (!get_config_fqfn(path, entry->d_name, fqfn, sizeof(fqfn))) continue;
    sandbox_config sbc;
    lua_State* L = hs_load_sandbox_config(fqfn, &sbc, &cfg->sbc);
    if (L) {
      if (!hs_get_fqfn(path, sbc.filename, fqfn, sizeof(fqfn))) {
        lua_close(L);
        hs_free_sandbox_config(&sbc);
        continue;
      }
      hs_plugin* p = create_plugin(fqfn, get_config_template(cfg), &sbc);
      if (p) {
        populate_environment(L, lsb_get_lua(p->lsb), &sbc);

        p->plugins = plugins;

        size_t len = strlen(entry->d_name);
        p->filename = malloc(len + 1);
        memcpy(p->filename, entry->d_name, len + 1);

        if (sbc.preserve_data) {
          len = strlen(fqfn);
          p->state = malloc(len + 1);
          memcpy(p->state, fqfn, len + 1);
          memcpy(p->state + len - 3, "dat", 3);
        }

        if (init_plugin(p)) {
          hs_free_plugin(p);
          free(p);
          p = NULL;
          lua_close(L);
          hs_free_sandbox_config(&sbc);
          continue;
        }
        add_to_plugins(plugins, p);
      }
      lua_close(L);
    }
    hs_free_sandbox_config(&sbc);
  }

  closedir(dp);
  return;
}

