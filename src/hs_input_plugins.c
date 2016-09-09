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
    if (cp->type == HS_CP_STRING) {
      free(cp->value.s);
      cp->value.s = NULL;
      cp->len = 0;
      cp->cap = 0;
    }
    cp->type = HS_CP_NUMERIC;
    cp->value.d = d;
  } else if (s) {
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
          hs_log(NULL, g_module, 0, "malloc failed");
          return false;
        }
        cp->cap = cp->len;
      }
      memcpy(cp->value.s, s, cp->len);
    } else {
      hs_log(NULL, g_module, 3, "checkpoint string exceeds %d",
             HS_MAX_IP_CHECKPOINT);
      return false;
    }
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
  static char header[14];

  hs_input_plugin *p = parent;
  int rv;
  pthread_mutex_lock(&p->cp.lock);
  rv = !update_checkpoint(cp_numeric, cp_string, &p->cp);
  if (p->sample) {
    p->stats = lsb_heka_get_stats(p->hsb);
    p->sample = false;
  }
  pthread_mutex_unlock(&p->cp.lock);

  if (!pb) { // a NULL message is used as a synchronization point
    if (!sem_trywait(&p->shutdown)) {
      sem_post(&p->shutdown);
      lsb_heka_stop_sandbox_clean(p->hsb);
    }
    return rv;
  }
  if (rv) return rv;

  bool bp;
  pthread_mutex_lock(&p->plugins->output.lock);
  int len = lsb_pb_output_varint(header + 3, pb_len);
  int tlen = 4 + len + pb_len;

  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  if (fwrite(header, 4 + len, 1, p->plugins->output.fh) == 1
      && fwrite(pb, pb_len, 1, p->plugins->output.fh) == 1) {
    p->plugins->output.cp.offset += tlen;
    if (p->plugins->output.cp.offset >= p->plugins->cfg->output_size) {
      ++p->plugins->output.cp.id;
      hs_open_output_file(&p->plugins->output);
      if (p->plugins->cfg->backpressure
          && p->plugins->output.cp.id - p->plugins->output.min_cp_id
          > p->plugins->cfg->backpressure) {
        backpressure = true;
        hs_log(NULL, g_module, 4, "applying backpressure (checkpoint)");
      }
      if (!backpressure && p->plugins->cfg->backpressure_df) {
        unsigned df = hs_disk_free_ob(p->plugins->output.path,
                                      p->plugins->cfg->output_size);
        if (df <= p->plugins->cfg->backpressure_df) {
          backpressure = true;
          hs_log(NULL, g_module, 4, "applying backpressure (disk)");
        }
      }
    }
    if (backpressure) {
      bool release_dfbp = true;
      if (p->plugins->cfg->backpressure_df) {
        unsigned df = hs_disk_free_ob(p->plugins->output.path,
                                      p->plugins->cfg->output_size);
        release_dfbp = (df > p->plugins->cfg->backpressure_df);
      }
      // even if we triggered on disk space continue to backpressure
      // until the queue is caught up too
      if (p->plugins->output.cp.id == p->plugins->output.min_cp_id
          && release_dfbp) {
        backpressure = false;
        hs_log(NULL, g_module, 4, "releasing backpressure");
      }
    }
    rv = 0;
  } else {
    hs_log(NULL, g_module, 0, "inject_message fwrite failed: %s",
           strerror(ferror(p->plugins->output.fh)));
    exit(EXIT_FAILURE);
  }
  bp = backpressure;
  pthread_mutex_unlock(&p->plugins->output.lock);

  if (bp) {
    usleep(100000); // throttle to 10 messages per second
  }
  return rv;
}


static void destroy_input_plugin(hs_input_plugin *p)
{
  if (!p) return;
  char *msg = lsb_heka_destroy_sandbox(p->hsb);
  if (msg) {
    hs_log(NULL, p->name, 3, "lsb_heka_destroy_sandbox failed: %s", msg);
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
  if (hs_get_fqfn(sbc->dir, sbc->filename, lua_file, sizeof(lua_file))) {
    hs_log(NULL, g_module, 3, "%s failed to construct the lua_file path",
           sbc->cfg_name);
    return NULL;
  }

  hs_input_plugin *p = calloc(1, sizeof(hs_input_plugin));
  if (!p) {
    hs_log(NULL, g_module, 2, "%s hs_input_plugin memory allocation failed",
           sbc->cfg_name);
    return NULL;
  }

  p->ticker_interval = sbc->ticker_interval;
  p->list_index = -1;

  if (sem_init(&p->shutdown, 0, 1)) {
    free(p);
    hs_log(NULL, g_module, 3, "%s sem_init failed", sbc->cfg_name);
    return NULL;
  }
  if (sem_wait(&p->shutdown)) {
    destroy_input_plugin(p);
    hs_log(NULL, g_module, 3, "%s sem_wait failed", sbc->cfg_name);
    return NULL;
  }

  size_t len = strlen(sbc->cfg_name) + 1;
  p->name = malloc(len);
  if (!p->name) {
    hs_log(NULL, g_module, 2, "%s name memory allocation failed",
           sbc->cfg_name);
    destroy_input_plugin(p);
  }
  memcpy(p->name, sbc->cfg_name, len);

  if (sbc->preserve_data) {
    size_t len = strlen(cfg->output_path) + strlen(sbc->cfg_name) + 7;
    state_file = malloc(len);
    if (!state_file) {
      hs_log(NULL, g_module, 2, "%s state_file memory allocation failed",
             sbc->cfg_name);
      destroy_input_plugin(p);
      return NULL;
    }
    int ret = snprintf(state_file, len, "%s/%s.data", cfg->output_path,
                       sbc->cfg_name);
    if (ret < 0 || ret > (int)len - 1) {
      hs_log(NULL, g_module, 3, "%s failed to construct the state_file path",
             sbc->cfg_name);
      free(state_file);
      destroy_input_plugin(p);
      return NULL;
    }
  }
  lsb_output_buffer ob;
  if (lsb_init_output_buffer(&ob, 8 * 1024)) {
    hs_log(NULL, g_module, 3, "%s configuration memory allocation failed",
           sbc->cfg_name);
    free(state_file);
    destroy_input_plugin(p);
    return NULL;
  }
  if (!hs_get_full_config(&ob, 'i', cfg, sbc)) {
    hs_log(NULL, g_module, 3, "%s hs_get_full_config failed", sbc->cfg_name);
    lsb_free_output_buffer(&ob);
    free(state_file);
    destroy_input_plugin(p);
    return NULL;
  }
  lsb_logger logger = { .context = NULL, .cb = hs_log };
  p->hsb = lsb_heka_create_input(p, lua_file, state_file, ob.buf, &logger,
                                 inject_message);
  lsb_free_output_buffer(&ob);
  free(sbc->cfg_lua);
  sbc->cfg_lua = NULL;
  free(state_file);
  if (!p->hsb) {
    destroy_input_plugin(p);
    hs_log(NULL, g_module, 3, "%s lsb_heka_create_input failed", sbc->cfg_name);
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

  hs_log(NULL, p->name, 6, "starting");
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
      if (p->ticker_interval == 0) { // run once
        if (!sem_trywait(&p->shutdown)) {
          shutdown = true;
        }
        break; // exit
      } else {  // poll
        if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
          hs_log(NULL, p->name, 3, "clock_gettime failed");
          ts.tv_sec = time(NULL);
          ts.tv_nsec = 0;
        }
        ts.tv_sec += p->ticker_interval;
        if (!sem_timedwait(&p->shutdown, &ts)) {
          shutdown = true;
          break; // shutting down
        }
      }
    } else {
      if (!sem_trywait(&p->shutdown)) {
        shutdown = true;
      }
      break; // exiting due to error
    }
  }

  if (p->orphaned) {
    pthread_exit(NULL);
  }

  hs_input_plugins *plugins = p->plugins;
  // hold the current checkpoint in memory until we shutdown to facilitate
  // resuming where it left off
  hs_update_checkpoint(plugins->cpr, p->name, &p->cp);

  if (shutdown) {
    hs_log(NULL, p->name, 6, "shutting down");
    sem_post(&p->shutdown);
  } else {
    hs_log(NULL, p->name, 6, "detaching received: %d msg: %s", ret,
           lsb_heka_get_error(p->hsb));
    pthread_mutex_lock(&plugins->list_lock);
    plugins->list[p->list_index] = NULL;
    if (pthread_detach(p->thread)) {
      hs_log(NULL, p->name, 3, "thread could not be detached");
    }
    destroy_input_plugin(p);
    --plugins->list_cnt;
    pthread_mutex_unlock(&plugins->list_lock);
  }
  pthread_exit(NULL);
}


static bool join_thread(hs_input_plugins *plugins, hs_input_plugin *p)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
    hs_log(NULL, p->name, 3, "clock_gettime failed");
    ts.tv_sec = time(NULL);
    ts.tv_nsec = 0;
  }

  ts.tv_sec += 2;
  if (pthread_timedjoin_np(p->thread, NULL, &ts)) {
    lsb_heka_stop_sandbox(p->hsb);
    hs_log(NULL, p->name, 4, "sandbox did not respond to a clean stop");
    ts.tv_sec += 2;
    if (pthread_timedjoin_np(p->thread, NULL, &ts)) {
      if (!sem_trywait(&p->shutdown)) {
        p->orphaned = true;
        hs_log(NULL, p->name, 3, "sandbox did not respond to a forced stop "
               "(orphaning)");
        sem_post(&p->shutdown);
        return false;
      } else {
        ts.tv_sec += 1;
        if (pthread_timedjoin_np(p->thread, NULL, &ts)) {
          hs_log(NULL, p->name, 2, "sandbox acknowledged the stop but failed "
                 "to stop (undefined behavior)");
          return false;
        }
      }
    }
  }
  destroy_input_plugin(p);
  --plugins->list_cnt;
  return true;
}


static bool remove_plugin(hs_input_plugins *plugins, int idx)
{
  hs_input_plugin *p = plugins->list[idx];
  sem_post(&p->shutdown);
  lsb_heka_stop_sandbox_clean(p->hsb);
  if (join_thread(plugins, p)) {
    plugins->list[idx] = NULL;
    return true;
  }
  return false;
}


static bool remove_from_input_plugins(hs_input_plugins *plugins,
                                      const char *name)
{
  bool removed = true;
  const size_t tlen = strlen(hs_input_dir) + 1;
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;

    char *pos = plugins->list[i]->name + tlen;
    if (strstr(name, pos) && strlen(pos) == strlen(name) - HS_EXT_LEN) {
      removed = remove_plugin(plugins, i);
      break;
    }
  }
  pthread_mutex_unlock(&plugins->list_lock);
  return removed;
}


static void add_plugin(hs_input_plugins *plugins, hs_input_plugin *p, int idx)
{
  plugins->list[idx] = p;
  p->list_index = idx;
  ++plugins->list_cnt;
}


static bool add_to_input_plugins(hs_input_plugins *plugins, hs_input_plugin *p)
{
  bool added = false;
  int idx = -1;

  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) {
      idx = i;
    } else if (strcmp(plugins->list[i]->name, p->name) == 0) {
      idx = i;
      if (!remove_plugin(plugins, idx)) {
        pthread_mutex_unlock(&plugins->list_lock);
        return false;
      }
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
      hs_log(NULL, g_module, 0, "plugins realloc failed");
      exit(EXIT_FAILURE);
    }
  }
  pthread_mutex_unlock(&plugins->list_lock);
  assert(p->list_index >= 0);

  hs_lookup_checkpoint(p->plugins->cpr, p->name, &p->cp);

  int ret = pthread_create(&p->thread,
                           NULL,
                           input_thread,
                           (void *)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
  return true;
}


void hs_init_input_plugins(hs_input_plugins *plugins,
                           hs_config *cfg,
                           hs_checkpoint_reader *cpr)
{
  hs_init_output(&plugins->output, cfg->output_path, hs_input_dir);
  plugins->cfg = cfg;
  plugins->cpr = cpr;
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
      if (hs_get_fqfn(lpath, entry->d_name, lua_lpath, sizeof(lua_lpath))) {
        hs_log(NULL, g_module, 0, "load lua path too long");
        exit(EXIT_FAILURE);
      }
      if (hs_get_fqfn(rpath, entry->d_name, lua_rpath, sizeof(lua_rpath))) {
        hs_log(NULL, g_module, 0, "run lua path too long");
        exit(EXIT_FAILURE);
      }
      if (rename(lua_lpath, lua_rpath)) {
        hs_log(NULL, g_module, 3, "failed to move: %s to %s errno: %d",
               lua_lpath, lua_rpath, errno);
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
            hs_log(NULL, g_module, 0, "load cfg path too long");
            exit(EXIT_FAILURE);
          }

          ret = snprintf(cfg_rpath, HS_MAX_PATH, "%s/%s%s", rpath,
                         p->name + tlen, hs_cfg_ext);
          if (ret < 0 || ret > HS_MAX_PATH - 1) {
            hs_log(NULL, g_module, 0, "run cfg path too long");
            exit(EXIT_FAILURE);
          }

          // if no new cfg was provided, move the existing cfg to the load
          // directory
          if (!hs_file_exists(cfg_lpath)) {
            if (rename(cfg_rpath, cfg_lpath)) {
              hs_log(NULL, g_module, 3, "failed to move: %s to %s errno: %d",
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


void hs_load_input_plugins(hs_input_plugins *plugins, bool dynamic)
{
  hs_config *cfg = plugins->cfg;
  char lpath[HS_MAX_PATH];
  char rpath[HS_MAX_PATH];
  if (hs_get_fqfn(cfg->load_path, hs_input_dir, lpath, sizeof(lpath))) {
    hs_log(NULL, g_module, 0, "load path too long");
    exit(EXIT_FAILURE);
  }
  if (hs_get_fqfn(cfg->run_path, hs_input_dir, rpath, sizeof(rpath))) {
    hs_log(NULL, g_module, 0, "run path too long");
    exit(EXIT_FAILURE);
  }

  const char *dir = dynamic ? lpath : rpath;
  DIR *dp = opendir(dir);
  if (dp == NULL) {
    hs_log(NULL, g_module, 0, "%s: %s", dir, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (dynamic) process_lua(plugins, lpath, rpath, dp);

  struct dirent *entry;
  while ((entry = readdir(dp))) {
    if (dynamic) {
      int ret = hs_process_load_cfg(lpath, rpath, entry->d_name);
      switch (ret) {
      case 0:
        if (remove_from_input_plugins(plugins, entry->d_name)) {
          break;
        } else {
          hs_log(NULL, g_module, 4, "%s stop request pending", entry->d_name);
          continue;
        }
      case 1: // proceed to load
        break;
      default: // ignore
        continue;
      }
    }
    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(rpath, entry->d_name, &sbc, &cfg->ipd, 'i')) {
      hs_input_plugin *p = create_input_plugin(cfg, &sbc);
      hs_free_sandbox_config(&sbc);
      if (p) {
        p->plugins = plugins;
        if (!add_to_input_plugins(plugins, p)) {
          destroy_input_plugin(p);
          hs_log(NULL, g_module, 3, "%s dynamic load request denied",
                 entry->d_name);
        }
      } else {
        hs_log(NULL, g_module, 3, "%s create_inputs_plugin failed",
               entry->d_name);
      }
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
  }
  pthread_mutex_unlock(&plugins->list_lock);
}
