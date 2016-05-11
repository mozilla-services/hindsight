/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight output plugin loader @file */

#include "hs_output_plugins.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <luasandbox.h>
#include <luasandbox/lauxlib.h>
#include <luasandbox_output.h>
#include <luasandbox/heka/sandbox.h>
#include <luasandbox/util/protobuf.h>
#include <luasandbox/util/running_stats.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hs_input.h"
#include "hs_logger.h"
#include "hs_output.h"
#include "hs_util.h"

static const char g_module[] = "output_plugins";
static const char g_output[] = "output";

static void destroy_output_plugin(hs_output_plugin *p)
{
  if (!p) return;
  hs_free_input(&p->analysis);
  hs_free_input(&p->input);
  char *msg = lsb_heka_destroy_sandbox(p->hsb);
  if (msg) {
    hs_log(NULL, p->name, 3, "lsb_heka_destroy_sandbox failed: %s", msg);
    free(msg);
  }
  lsb_destroy_message_matcher(p->mm);
  free(p->name);
  free(p->async_cp);
  pthread_mutex_destroy(&p->cp_lock);
  free(p);
}


static void update_checkpoint(hs_output_plugin *p)
{
  pthread_mutex_lock(&p->cp_lock);
  p->cp.input.id = p->cur.input.id;
  p->cp.input.offset = p->cur.input.offset;
  p->cp.analysis.id = p->cur.analysis.id;
  p->cp.analysis.offset = p->cur.analysis.offset;
  pthread_mutex_unlock(&p->cp_lock);

}


static int update_checkpoint_callback(void *parent, void *sequence_id)
{
  hs_output_plugin *p = parent;

  if (sequence_id && p->async_cp) {
    int i = (unsigned long long)sequence_id % p->async_len;
    pthread_mutex_lock(&p->cp_lock);
    if (p->async_cp[i].input.id >= p->cp.input.id
        && p->async_cp[i].input.offset > p->cp.input.offset) {
      p->cp.input.id = p->async_cp[i].input.id;
      p->cp.input.offset = p->async_cp[i].input.offset;
    }
    if (p->async_cp[i].analysis.id >= p->cp.analysis.id
        && p->async_cp[i].analysis.offset > p->cp.analysis.offset) {
      p->cp.analysis.id = p->async_cp[i].analysis.id;
      p->cp.analysis.offset = p->async_cp[i].analysis.offset;
    }
    pthread_mutex_unlock(&p->cp_lock);
  } else if (p->batching) {
    update_checkpoint(p);
    p->batching = false;
  }
  return 0;
}


static hs_output_plugin*
create_output_plugin(const hs_config *cfg, hs_sandbox_config *sbc)
{
  char *state_file = NULL;
  char lua_file[HS_MAX_PATH];
  if (!hs_get_fqfn(sbc->dir, sbc->filename, lua_file, sizeof(lua_file))) {
    hs_log(NULL, g_module, 3, "%s failed to construct the lua_file path",
           sbc->cfg_name);
    return NULL;
  }

  hs_output_plugin *p = calloc(1, sizeof(hs_output_plugin));
  if (!p) {
    hs_log(NULL, g_module, 2, "%s hs_output_plugin memory allocation failed",
           sbc->cfg_name);
    return NULL;
  }

  if (pthread_mutex_init(&p->cp_lock, NULL)) {
    free(p);
    hs_log(NULL, g_module, 3, "%s pthread_mutex_init failed", sbc->cfg_name);
    return NULL;
  }

  p->list_index = -1;
  p->sequence_id = 1;
  p->ticker_interval = sbc->ticker_interval;
  int stagger = p->ticker_interval > 60 ? 60 : p->ticker_interval;
  // distribute when the timer_events will fire
  if (stagger) {
    p->ticker_expires = time(NULL) + rand() % stagger;
  }

  if (sbc->async_buffer_size > 0) {
    p->async_len = sbc->async_buffer_size;
    p->async_cp = calloc(p->async_len, sizeof(hs_checkpoint_pair));
    if (!p->async_cp) {
      destroy_output_plugin(p);
      hs_log(NULL, g_module, 2, "%s async buffer memory allocation failed",
             sbc->cfg_name);
      return NULL;
    }
  }

  p->mm = lsb_create_message_matcher(sbc->message_matcher);
  if (!p->mm) {
    hs_log(NULL, g_module, 3, "%s invalid message_matcher: %s", sbc->cfg_name,
           sbc->message_matcher);
    destroy_output_plugin(p);
    return NULL;
  }

  size_t len = strlen(sbc->cfg_name) + 1;
  p->name = malloc(len);
  if (!p->name) {
    hs_log(NULL, g_module, 2, "%s name memory allocation failed",
           sbc->cfg_name);
    destroy_output_plugin(p);
  }
  memcpy(p->name, sbc->cfg_name, len);

  if (sbc->preserve_data) {
    size_t len = strlen(cfg->output_path) + strlen(sbc->cfg_name) + 7;
    state_file = malloc(len);
    if (!state_file) {
      hs_log(NULL, g_module, 2, "%s state_file memory allocation failed",
             sbc->cfg_name);
      destroy_output_plugin(p);
      return NULL;
    }
    int ret = snprintf(state_file, len, "%s/%s.data", cfg->output_path,
                       sbc->cfg_name);
    if (ret < 0 || ret > (int)len - 1) {
      hs_log(NULL, g_module, 3, "%s failed to construct the state_file path",
             sbc->cfg_name);
      free(state_file);
      destroy_output_plugin(p);
      return NULL;
    }
  }
  lsb_output_buffer ob;
  if (lsb_init_output_buffer(&ob, 8 * 1024)) {
    hs_log(NULL, g_module, 3, "%s configuration memory allocation failed",
           sbc->cfg_name);
    free(state_file);
    destroy_output_plugin(p);
    return NULL;
  }
  if (!hs_get_full_config(&ob, 'o', cfg, sbc)) {
    hs_log(NULL, g_module, 3, "%s hs_get_full_config failed", sbc->cfg_name);
    lsb_free_output_buffer(&ob);
    free(state_file);
    destroy_output_plugin(p);
    return NULL;
  }
  lsb_logger logger = { .context = NULL, .cb = hs_log };
  p->hsb = lsb_heka_create_output(p, lua_file, state_file, ob.buf, &logger,
                                  update_checkpoint_callback);
  lsb_free_output_buffer(&ob);
  free(sbc->cfg_lua);
  sbc->cfg_lua = NULL;
  free(state_file);
  if (!p->hsb) {
    destroy_output_plugin(p);
    hs_log(NULL, g_module, 3, "%s lsb_heka_create_output failed",
           sbc->cfg_name);
    return NULL;
  }

  return p;
}


static void shutdown_timer_event(hs_output_plugin *p)
{
  if (lsb_heka_is_running(p->hsb)) {
    if (lsb_heka_timer_event(p->hsb, time(NULL), true)) {
      hs_log(NULL, p->name, 3, "terminated: %s", lsb_heka_get_error(p->hsb));
    }
  }
}


static int output_message(hs_output_plugin *p, lsb_heka_message *msg)
{
  int ret = 0, te_ret = 0;
  bool sample = p->sample;
  time_t current_t = time(NULL);
  unsigned long long start;

  if (msg->raw.s) { // non idle/empty message
    unsigned long long mmdelta = 0;
    if (sample) start = lsb_get_time();
    bool matched = lsb_eval_message_matcher(p->mm, msg);
    if (sample) {
      mmdelta = lsb_get_time() - start;
    }
    if (matched) {
      if (p->async_len) {
        int i = p->sequence_id % p->async_len;
        p->async_cp[i].input.id = p->cur.input.id;
        p->async_cp[i].input.offset = p->cur.input.offset;
        p->async_cp[i].analysis.id = p->cur.analysis.id;
        p->async_cp[i].analysis.offset = p->cur.analysis.offset;
      }
      ret = lsb_heka_pm_output(p->hsb, msg, (void *)p->sequence_id,
                               sample);
      if (ret <= 0) {
        if (ret == LSB_HEKA_PM_SENT) {
          p->batching = false;
        } else if (ret == LSB_HEKA_PM_BATCH) {
          p->batching = true;
        } else if (ret == LSB_HEKA_PM_ASYNC) {
          if (!p->async_len) {
            lsb_heka_terminate_sandbox(p->hsb, "cannot use async checkpointing "
                                       "without a configured buffer");
            ret = 1;
          }
          p->batching = true;
        } else if (ret == LSB_HEKA_PM_FAIL) {
          const char *err = lsb_heka_get_error(p->hsb);
          if (strlen(err) > 0) {
            hs_log(NULL, p->name, 4, "process_message returned: %d %s", ret,
                   err);
          }
        }
        if (ret != LSB_HEKA_PM_RETRY) ++p->sequence_id;
      }
    }

    // advance the checkpoint if not batching/async
    if (ret <= 0 && !p->batching) {
      update_checkpoint(p);
    }

    if (mmdelta) {
      pthread_mutex_lock(&p->cp_lock);
      lsb_update_running_stats(&p->mms, mmdelta);
      p->sample = false;
      pthread_mutex_unlock(&p->cp_lock);
    }
  }

  if (ret <= 0 && p->ticker_interval
      && current_t >= p->ticker_expires) {
    te_ret = lsb_heka_timer_event(p->hsb, current_t, false);
    p->ticker_expires = current_t + p->ticker_interval;
  }

  if (ret > 0 || te_ret > 0) {
    hs_log(NULL, p->name, 3, "terminated: %s", lsb_heka_get_error(p->hsb));
    return 1;
  }
  return ret;
}


static void* input_thread(void *arg)
{
  lsb_heka_message *msg = NULL;

  lsb_heka_message im, *pim = NULL;
  lsb_init_heka_message(&im, 8);

  lsb_heka_message am, *pam = NULL;
  lsb_init_heka_message(&am, 8);

  hs_output_plugin *p = (hs_output_plugin *)arg;
  hs_log(NULL, p->name, 6, "starting");

  size_t discarded_bytes;
  size_t bytes_read[2] = { 0 };
  int ret = 0;
  lsb_logger logger = { .context = NULL, .cb = hs_log };
#ifdef HINDSIGHT_CLI
  bool input_stop = false, analysis_stop = false;
  while (!(p->stop && input_stop && analysis_stop)) {
#else
  while (!p->stop) {
#endif
    if (p->input.fh && !pim) {
      if (lsb_find_heka_message(&im, &p->input.ib, true, &discarded_bytes,
                                &logger)) {
        pim = &im;
      } else {
        bytes_read[0] = hs_read_file(&p->input);
      }

      if (!bytes_read[0]) {
#ifdef HINDSIGHT_CLI
        size_t cid = p->input.cp.id;
#endif
        // see if the next file is there yet
        hs_open_file(&p->input, hs_input_dir, p->input.cp.id + 1);
#ifdef HINDSIGHT_CLI
        if (cid == p->input.cp.id && p->stop) {
          input_stop = true;
        }
#endif
      }
    } else if (!p->input.fh) { // still waiting on the first file
      hs_open_file(&p->input, hs_input_dir, p->input.cp.id);
#ifdef HINDSIGHT_CLI
      if (!p->input.fh && p->stop) {
        input_stop = true;
      }
#endif
    }

    if (p->analysis.fh && !pam) {
      if (lsb_find_heka_message(&am, &p->analysis.ib, true, &discarded_bytes,
                                &logger)) {
        pam = &am;
      } else {
        bytes_read[1] = hs_read_file(&p->analysis);
      }

      if (!bytes_read[1]) {
#ifdef HINDSIGHT_CLI
        size_t cid = p->analysis.cp.id;
#endif
        // see if the next file is there yet
        hs_open_file(&p->analysis, hs_analysis_dir, p->analysis.cp.id + 1);
#ifdef HINDSIGHT_CLI
        if (cid == p->analysis.cp.id && p->stop) {
          analysis_stop = true;
        }
#endif
      }
    } else if (!p->analysis.fh) { // still waiting on the first file
      hs_open_file(&p->analysis, hs_analysis_dir, p->analysis.cp.id);
#ifdef HINDSIGHT_CLI
      if (!p->analysis.fh && p->stop) {
        analysis_stop = true;
      }
#endif
    }

    // if we have one send the oldest first
    if (pim) {
      if (pam) {
        if (pim->timestamp <= pam->timestamp) {
          msg = pim;
        } else {
          msg = pam;
        }
      } else {
        msg = pim;
      }
    } else if (pam) {
      msg = pam;
    }

    if (msg) {
      if (msg == pim) {
        pim = NULL;
        p->cur.input.id = p->input.cp.id;
        p->cur.input.offset = p->input.cp.offset -
            (p->input.ib.readpos - p->input.ib.scanpos);
      } else {
        pam = NULL;
        p->cur.analysis.id = p->analysis.cp.id;
        p->cur.analysis.offset = p->analysis.cp.offset -
            (p->analysis.ib.readpos - p->analysis.ib.scanpos);
      }
      ret = output_message(p, msg);
      if (ret == LSB_HEKA_PM_RETRY) {
        while (!p->stop && ret == LSB_HEKA_PM_RETRY) {
          const char *err = lsb_heka_get_error(p->hsb);
          hs_log(NULL, p->name, 7, "retry message %llu err: %s", p->sequence_id,
                 err);
          sleep(1);
          ret = output_message(p, msg);
        }
      }
      if (ret > 0) {
        break; // fatal error
      }
      msg = NULL;
    } else if (!bytes_read[0] && !bytes_read[1]) {
      // trigger any pending timer events
      lsb_clear_heka_message(&im); // create an idle/empty message
      msg = &im;
      output_message(p, msg);
      msg = NULL;
      sleep(1);
    }
  }

  shutdown_timer_event(p);
  lsb_free_heka_message(&am);
  lsb_free_heka_message(&im);

  // hold the current checkpoints in memory incase we restart it
  hs_output_plugins *plugins = p->plugins;
  hs_update_input_checkpoint(&plugins->cfg->cp_reader,
                             hs_input_dir,
                             p->name,
                             &p->cp.input);

  hs_update_input_checkpoint(&plugins->cfg->cp_reader,
                             hs_analysis_dir,
                             p->name,
                             &p->cp.analysis);

  if (p->stop) {
    hs_log(NULL, p->name, 6, "shutting down");
  } else {
    hs_log(NULL, p->name, 6, "detaching received: %d msg: %s", ret,
           lsb_heka_get_error(p->hsb));
    if (plugins->cfg->rm_checkpoint) {
      char key[HS_MAX_PATH];
      snprintf(key, HS_MAX_PATH, "%s->%s", hs_input_dir, p->name);
      hs_remove_checkpoint(&plugins->cfg->cp_reader, key);
      snprintf(key, HS_MAX_PATH, "%s->%s", hs_analysis_dir, p->name);
      hs_remove_checkpoint(&plugins->cfg->cp_reader, key);
    }
    pthread_mutex_lock(&plugins->list_lock);
    plugins->list[p->list_index] = NULL;
    if (pthread_detach(p->thread)) {
      hs_log(NULL, p->name, 3, "thread could not be detached");
    }
    destroy_output_plugin(p);
    --plugins->list_cnt;
    pthread_mutex_unlock(&plugins->list_lock);
  }
  pthread_exit(NULL);
}


static void remove_plugin(hs_output_plugins *plugins, int idx)
{
  hs_output_plugin *p = plugins->list[idx];
  plugins->list[idx] = NULL;
  p->stop = true;
  if (pthread_join(p->thread, NULL)) {
    hs_log(NULL, p->name, 3, "remove_plugin could not pthread_join");
  }
  destroy_output_plugin(p);
  --plugins->list_cnt;
}


static void remove_from_output_plugins(hs_output_plugins *plugins,
                                       const char *name)
{
  const size_t tlen = strlen(hs_output_dir) + 1;
  hs_output_plugin *p;
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    p = plugins->list[i];
    if (!p) continue;

    char *pos = p->name + tlen;
    if (strstr(name, pos) && strlen(pos) == strlen(name) - HS_EXT_LEN) {
      remove_plugin(plugins, i);
      if (plugins->cfg->rm_checkpoint) {
        char key[HS_MAX_PATH];
        snprintf(key, HS_MAX_PATH, "%s->%s.%.*s", hs_input_dir,
                 hs_output_dir, (int)strlen(name) - HS_EXT_LEN, name);
        hs_remove_checkpoint(&plugins->cfg->cp_reader, key);
        snprintf(key, HS_MAX_PATH, "%s->%s.%.*s", hs_analysis_dir,
                 hs_output_dir, (int)strlen(name) - HS_EXT_LEN, name);
        hs_remove_checkpoint(&plugins->cfg->cp_reader, key);
      }
      break;
    }
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


static void add_plugin(hs_output_plugins *plugins, hs_output_plugin *p, int idx)
{
  plugins->list[idx] = p;
  p->list_index = idx;
  ++plugins->list_cnt;
}

static void add_to_output_plugins(hs_output_plugins *plugins,
                                  hs_output_plugin *p,
                                  bool checkpoint_newest)
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
    hs_output_plugin **tmp = realloc(plugins->list,
                                     sizeof(hs_output_plugin *)
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

  hs_config *cfg = p->plugins->cfg;
  // sync the output and read checkpoints
  // the read and output checkpoints can differ to allow for batching
  hs_lookup_input_checkpoint(&cfg->cp_reader,
                             hs_input_dir,
                             p->name,
                             cfg->output_path,
                             &p->input.cp,
                             checkpoint_newest);
  p->cur.input.id = p->cp.input.id = p->input.cp.id;
  p->cur.input.offset = p->cp.input.offset = p->input.cp.offset;

  hs_lookup_input_checkpoint(&cfg->cp_reader,
                             hs_analysis_dir,
                             p->name,
                             cfg->output_path,
                             &p->analysis.cp,
                             checkpoint_newest);
  p->cur.analysis.id = p->cp.analysis.id = p->analysis.cp.id;
  p->cur.analysis.offset = p->cp.analysis.offset = p->analysis.cp.offset;

  int ret = pthread_create(&p->thread, NULL, input_thread, (void *)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
}


void hs_init_output_plugins(hs_output_plugins *plugins, hs_config *cfg)
{
  plugins->cfg = cfg;
  plugins->list = NULL;
  plugins->list_cnt = 0;
  plugins->list_cap = 0;
  if (pthread_mutex_init(&plugins->list_lock, NULL)) {
    perror("list_lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
}


void hs_wait_output_plugins(hs_output_plugins *plugins)
{
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;

    hs_output_plugin *p = plugins->list[i];
    plugins->list[i] = NULL;
    if (pthread_join(p->thread, NULL)) {
      hs_log(NULL, p->name, 3, "thread could not be joined");
    }
    destroy_output_plugin(p);
    --plugins->list_cnt;
  }
  pthread_mutex_unlock(&plugins->list_lock);
}


void hs_free_output_plugins(hs_output_plugins *plugins)
{
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (plugins->list[i]) {
      destroy_output_plugin(plugins->list[i]);
      plugins->list[i] = NULL;
    }
  }
  free(plugins->list);

  pthread_mutex_destroy(&plugins->list_lock);
  plugins->list = NULL;
  plugins->cfg = NULL;
  plugins->list_cnt = 0;
  plugins->list_cap = 0;
}


static void process_lua(hs_output_plugins *plugins, const char *lpath,
                        const char *rpath, DIR *dp)
{
  char lua_lpath[HS_MAX_PATH];
  char lua_rpath[HS_MAX_PATH];
  char cfg_lpath[HS_MAX_PATH];
  char cfg_rpath[HS_MAX_PATH];
  size_t tlen = strlen(hs_output_dir) + 1;

  struct dirent *entry;
  while ((entry = readdir(dp))) {
    if (hs_has_ext(entry->d_name, hs_lua_ext)) {
      // move the Lua to the run directory
      if (!hs_get_fqfn(lpath, entry->d_name, lua_lpath, sizeof(lua_lpath))) {
        hs_log(NULL, g_module, 0, "load lua path too long");
        exit(EXIT_FAILURE);
      }
      if (!hs_get_fqfn(rpath, entry->d_name, lua_rpath, sizeof(lua_rpath))) {
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

        hs_output_plugin *p = plugins->list[i];
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


void hs_load_output_plugins(hs_output_plugins *plugins, const hs_config *cfg,
                            bool dynamic)
{
  char lpath[HS_MAX_PATH];
  char rpath[HS_MAX_PATH];
  if (!hs_get_fqfn(cfg->load_path, hs_output_dir, lpath, sizeof(lpath))) {
    hs_log(NULL, g_module, 0, "load path too long");
    exit(EXIT_FAILURE);
  }
  if (!hs_get_fqfn(cfg->run_path, hs_output_dir, rpath, sizeof(rpath))) {
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
        remove_from_output_plugins(plugins, entry->d_name);
        break;
      case 1: // proceed to load
        break;
      default: // ignore
        continue;
      }
    }
    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(rpath, entry->d_name, &sbc, &cfg->opd, 'o')) {
      hs_output_plugin *p = create_output_plugin(cfg, &sbc);
      if (p) {
        p->plugins = plugins;
        hs_init_input(&p->input, cfg->max_message_size, cfg->output_path,
                      p->name);
        hs_init_input(&p->analysis, cfg->max_message_size, cfg->output_path,
                      p->name);
        add_to_output_plugins(plugins, p, sbc.checkpoint_newest);
      } else {
        hs_log(NULL, g_module, 3, "%s create_output_plugin failed",
               sbc.cfg_name);
      }
      hs_free_sandbox_config(&sbc);
    }
  }
  closedir(dp);
}


void hs_stop_output_plugins(hs_output_plugins *plugins)
{
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;
    plugins->list[i]->stop = true;
  }
  pthread_mutex_unlock(&plugins->list_lock);
}
