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
#include <luasandbox/util/protobuf.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hs_input.h"
#include "hs_logger.h"
#include "hs_output.h"
#include "hs_util.h"

static const char g_module[] = "analysis_plugins";


static int inject_message(void *parent, const char *pb, size_t pb_len)
{
  static bool backpressure = false;
  static char header[14];
  hs_analysis_plugin *p = parent;

  pthread_mutex_lock(&p->at->plugins->output.lock);
  int len = lsb_pb_output_varint(header + 3, pb_len);
  int tlen = 4 + len + pb_len;
  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  fwrite(header, 4 + len, 1, p->at->plugins->output.fh);
  fwrite(pb, pb_len, 1, p->at->plugins->output.fh);
  p->at->plugins->output.cp.offset += tlen;
  if (p->at->plugins->output.cp.offset >= p->at->plugins->cfg->output_size) {
    ++p->at->plugins->output.cp.id;
    hs_open_output_file(&p->at->plugins->output);
    if (p->at->plugins->cfg->backpressure
        && p->at->plugins->output.cp.id - p->at->plugins->output.min_cp_id
        > p->at->plugins->cfg->backpressure) {
      backpressure = true;
      hs_log(g_module, 4, "applying backpressure");
    }
  }
  if (backpressure) {
    if (p->at->plugins->output.cp.id == p->at->plugins->output.min_cp_id) {
      backpressure = false;
      hs_log(g_module, 4, "releasing backpressure");
    }
  }
  pthread_mutex_unlock(&p->at->plugins->output.lock);

  if (backpressure) {
    usleep(100000); // throttle to 10 messages per second
  }
  return 0;
}


static void destroy_analysis_plugin(hs_analysis_plugin *p)
{
  if (!p) return;
  char *msg = lsb_heka_destroy_sandbox(p->hsb);
  if (msg) {
    hs_log(g_module, 3, "%s lsb_heka_destroy_sandbox failed: %s", p->name, msg);
    free(msg);
  }
  lsb_destroy_message_matcher(p->mm);
  free(p->name);
  free(p);
}


static hs_analysis_plugin*
create_analysis_plugin(lsb_message_match_builder *mmb, const hs_config *cfg,
                       hs_sandbox_config *sbc)
{
  char *state_file = NULL;

  char lua_file[HS_MAX_PATH];
  if (!hs_get_fqfn(sbc->dir, sbc->filename, lua_file, sizeof(lua_file))) {
    hs_log(g_module, 3, "% failed to construct the lua_file path ",
           sbc->cfg_name);
    return NULL;
  }

  hs_analysis_plugin *p = calloc(1, sizeof(hs_analysis_plugin));
  if (!p) {
    hs_log(g_module, 2, "%s hs_analysis_plugin memory allocation failed",
           sbc->cfg_name);
    return NULL;
  }

  p->ticker_interval = sbc->ticker_interval;
  int stagger = p->ticker_interval > 60 ? 60 : p->ticker_interval;
  // distribute when the timer_events will fire
  if (stagger) {
    p->ticker_expires = time(NULL) + rand() % stagger;
  }

  p->mm = lsb_create_message_matcher(mmb, sbc->message_matcher);
  if (!p->mm) {
    hs_log(g_module, 3, "%s invalid message_matcher: %s", sbc->cfg_name,
           sbc->message_matcher);
    destroy_analysis_plugin(p);
    return NULL;
  }

  size_t len = strlen(sbc->cfg_name) + 1;
  p->name = malloc(len);
  if (!p->name) {
    hs_log(g_module, 2, "%s name memory allocation failed", sbc->cfg_name);
    destroy_analysis_plugin(p);
  }
  memcpy(p->name, sbc->cfg_name, len);

  if (sbc->preserve_data) {
    size_t len = strlen(cfg->output_path) + strlen(sbc->cfg_name) + 7;
    state_file = malloc(len);
    if (!state_file) {
      hs_log(g_module, 2, "%s state_file memory allocation failed",
             sbc->cfg_name);
      destroy_analysis_plugin(p);
      return NULL;
    }
    int ret = snprintf(state_file, len, "%s/%s.data", cfg->output_path,
                       sbc->cfg_name);
    if (ret < 0 || ret > (int)len - 1) {
      hs_log(g_module, 3, "%s failed to construct the state_file path",
             sbc->cfg_name);
      destroy_analysis_plugin(p);
      return NULL;
    }
  }
  lsb_output_buffer ob;
  if (lsb_init_output_buffer(&ob, 8 * 1024)) {
    hs_log(g_module, 3, "%s configuration memory allocation failed",
           sbc->cfg_name);
    free(state_file);
    destroy_analysis_plugin(p);
    return NULL;
  }
  if (!hs_get_full_config(&ob, 'a', cfg, sbc)) {
    hs_log(g_module, 3, "%s hs_get_full_config failed", sbc->cfg_name);
    lsb_free_output_buffer(&ob);
    free(state_file);
    destroy_analysis_plugin(p);
    return NULL;
  }
  p->hsb = lsb_heka_create_analysis(p, lua_file, state_file, ob.buf, hs_log,
                                    inject_message);
  lsb_free_output_buffer(&ob);
  free(sbc->cfg_lua);
  sbc->cfg_lua = NULL;
  free(state_file);
  if (!p->hsb) {
    hs_log(g_module, 3, "%s lsb_heka_create_analysis failed", sbc->cfg_name);
    destroy_analysis_plugin(p);
    return NULL;
  }

  return p;
}


static void remove_plugin(hs_analysis_thread *at, int idx)
{
  hs_log(g_module, 6, "analysis thread: %d stopping: %s", at->tid,
         at->list[idx]->name);
  hs_analysis_plugin *p = at->list[idx];
  at->list[idx] = NULL;
  destroy_analysis_plugin(p);
  p = NULL;
  --at->list_cnt;
}


static void remove_from_analysis_plugins(hs_analysis_thread *at,
                                         const char *name)
{
  const size_t tlen = strlen(hs_analysis_dir) + 1;
  pthread_mutex_lock(&at->list_lock);
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;

    char *pos = at->list[i]->name + tlen;
    if (strstr(name, pos) && strlen(pos) == strlen(name) - HS_EXT_LEN) {
      remove_plugin(at, i);
      break;
    }
  }
  pthread_mutex_unlock(&at->list_lock);
}


static void add_plugin(hs_analysis_thread *at, hs_analysis_plugin *p, int idx)
{
  hs_log(g_module, 6, "analysis thread: %d starting: %s", at->tid, p->name);
  at->list[idx] = p;
  ++at->list_cnt;
}


static void add_to_analysis_plugins(const hs_sandbox_config *cfg,
                                    hs_analysis_plugins *plugins,
                                    hs_analysis_plugin *p)
{
  bool added = false;
  int idx = -1;
  int thread = cfg->thread % plugins->cfg->analysis_threads;
  hs_analysis_thread *at = &plugins->list[thread];
  p->at = at;

  pthread_mutex_lock(&at->list_lock);
  // todo shrink it down if there are a lot of empty slots
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) {
      idx = i;
    } else if (strcmp(at->list[i]->name, p->name) == 0) {
      idx = i;
      remove_plugin(at, idx);
      add_plugin(at, p, idx);
      added = true;
      break;
    }
  }
  if (!added && idx != -1) add_plugin(at, p, idx);

  if (idx == -1) {
    ++at->list_cap;
    // todo probably don't want to grow it by 1
    hs_analysis_plugin **tmp = realloc(at->list,
                                       sizeof(hs_analysis_plugin *)
                                       * at->list_cap);
    idx = at->list_cap - 1;
    if (tmp) {
      at->list = tmp;
      add_plugin(at, p, idx);
    } else {
      hs_log(g_module, 0, "plugins realloc failed");
      exit(EXIT_FAILURE);
    }
  }
  pthread_mutex_unlock(&at->list_lock);
}


static void init_analysis_thread(hs_analysis_plugins *plugins, int tid)
{
  hs_analysis_thread *at = &plugins->list[tid];
  at->plugins = plugins;
  at->list = NULL;
  at->msg = NULL;
  if (pthread_mutex_init(&at->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
  if (pthread_mutex_init(&at->cp_lock, NULL)) {
    perror("cp_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
  at->cp.id = 0;
  at->cp.offset = 0;
  at->current_t = 0;
  at->list_cap = 0;
  at->list_cnt = 0;
  at->tid = tid;

  char name[255];
  int n = snprintf(name, sizeof name, "%s%d", hs_analysis_dir, tid);
  if (n < 0 || n >= (int)sizeof name) {
    hs_log(g_module, 0, "name exceeded the buffer length: %s%d",
           hs_analysis_dir, tid);
    exit(EXIT_FAILURE);
  }

  hs_init_input(&at->input, plugins->cfg->max_message_size,
                plugins->cfg->output_path, name);
}


static void free_analysis_thread(hs_analysis_thread *at)
{
  pthread_mutex_destroy(&at->cp_lock);
  pthread_mutex_destroy(&at->list_lock);
  at->plugins = NULL;
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;
    remove_plugin(at, i);
  }
  free(at->list);
  at->list = NULL;
  at->msg = NULL;
  at->cp.id = 0;
  at->cp.offset = 0;
  at->current_t = 0;
  at->list_cap = 0;
  at->list_cnt = 0;
  at->tid = 0;

  hs_free_input(&at->input);
}


static void terminate_sandbox(hs_analysis_thread *at, int i)
{
  hs_log(g_module, 3, "terminated: %s msg: %s", at->list[i]->name,
         lsb_heka_get_error(at->list[i]->hsb));
  remove_plugin(at, i);
}


static void analyze_message(hs_analysis_thread *at)
{
  hs_analysis_plugin *p = NULL;
  bool sample = at->plugins->sample;
  int ret;

  pthread_mutex_lock(&at->list_lock);
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;
    p = at->list[i];

    ret = 0;
    struct timespec ts, ts1;

    if (at->msg->raw.s) { // non idle/empty message
      if (sample) clock_gettime(CLOCK_MONOTONIC, &ts);
      bool matched = lsb_eval_message_matcher(p->mm, at->msg);
      if (sample) {
        clock_gettime(CLOCK_MONOTONIC, &ts1);
        lsb_update_running_stats(&p->mms, lsb_timespec_delta(&ts, &ts1));
      }
      if (matched) {
        ret = lsb_heka_pm_analysis(p->hsb, at->msg, false);
        if (ret < 0) {
          const char *err = lsb_heka_get_error(p->hsb);
          if (strlen(err) > 0) {
            hs_log(g_module, 4, "plugin: %s received: %d %s", p->name, ret,
                   err);
          }
        }
      }
    }

    if (ret <= 0 && p->ticker_interval && at->current_t >= p->ticker_expires) {
      ret = lsb_heka_timer_event(p->hsb, at->current_t, false);
      p->ticker_expires = at->current_t + p->ticker_interval;
    }

    if (ret > 0) terminate_sandbox(at, i);
  }
  pthread_mutex_unlock(&at->list_lock);
}


static void shutdown_timer_event(hs_analysis_thread *at)
{
  pthread_mutex_lock(&at->list_lock);
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;

    if (lsb_heka_timer_event(at->list[i]->hsb, at->current_t, true)) {
      terminate_sandbox(at, i);
    }
  }
  pthread_mutex_unlock(&at->list_lock);
}


static void* input_thread(void *arg)
{
  hs_analysis_thread *at = (hs_analysis_thread *)arg;
  hs_log(g_module, 6, "starting input thread: %d", at->tid);

  lsb_heka_message msg;
  lsb_init_heka_message(&msg, 8);

  hs_config *cfg = at->plugins->cfg;
  hs_lookup_input_checkpoint(&cfg->cp_reader,
                             hs_input_dir,
                             at->input.name,
                             cfg->output_path,
                             &at->input.cp);
  at->cp.id = at->input.cp.id;
  at->cp.offset = at->input.cp.offset;
  size_t discarded_bytes;

  size_t bytes_read = 0;
#ifdef HINDSIGHT_CLI
  bool input_stop = false;
  while (!(at->plugins->stop && input_stop)) {
#else
  while (!at->plugins->stop) {
#endif
    if (at->input.fh) {
      if (lsb_find_heka_message(&msg, &at->input.ib, true, &discarded_bytes,
                                hs_log)) {
        at->msg = &msg;
        at->current_t = time(NULL);
        analyze_message(at);

        // advance the checkpoint
        pthread_mutex_lock(&at->cp_lock);
        at->plugins->sample = false;
        at->cp.id = at->input.cp.id;
        at->cp.offset = at->input.cp.offset -
            (at->input.ib.readpos - at->input.ib.scanpos);
        pthread_mutex_unlock(&at->cp_lock);
      } else {
        bytes_read = hs_read_file(&at->input);
      }

      if (!bytes_read) {
#ifdef HINDSIGHT_CLI
        size_t cid = at->input.cp.id;
#endif
        // see if the next file is there yet
        hs_open_file(&at->input, hs_input_dir, at->input.cp.id + 1);
#ifdef HINDSIGHT_CLI
        if (cid == at->input.cp.id && at->plugins->stop) {
          input_stop = true;
        }
#endif
      }
    } else { // still waiting on the first file
      hs_open_file(&at->input, hs_input_dir, at->input.cp.id);
#ifdef HINDSIGHT_CLI
      if (!at->input.fh && at->plugins->stop) {
        input_stop = true;
      }
#endif
    }

    if (bytes_read || at->msg) {
      at->msg = NULL;
    } else {
      // trigger any pending timer events
      lsb_clear_heka_message(&msg); // create an idle/empty message
      at->msg = &msg;
      at->current_t = time(NULL);
      analyze_message(at);
      at->msg = NULL;
      sleep(1);
    }
  }
  shutdown_timer_event(at);
  lsb_free_heka_message(&msg);
  hs_log(g_module, 6, "exiting input_thread: %d", at->tid);
  pthread_exit(NULL);
}


void hs_init_analysis_plugins(hs_analysis_plugins *plugins,
                              hs_config *cfg,
                              lsb_message_match_builder *mmb)
{
  hs_init_output(&plugins->output, cfg->output_path, hs_analysis_dir);

  plugins->thread_cnt = cfg->analysis_threads;
  plugins->cfg = cfg;
  plugins->stop = false;
  plugins->sample = false;
  plugins->mmb = mmb;

  plugins->list = malloc(sizeof(hs_analysis_thread) * cfg->analysis_threads);
  for (unsigned i = 0; i < cfg->analysis_threads; ++i) {
    init_analysis_thread(plugins, i);
  }
  plugins->threads = malloc(sizeof(pthread_t *) * (cfg->analysis_threads));
}


void hs_wait_analysis_plugins(hs_analysis_plugins *plugins)
{
  void *thread_result;
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    if (pthread_join(plugins->threads[i], &thread_result)) {
      hs_log(g_module, 3, "thread could not be joined");
    }
  }
  free(plugins->threads);
  plugins->threads = NULL;
}


void hs_free_analysis_plugins(hs_analysis_plugins *plugins)
{
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    free_analysis_thread(&plugins->list[i]);
  }
  free(plugins->list);
  plugins->list = NULL;

  hs_free_output(&plugins->output);

  plugins->cfg = NULL;
  plugins->thread_cnt = 0;
  plugins->sample = false;
}


static void process_lua(hs_analysis_plugins *plugins, const char *lpath,
                        const char *rpath, DIR *dp)
{
  char lua_lpath[HS_MAX_PATH];
  char lua_rpath[HS_MAX_PATH];
  char cfg_lpath[HS_MAX_PATH];
  char cfg_rpath[HS_MAX_PATH];
  size_t tlen = strlen(hs_analysis_dir) + 1;

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

      for (int t = 0; t < plugins->thread_cnt; ++t) {
        // restart any plugins using this Lua code
        hs_analysis_thread *at = &plugins->list[t];
        pthread_mutex_lock(&at->list_lock);
        for (int i = 0; i < at->list_cap; ++i) {
          if (!at->list[i]) continue;

          hs_analysis_plugin *p = at->list[i];
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
        pthread_mutex_unlock(&at->list_lock);
      }
    }
  }
  rewinddir(dp);
}


static int get_thread_id(const char *lpath, const char *rpath, const char *name)
{
  if (hs_has_ext(name, hs_cfg_ext)) {
    int otid = -1, ntid = -2;
    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(lpath, name, &sbc, NULL, 'a')) {
      ntid = sbc.thread;
      hs_free_sandbox_config(&sbc);
    }

    if (hs_load_sandbox_config(rpath, name, &sbc, NULL, 'a')) {
      otid = sbc.thread;
      hs_free_sandbox_config(&sbc);
    } else {
      otid = ntid;
    }

    if (otid != ntid) { // mis-matched cfgs so remove the load .cfg
      char path[HS_MAX_PATH];
      if (!hs_get_fqfn(lpath, name, path, sizeof(path))) {
        hs_log(g_module, 0, "load off path too long");
        exit(EXIT_FAILURE);
      }
      if (unlink(path)) {
        hs_log(g_module, 3, "failed to delete: %s errno: %d", path, errno);
      }
      return -1;
    }
    return otid;
  } else if (hs_has_ext(name, hs_off_ext)) {
    char cfg[HS_MAX_PATH];
    strcpy(cfg, name);
    strcpy(cfg + strlen(name) - HS_EXT_LEN, hs_cfg_ext);

    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(rpath, cfg, &sbc, NULL, 'a')) {
      hs_free_sandbox_config(&sbc);
      return sbc.thread;
    }

    // no config was found so remove the .off flag
    char path[HS_MAX_PATH];
    if (!hs_get_fqfn(lpath, name, path, sizeof(path))) {
      hs_log(g_module, 0, "load off path too long");
      exit(EXIT_FAILURE);
    }
    if (unlink(path)) {
      hs_log(g_module, 3, "failed to delete: %s errno: %d", path, errno);
    }
  }
  return -2;
}


void hs_load_analysis_plugins(hs_analysis_plugins *plugins,
                              const hs_config *cfg,
                              bool dynamic)
{
  char lpath[HS_MAX_PATH];
  char rpath[HS_MAX_PATH];
  if (!hs_get_fqfn(cfg->load_path, hs_analysis_dir, lpath, sizeof(lpath))) {
    hs_log(g_module, 0, "load path too long");
    exit(EXIT_FAILURE);
  }
  if (!hs_get_fqfn(cfg->run_path, hs_analysis_dir, rpath, sizeof(rpath))) {
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
      int tid = get_thread_id(lpath, rpath, entry->d_name);
      if (tid < 0) {
        if (tid == -1) {
          hs_log(g_module, 3, "plugin cannot be restarted on a different "
                 "thread: %s", entry->d_name);
        }
        continue;
      }

      tid %= plugins->thread_cnt;
      int ret = hs_process_load_cfg(lpath, rpath, entry->d_name);
      switch (ret) {
      case 0:
        remove_from_analysis_plugins(&plugins->list[tid], entry->d_name);
        break;
      case 1: // proceed to load
        break;
      default: // ignore
        continue;
      }
    }

    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(rpath, entry->d_name, &sbc, &cfg->apd, 'a')) {
      hs_analysis_plugin *p = create_analysis_plugin(plugins->mmb, cfg, &sbc);
      if (p) {
        add_to_analysis_plugins(&sbc, plugins, p);
      } else {
        hs_log(g_module, 3, "%s create_analysis_plugin failed", sbc.cfg_name);
      }
      hs_free_sandbox_config(&sbc);
    }
  }
  closedir(dp);
}


void hs_start_analysis_threads(hs_analysis_plugins *plugins)
{
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    if (pthread_create(&plugins->threads[i], NULL, input_thread,
                       (void *)&plugins->list[i])) {
      perror("hs_start_analysis_threads pthread_create failed");
      exit(EXIT_FAILURE);
    }
  }
}
