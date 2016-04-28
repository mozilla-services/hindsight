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
#include <luasandbox/util/protobuf.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hs_logger.h"
#include "hs_util.h"

static const char g_module[] = "input_plugins";

static void init_ip_checkpoint(hs_ip_checkpoint *cp)
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


static void free_ip_checkpoint(hs_ip_checkpoint *cp)
{
  if (!cp) return;

  cp->type = HS_CP_NONE;
  if (cp->type == HS_CP_STRING) {
    free(cp->value.s);
    cp->value.s = NULL;
  }
  pthread_mutex_destroy(&cp->lock);
}


static bool update_checkpoint(double d, const char *s, hs_ip_checkpoint *cp)
{
  if (!isnan(d)) {
    pthread_mutex_lock(&cp->lock);
    if (cp->type == HS_CP_STRING) {
      free(cp->value.s);
      cp->value.s = NULL;
      cp->len = 0;
      cp->cap = 0;
    }
    cp->type = HS_CP_NUMERIC;
    cp->value.d = d;
    pthread_mutex_unlock(&cp->lock);
  } else if (s) {
    pthread_mutex_lock(&cp->lock);
    if (cp->type == HS_CP_NUMERIC) cp->value.s = NULL;
    cp->type = HS_CP_STRING;
    cp->len = strlen(s);
    cp->len++;
    if (cp->len <= HS_MAX_IP_CHECKPOINT) {
      if (cp->len > cp->cap) {
        free(cp->value.s);
        cp->value.s = malloc(cp->len);
        if (!cp->value.s) {
          cp->len = 0;
          cp->cap = 0;
          pthread_mutex_unlock(&cp->lock);
          hs_log(g_module, 0, "malloc failed");
          return false;
        }
        cp->cap = cp->len;
      }
      memcpy(cp->value.s, s, cp->len);
    } else {
      pthread_mutex_unlock(&cp->lock);
      hs_log(g_module, 3, "chepoint string exceeds %d", HS_MAX_IP_CHECKPOINT);
      return false;
    }
    pthread_mutex_unlock(&cp->lock);
  }
  return true;
}


static int inject_message(void *parent,
                          const char *pb,
                          size_t pb_len,
                          double cp_numeric,
                          const char *cp_string)
{
  static bool backpressure = false;
  static size_t bytes_written = 0;
  static char header[14];

  hs_input_plugin *p = parent;
  if (!update_checkpoint(cp_numeric, cp_string, &p->cp)) return false;

  pthread_mutex_lock(&p->plugins->output.lock);
  int len = lsb_pb_output_varint(header + 3, pb_len);
  int tlen = 4 + len + pb_len;

  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  fwrite(header, 4 + len, 1, p->plugins->output.fh);
  fwrite(pb, pb_len, 1, p->plugins->output.fh);
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


static void destroy_input_plugin(hs_input_plugin *p)
{
  if (!p) return;
  char *msg = lsb_heka_destroy_sandbox(p->hsb);
  if (msg) {
    hs_log(g_module, 3, "%s lsb_heka_destroy_sandbox failed: %s", p->name, msg);
    free(msg);
  }
  free(p->name);
  free_ip_checkpoint(&p->cp);
  sem_destroy(&p->shutdown);
  free(p);
}


static hs_input_plugin*
create_input_plugin(const hs_config *cfg, hs_sandbox_config *sbc)
{
  char *state_file = NULL;
  char lua_file[HS_MAX_PATH];
  if (!hs_get_fqfn(sbc->dir, sbc->filename, lua_file, sizeof(lua_file))) {
    hs_log(g_module, 3, "%s failed to construct the lua_file path",
           sbc->cfg_name);
    return NULL;
  }

  hs_input_plugin *p = calloc(1, sizeof(hs_input_plugin));
  if (!p) {
    hs_log(g_module, 2, "%s hs_input_plugin memory allocation failed",
           sbc->cfg_name);
    return NULL;
  }

  p->ticker_interval = sbc->ticker_interval;
  p->list_index = -1;

  if (sem_init(&p->shutdown, 0, 1)) {
    free(p);
    hs_log(g_module, 3, "%s sem_init failed", sbc->cfg_name);
    return NULL;
  }
  if (sem_wait(&p->shutdown)) {
    destroy_input_plugin(p);
    hs_log(g_module, 3, "%s sem_wait failed", sbc->cfg_name);
    return NULL;
  }

  size_t len = strlen(sbc->cfg_name) + 1;
  p->name = malloc(len);
  if (!p->name) {
    hs_log(g_module, 2, "%s name memory allocation failed", sbc->cfg_name);
    destroy_input_plugin(p);
  }
  memcpy(p->name, sbc->cfg_name, len);

  if (sbc->preserve_data) {
    size_t len = strlen(cfg->output_path) + strlen(sbc->cfg_name) + 7;
    state_file = malloc(len);
    if (!state_file) {
      hs_log(g_module, 2, "%s state_file memory allocation failed",
             sbc->cfg_name);
      destroy_input_plugin(p);
      return NULL;
    }
    int ret = snprintf(state_file, len, "%s/%s.data", cfg->output_path,
                       sbc->cfg_name);
    if (ret < 0 || ret > (int)len - 1) {
      hs_log(g_module, 3, "%s failed to construct the state_file path",
             sbc->cfg_name);
      free(state_file);
      destroy_input_plugin(p);
      return NULL;
    }
  }
  lsb_output_buffer ob;
  if (lsb_init_output_buffer(&ob, 8 * 1024)) {
    hs_log(g_module, 3, "%s configuration memory allocation failed",
           sbc->cfg_name);
    free(state_file);
    destroy_input_plugin(p);
    return NULL;
  }
  if (!hs_get_full_config(&ob, 'i', cfg, sbc)) {
    hs_log(g_module, 3, "%s hs_get_full_config failed", sbc->cfg_name);
    lsb_free_output_buffer(&ob);
    free(state_file);
    destroy_input_plugin(p);
    return NULL;
  }
  p->hsb = lsb_heka_create_input(p, lua_file, state_file, ob.buf, hs_log,
                                 inject_message);
  lsb_free_output_buffer(&ob);
  free(sbc->cfg_lua);
  sbc->cfg_lua = NULL;
  free(state_file);
  if (!p->hsb) {
    destroy_input_plugin(p);
    hs_log(g_module, 3, "%s lsb_heka_create_input failed", sbc->cfg_name);
    return NULL;
  }

  init_ip_checkpoint(&p->cp);
  return p;
}

static void* input_thread(void *arg)
{
  hs_input_plugin *p = (hs_input_plugin *)arg;
  struct timespec ts;
  int ret = 0;
  bool shutdown = false;
  bool profile = (p->ticker_interval > 0);
  double ncp = NAN;
  const char *scp = NULL;

  hs_log(g_module, 6, "starting: %s", p->name);
  while (true) {
    switch (p->cp.type) {
    case HS_CP_STRING:
      ncp = NAN;
      scp = p->cp.value.s;
      break;
    case HS_CP_NUMERIC:
      scp = NULL;
      ncp = p->cp.value.d;
      break;
    case HS_CP_NONE:
      ncp = NAN;
      scp = NULL;
      break;
    }
    ret = lsb_heka_pm_input(p->hsb, ncp, scp, profile);
    if (ret <= 0) {
      if (p->ticker_interval == 0) break; // run once

      if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        hs_log(g_module, 3, "clock_gettime failed: %s", p->name);
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
      }
      ts.tv_sec += p->ticker_interval;
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
  hs_update_checkpoint(&p->plugins->cfg->cp_reader, p->name, &p->cp);

  if (shutdown) {
    hs_log(g_module, 6, "shutting down: %s", p->name);
  } else {
    hs_log(g_module, 6, "detaching: %s received: %d msg: %s",
           p->name, ret, lsb_heka_get_error(p->hsb));
    pthread_mutex_lock(&p->plugins->list_lock);
    hs_input_plugins *plugins = p->plugins;
    plugins->list[p->list_index] = NULL;
    if (pthread_detach(p->thread)) {
      hs_log(g_module, 3, "thread could not be detached");
    }
    destroy_input_plugin(p);
    --plugins->list_cnt;
    pthread_mutex_unlock(&plugins->list_lock);
  }
  pthread_exit(NULL);
}


static void join_thread(hs_input_plugins *plugins, hs_input_plugin *p)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
    hs_log(g_module, 3, "%s clock_gettime failed", p->name);
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
      hs_log(g_module, 2, "%s join timed out, cancelling the thread", p->name);
      pthread_cancel(p->thread);
      if (pthread_join(p->thread, NULL)) {
        hs_log(g_module, 2, "%s cancelled thread could not be joined", p->name);
      }
      lsb_heka_terminate_sandbox(p->hsb, "thread cancelled");
    } else {
      hs_log(g_module, 2, "%s thread could not be joined", p->name);
      lsb_heka_terminate_sandbox(p->hsb, "thread join error");
    }
  }
  destroy_input_plugin(p);
  --plugins->list_cnt;
}


static void remove_plugin(hs_input_plugins *plugins, int idx)
{
  hs_input_plugin *p = plugins->list[idx];
  plugins->list[idx] = NULL;
  sem_post(&p->shutdown);
  lsb_heka_stop_sandbox(p->hsb);
  join_thread(plugins, p);
}


static void remove_from_input_plugins(hs_input_plugins *plugins,
                                      const char *name)
{
  const size_t tlen = strlen(hs_input_dir) + 1;
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;

    char *pos = plugins->list[i]->name + tlen;
    if (strstr(name, pos) && strlen(pos) == strlen(name) - HS_EXT_LEN) {
      remove_plugin(plugins, i);
      break;
    }
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


static void add_plugin(hs_input_plugins *plugins, hs_input_plugin *p, int idx)
{
  plugins->list[idx] = p;
  p->list_index = idx;
  ++plugins->list_cnt;
}


static void add_to_input_plugins(hs_input_plugins *plugins, hs_input_plugin *p)
{
  bool added = false;
  int idx = -1;

  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) {
      idx = i;
    } else if (strcmp(plugins->list[i]->name, p->name) == 0) {
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
    hs_input_plugin **tmp = realloc(plugins->list,
                                    sizeof(hs_input_plugin *)
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

  hs_lookup_checkpoint(&p->plugins->cfg->cp_reader, p->name, &p->cp);

  int ret = pthread_create(&p->thread,
                           NULL,
                           input_thread,
                           (void *)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
}


void hs_init_input_plugins(hs_input_plugins *plugins, hs_config *cfg)
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


void hs_wait_input_plugins(hs_input_plugins *plugins)
{
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;

    hs_input_plugin *p = plugins->list[i];
    plugins->list[i] = NULL;
    join_thread(plugins, p);
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


void hs_free_input_plugins(hs_input_plugins *plugins)
{
  free(plugins->list);
  plugins->list = NULL;

  pthread_mutex_destroy(&plugins->list_lock);
  hs_free_output(&plugins->output);
  plugins->cfg = NULL;
  plugins->list_cnt = 0;
  plugins->list_cap = 0;
}


static void process_lua(hs_input_plugins *plugins, const char *lpath,
                        const char *rpath, DIR *dp)
{
  char lua_lpath[HS_MAX_PATH];
  char lua_rpath[HS_MAX_PATH];
  char cfg_lpath[HS_MAX_PATH];
  char cfg_rpath[HS_MAX_PATH];
  size_t tlen = strlen(hs_input_dir) + 1;

  struct dirent *entry;
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

        hs_input_plugin *p = plugins->list[i];
        if (strcmp(lua_rpath, lsb_heka_get_lua_file(p->hsb)) == 0) {
          int ret = snprintf(cfg_lpath, HS_MAX_PATH, "%s/%s%s", lpath,
                             p->name + tlen, hs_cfg_ext);
          if (ret < 0 || ret > HS_MAX_PATH - 1) {
            hs_log(g_module, 0, "load cfg path too long");
            exit(EXIT_FAILURE);
          }

          ret = snprintf(cfg_rpath, HS_MAX_PATH, "%s/%s%s", rpath,
                         p->name + tlen, hs_cfg_ext);
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


void hs_load_input_plugins(hs_input_plugins *plugins, const hs_config *cfg,
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

  const char *dir = dynamic ? lpath : rpath;
  DIR *dp = opendir(dir);
  if (dp == NULL) {
    hs_log(g_module, 0, "%s: %s", dir, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (dynamic) process_lua(plugins, lpath, rpath, dp);

  struct dirent *entry;
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
    if (hs_load_sandbox_config(rpath, entry->d_name, &sbc, &cfg->ipd, 'i')) {
      hs_input_plugin *p = create_input_plugin(cfg, &sbc);
      if (p) {
        p->plugins = plugins;
        add_to_input_plugins(plugins, p);
      } else {
        hs_log(g_module, 3, "%s create_inputs_plugin failed", sbc.cfg_name);
      }
      hs_free_sandbox_config(&sbc);
    }
  }
  closedir(dp);
}


void hs_stop_input_plugins(hs_input_plugins *plugins)
{
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;

    sem_post(&plugins->list[i]->shutdown);
    lsb_heka_stop_sandbox(plugins->list[i]->hsb);
  }
  pthread_mutex_unlock(&plugins->list_lock);
}
