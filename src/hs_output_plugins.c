/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
* License, v. 2.0. If a copy of the MPL was not distributed with this
* file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/** @brief Hindsight output plugin loader @file */

#include "hs_output_plugins.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <luasandbox.h>
#include <luasandbox/heka/sandbox.h>
#include <luasandbox/lauxlib.h>
#include <luasandbox/util/protobuf.h>
#include <luasandbox/util/running_stats.h>
#include <luasandbox_output.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "hs_input.h"
#include "hs_output.h"
#include "hs_util.h"

static const char g_module[] = "output_plugins";


static int inject_message(void *parent, const char *pb, size_t pb_len)
{
  static time_t last_bp_check = 0;
  static bool backpressure = false;
  static char header[14];

  hs_output_plugin *p = parent;
  bool bp;
  pthread_mutex_lock(&p->plugins->output->lock);
  int len = lsb_pb_output_varint(header + 3, pb_len);
  int tlen = 4 + len + pb_len;
  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  if (fwrite(header, 4 + len, 1, p->plugins->output->fh) == 1
      && fwrite(pb, pb_len, 1, p->plugins->output->fh) == 1) {
    p->plugins->output->cp.offset += tlen;
    if (p->plugins->output->cp.offset >= p->plugins->cfg->output_size) {
      ++p->plugins->output->cp.id;
      hs_open_output_file(p->plugins->output);
      if (p->plugins->cfg->backpressure
          && p->plugins->output->cp.id - p->plugins->output->min_cp_id
          > p->plugins->cfg->backpressure) {
        backpressure = true;
        hs_log(NULL, g_module, 4, "applying backpressure (checkpoint)");
      }
      if (!backpressure && p->plugins->cfg->backpressure_df) {
        unsigned df = hs_disk_free_ob(p->plugins->output->path,
                                      p->plugins->cfg->output_size);
        if (df <= p->plugins->cfg->backpressure_df) {
          backpressure = true;
          hs_log(NULL, g_module, 4, "applying backpressure (disk)");
        }
      }
    }
    if (backpressure && last_bp_check < time(NULL)) {
      last_bp_check = time(NULL);
      bool release_dfbp = true;
      if (p->plugins->cfg->backpressure_df) {
        unsigned df = hs_disk_free_ob(p->plugins->output->path,
                                      p->plugins->cfg->output_size);
        release_dfbp = (df > p->plugins->cfg->backpressure_df);
      }
      // even if we triggered on disk space continue to backpressure
      // until the queue is caught up too
      if (p->plugins->output->cp.id == p->plugins->output->min_cp_id
          && release_dfbp) {
        backpressure = false;
        hs_log(NULL, g_module, 4, "releasing backpressure");
      }
    }
  } else {
    hs_log(NULL, g_module, 0, "inject_message fwrite failed: %s",
           strerror(ferror(p->plugins->output->fh)));
    exit(EXIT_FAILURE);
  }
  bp = backpressure;
  pthread_mutex_unlock(&p->plugins->output->lock);

  if (bp) {
    usleep(100000); // throttle to 10 messages per second
  }
  return LSB_HEKA_IM_SUCCESS;
}


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


static void remove_checkpoint_q(hs_output_plugins *plugins,
                                const char *plugin_name,
                                char q)
{
  char key[HS_MAX_PATH];
  if (q >= 'b') {
    snprintf(key, HS_MAX_PATH, "%s->%s", hs_input_dir, plugin_name);
    hs_remove_checkpoint(plugins->cpr, key);
  }
  if (q <= 'b') {
    snprintf(key, HS_MAX_PATH, "%s->%s", hs_analysis_dir, plugin_name);
    hs_remove_checkpoint(plugins->cpr, key);
  }
}


static int update_checkpoint_callback(void *parent, void *sequence_id)
{
  hs_output_plugin *p = parent;

  if (sequence_id && p->async_cp) {
    if ((uintptr_t)sequence_id > p->ack_sequence_id
        || p->sequence_id < p->ack_sequence_id) { // handle wrapping
      p->ack_sequence_id = (uintptr_t)sequence_id;
    }
    int i = (uintptr_t)sequence_id % p->async_len;
    pthread_mutex_lock(&p->cp_lock);
    if ((p->async_cp[i].input.id == p->cp.input.id
         && p->async_cp[i].input.offset > p->cp.input.offset)
        || p->async_cp[i].input.id > p->cp.input.id) {
      p->cp.input.id = p->async_cp[i].input.id;
      p->cp.input.offset = p->async_cp[i].input.offset;
    }
    if ((p->async_cp[i].analysis.id == p->cp.analysis.id
         && p->async_cp[i].analysis.offset > p->cp.analysis.offset)
        || p->async_cp[i].analysis.id > p->cp.analysis.id) {
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
  char lua_file[HS_MAX_PATH];
  if (!hs_find_lua(cfg, sbc, hs_output_dir, lua_file, sizeof(lua_file))) {
    hs_log(NULL, g_module, 3, "%s failed to find the specified lua filename: %s"
           , sbc->cfg_name, sbc->filename);
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
  p->sequence_id = 0;
  p->ack_sequence_id = 0;
  p->ticker_interval = sbc->ticker_interval;
  p->rm_cp_terminate = sbc->rm_cp_terminate;
  p->read_queue = sbc->read_queue;
  p->shutdown_terminate = sbc->shutdown_terminate;
  p->pm_sample = true;
  int stagger = p->ticker_interval > 60 ? 60 : p->ticker_interval;
  // distribute when the timer_events will fire
  if (stagger) {
    p->ticker_expires = time(NULL) + rand() % stagger;
#ifdef HINDSIGHT_CLI
    p->ticker_expires = 0;
#endif
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
  p->ctx.plugin_name = p->name;
  p->ctx.output_path = cfg->run_path;

  char *state_file = NULL;
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
  if (lsb_init_output_buffer(&ob, strlen(sbc->cfg_lua) + (8 * 1024))) {
    hs_log(NULL, g_module, 3, "%s configuration memory allocation failed",
           sbc->cfg_name);
    free(state_file);
    destroy_output_plugin(p);
    return NULL;
  }
  if (!hs_output_runtime_cfg(&ob, 'o', cfg, sbc)) {
    hs_log(NULL, g_module, 3, "failed to write %s/%s%s", cfg->output_path,
           sbc->cfg_name, hs_rtc_ext);
    lsb_free_output_buffer(&ob);
    free(state_file);
    destroy_output_plugin(p);
    return NULL;
  }
  lsb_logger logger = { .context = &p->ctx, .cb = hs_log };
  p->hsb = lsb_heka_create_output_im(p, lua_file, state_file, ob.buf, &logger,
                                  update_checkpoint_callback, inject_message);

  if (!p->hsb && hs_is_bad_state(cfg->run_path, p->name, state_file)) {
    p->hsb = lsb_heka_create_output_im(p, lua_file, state_file, ob.buf, &logger,
                                    update_checkpoint_callback, inject_message);
  }
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

  p->ctx.plugin_name = NULL;
  p->ctx.output_path = NULL;
  return p;
}


static void shutdown_timer_event(hs_output_plugin *p, time_t current_t)
{
  if (lsb_heka_is_running(p->hsb)) {
    if (lsb_heka_timer_event(p->hsb, current_t, true)) {
      hs_log(NULL, p->name, 3, "terminated: %s", lsb_heka_get_error(p->hsb));
    }
  }
}


static int output_message(hs_output_plugin *p, lsb_heka_message *msg,
                          bool sample, time_t current_t)
{
  int ret = 0, te_ret = 0;
  unsigned long long start;
  unsigned long long mmdelta = 0;

  if (msg->raw.s) { // non idle/empty message
    if (sample) start = lsb_get_time();
    bool matched = lsb_eval_message_matcher(p->mm, msg);
    if (sample) {
      mmdelta = lsb_get_time() - start;
      p->pm_sample = true;
    }
    if (matched) {
      if (p->async_len) {
        int i = (p->sequence_id + 1) % p->async_len;
        p->async_cp[i].input.id = p->cur.input.id;
        p->async_cp[i].input.offset = p->cur.input.offset;
        p->async_cp[i].analysis.id = p->cur.analysis.id;
        p->async_cp[i].analysis.offset = p->cur.analysis.offset;
      }
      ret = lsb_heka_pm_output(p->hsb, msg, (void *)(p->sequence_id + 1),
                               p->pm_sample);
      p->pm_sample = false;
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
        } else if (ret == LSB_HEKA_PM_FAIL) {
          const char *err = lsb_heka_get_error(p->hsb);
          if (strlen(err) > 0) {
            hs_log(NULL, p->name, 4, "process_message returned: %d %s", ret,
                   err);
          }
        }
        if (ret != LSB_HEKA_PM_RETRY) {
          pthread_mutex_lock(&p->cp_lock);
          ++p->pm_delta_cnt;
          pthread_mutex_unlock(&p->cp_lock);
          ++p->sequence_id;
        }
      }
    } else {
      if (sample) p->pm_sample = true;
    }

    // advance the checkpoint if not fatal/batching/pending asyc/retrying
    bool pending = (p->async_len && p->sequence_id != p->ack_sequence_id);
    if (ret <= 0 && !p->batching && !pending && ret != LSB_HEKA_PM_RETRY) {
      update_checkpoint(p);
    }
  }

  if (ret <= 0 && p->ticker_interval
      && current_t >= p->ticker_expires) {
    te_ret = lsb_heka_timer_event(p->hsb, current_t, false);
    p->ticker_expires = current_t + p->ticker_interval;
  }

  if (sample) {
    pthread_mutex_lock(&p->cp_lock);
    if (mmdelta) {
      lsb_update_running_stats(&p->mms, mmdelta);
    }
    p->stats = lsb_heka_get_stats(p->hsb);
    p->sample = false;
    pthread_mutex_unlock(&p->cp_lock);
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

  size_t db;
  size_t bytes_read[2] = { 0 };
  int ret = 0;
  bool stop = false;
  bool sample = false;
  time_t current_t = time(NULL);
  lsb_logger logger = { .context = NULL, .cb = hs_log };
#ifdef HINDSIGHT_CLI
  long long cli_ns = 0;
  bool input_stop = p->read_queue == 'a';
  bool analysis_stop = p->read_queue == 'i';
  while (!(stop && input_stop && analysis_stop)) {
#else
  time_t itimer = 0;
  time_t atimer = 0;
  bool inext = false;
  bool anext = false;
  int iwait_cnt = 0;
  int await_cnt = 0;
  while (!stop) {
#endif
    pthread_mutex_lock(&p->cp_lock);
    stop = p->stop;
    sample = p->sample;
    pthread_mutex_unlock(&p->cp_lock);
#ifndef HINDSIGHT_CLI
    current_t = time(NULL);
#endif

    if (p->read_queue >= 'b') {
      if (p->input.fh && !pim) {
        if (lsb_find_heka_message(&im, &p->input.ib, true, &db, &logger)) {
          pim = &im;
        } else {
          bytes_read[0] = hs_read_file(&p->input);
#ifdef HINDSIGHT_CLI
          bool next = false;
          if (!bytes_read[0]
              && (p->input.cp.offset >= p->plugins->cfg->output_size)) {
            next = hs_open_file(&p->input, hs_input_dir, p->input.cp.id + 1);
          }
          if (!bytes_read[0] && !next && stop) {
            input_stop = true;
          }
#else
          // When the read gets to the end it will always check once for the
          // next available file just incase the output_size was increased on
          // the last restart.
          if (!bytes_read[0] &&
              (p->input.cp.offset >= p->plugins->cfg->output_size || inext)) {
            if (current_t != itimer) {
              itimer = current_t;
              inext = hs_open_file(&p->input, hs_input_dir, p->input.cp.id + 1);
              if (inext) {
                iwait_cnt = 0;
              } else {
                if (++iwait_cnt > 60
                    || p->input.cp.offset < p->plugins->cfg->output_size) {
                  size_t next_id = hs_find_next_id(p->plugins->cfg->output_path,
                                                   hs_input_dir,
                                                   p->input.cp.id);
                  if (next_id > p->input.cp.id + 1) {
                    hs_log(NULL, p->name, 3,
                           "the input checkpoint skipped %zu missing files",
                           next_id - p->input.cp.id - 1);
                    inext = hs_open_file(&p->input, hs_input_dir, next_id);
                    if (!inext) {
                      hs_log(NULL, p->name, 2,
                             "unable to open input queue file: %zu", next_id);
                    }
                  }
                  iwait_cnt = 0;
                }
              }
            }
          }
#endif
        }
      } else if (!p->input.fh) { // still waiting on the first file
#ifdef HINDSIGHT_CLI
        bool next = hs_open_file(&p->input, hs_input_dir, p->input.cp.id);
        if (!next && stop) input_stop = true;
#else
        if (current_t != itimer) {
          itimer = current_t;
          if (++iwait_cnt > 60) {
            // the internal state is bad (manual prune?)
            hs_lookup_input_checkpoint(p->plugins->cpr,
                                       hs_input_dir,
                                       NULL, // restart from the end
                                       p->plugins->cfg->output_path,
                                       &p->input.cp);
            pthread_mutex_lock(&p->cp_lock);
            p->cur.input.id = p->cp.input.id = p->input.cp.id;
            p->cur.input.offset = p->cp.input.offset = p->input.cp.offset;
            pthread_mutex_unlock(&p->cp_lock);
            hs_log(NULL, p->name, 3, "the input checkpoint was reset");
            iwait_cnt = 0;
          }
          inext = hs_open_file(&p->input, hs_input_dir, p->input.cp.id);
          if (inext) iwait_cnt = 0;
        }
#endif
      }
    }

    if (p->read_queue <= 'b') {
      if (p->analysis.fh && !pam) {
        if (lsb_find_heka_message(&am, &p->analysis.ib, true, &db, &logger)) {
          pam = &am;
        } else {
          bytes_read[1] = hs_read_file(&p->analysis);
#ifdef HINDSIGHT_CLI
          bool next = false;
          if (!bytes_read[1]
              && (p->analysis.cp.offset >= p->plugins->cfg->output_size)) {
            next = hs_open_file(&p->analysis, hs_analysis_dir,
                                p->analysis.cp.id + 1);
          }
          if (!bytes_read[1] && !next && input_stop && stop) {
            analysis_stop = true;
          }
#else
          // When the read gets to the end it will always check once for the
          // next available file just incase the output_size was increased on
          // the last restart.
          if (!bytes_read[1]
              && (p->analysis.cp.offset >= p->plugins->cfg->output_size
                  || anext)) {
            if (current_t != atimer) {
              atimer = current_t;
              anext = hs_open_file(&p->analysis, hs_analysis_dir,
                                   p->analysis.cp.id + 1);
              if (anext) {
                await_cnt = 0;
              } else {
                if (++await_cnt > 60
                    || p->analysis.cp.offset < p->plugins->cfg->output_size) {
                  size_t next_id = hs_find_next_id(p->plugins->cfg->output_path,
                                                   hs_analysis_dir,
                                                   p->analysis.cp.id);
                  if (next_id > p->analysis.cp.id + 1) {
                    hs_log(NULL, p->name, 3,
                           "the analysis checkpoint skipped %zu missing files",
                           next_id - p->analysis.cp.id - 1);
                    anext = hs_open_file(&p->analysis, hs_analysis_dir,
                                         next_id);
                    if (!anext) {
                      hs_log(NULL, p->name, 2,
                             "unable to open analysis queue file: %zu",
                             next_id);
                    }
                  }
                  await_cnt = 0;
                }
              }
            }
          }
#endif
        }
      } else if (!p->analysis.fh) { // still waiting on the first file
#ifdef HINDSIGHT_CLI
        bool next = hs_open_file(&p->analysis, hs_analysis_dir, p->analysis.cp.id);
        if (!next && input_stop && stop) analysis_stop = true;
#else
        if (current_t != atimer) {
          atimer = current_t;
          if (++await_cnt > 60) {
            // the internal state is bad (manual prune?)
            hs_lookup_input_checkpoint(p->plugins->cpr,
                                       hs_analysis_dir,
                                       NULL, // restart from the end
                                       p->plugins->cfg->output_path,
                                       &p->analysis.cp);
            pthread_mutex_lock(&p->cp_lock);
            p->cur.analysis.id = p->cp.analysis.id = p->analysis.cp.id;
            p->cur.analysis.offset = p->cp.analysis.offset = p->analysis.cp.offset;
            pthread_mutex_unlock(&p->cp_lock);
            hs_log(NULL, p->name, 3, "the analysis checkpoint was reset");
            await_cnt = 0;
          }
          anext = hs_open_file(&p->analysis, hs_analysis_dir,
                               p->analysis.cp.id);
          if (anext) await_cnt = 0;
        }
#endif
      }
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
      pthread_mutex_lock(&p->cp_lock);
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
      ++p->mm_delta_cnt;
      pthread_mutex_unlock(&p->cp_lock);
#ifdef HINDSIGHT_CLI
      if (msg->timestamp > cli_ns) {
        cli_ns = msg->timestamp;
        current_t = cli_ns / 1000000000LL;
      }
#endif
      ret = output_message(p, msg, sample, current_t);
      if (ret == LSB_HEKA_PM_RETRY) {
        while (!stop) {
          const char *err = lsb_heka_get_error(p->hsb);
          hs_log(NULL, p->name, 7, "retry message %llu err: %s",
                 p->sequence_id + 1, err);
          sleep(1);
#ifndef HINDSIGHT_CLI
          current_t = time(NULL);
#endif
          ret = output_message(p, msg, false, current_t);
          if (ret == LSB_HEKA_PM_RETRY) {
            pthread_mutex_lock(&p->cp_lock);
            stop = p->stop;
            pthread_mutex_unlock(&p->cp_lock);
            continue;
          }
          break;
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
      output_message(p, msg, sample, current_t);
      msg = NULL;
      sleep(1);
    }
  }

  shutdown_timer_event(p, current_t);
  lsb_free_heka_message(&am);
  lsb_free_heka_message(&im);

// hold the current checkpoints in memory incase we restart it
  hs_output_plugins *plugins = p->plugins;

  if (p->read_queue >= 'b') {
    hs_update_input_checkpoint(plugins->cpr,
                               hs_input_dir,
                               p->name,
                               &p->cp.input);
  }

  if (p->read_queue <= 'b') {
    hs_update_input_checkpoint(plugins->cpr,
                               hs_analysis_dir,
                               p->name,
                               &p->cp.analysis);
  }

  if (stop) {
    hs_log(NULL, p->name, 6, "shutting down");
  } else {
    const char *err = lsb_heka_get_error(p->hsb);
    hs_log(NULL, p->name, 6, "detaching received: %d msg: %s", ret, err);
    hs_save_termination_err(plugins->cfg->run_path, p->name, err);
    if (p->rm_cp_terminate) {
      remove_checkpoint_q(plugins, p->name, p->read_queue);
    }
    pthread_mutex_lock(&plugins->list_lock);
#ifdef HINDSIGHT_CLI
    plugins->terminated = true;
#endif
    plugins->list[p->list_index] = NULL;
    if (pthread_detach(p->thread)) {
      hs_log(NULL, p->name, 3, "thread could not be detached");
    }
    if (p->shutdown_terminate) {
      hs_log(NULL, p->name, 6, "shutting down on terminate");
      kill(getpid(), SIGTERM);
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
  pthread_mutex_lock(&p->cp_lock);
  p->stop = true;
  pthread_mutex_unlock(&p->cp_lock);
  if (pthread_join(p->thread, NULL)) {
    hs_log(NULL, p->name, 3, "remove_plugin could not pthread_join");
  }
  destroy_output_plugin(p);
  --plugins->list_cnt;
}


static bool
remove_from_output_plugins(hs_output_plugins *plugins, const char *name)
{
  bool removed = false;
  const size_t tlen = strlen(hs_output_dir) + 1;
  hs_output_plugin *p;
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    p = plugins->list[i];
    if (!p) continue;

    char *pos = p->name + tlen;
    if (strstr(name, pos) && strlen(pos) == strlen(name) - HS_EXT_LEN) {
      remove_plugin(plugins, i);
      removed = true;
      break;
    }
  }
  pthread_mutex_unlock(&plugins->list_lock);
  return removed;
}


static void add_plugin(hs_output_plugins *plugins, hs_output_plugin *p, int idx)
{
  plugins->list[idx] = p;
  p->list_index = idx;
  ++plugins->list_cnt;
}

static void add_to_output_plugins(hs_output_plugins *plugins,
                                  hs_output_plugin *p,
                                  bool dynamic)
{
  int idx = -1;
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) {
      idx = i;
      break;
    }
  }
  if (idx != -1) {
    add_plugin(plugins, p, idx);
  } else {
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

  const char *path = dynamic ? NULL : p->plugins->cfg->output_path;
  // sync the output and read checkpoints
  // the read and output checkpoints can differ to allow for batching
  if (p->read_queue >= 'b') {
    hs_lookup_input_checkpoint(p->plugins->cpr,
                               hs_input_dir,
                               p->name,
                               path,
                               &p->input.cp);
    p->cur.input.id = p->cp.input.id = p->input.cp.id;
    p->cur.input.offset = p->cp.input.offset = p->input.cp.offset;
  } else {
    remove_checkpoint_q(plugins, p->name, 'i');
  }

  if (p->read_queue <= 'b') {
    hs_lookup_input_checkpoint(p->plugins->cpr,
                               hs_analysis_dir,
                               p->name,
                               path,
                               &p->analysis.cp);
    p->cur.analysis.id = p->cp.analysis.id = p->analysis.cp.id;
    p->cur.analysis.offset = p->cp.analysis.offset = p->analysis.cp.offset;
  } else {
    remove_checkpoint_q(plugins, p->name, 'a');
  }

  int ret = pthread_create(&p->thread, NULL, input_thread, (void *)p);
  if (ret) {
    perror("pthread_create failed");
    exit(EXIT_FAILURE);
  }
}


void hs_init_output_plugins(hs_output_plugins *plugins,
                            hs_config *cfg,
                            hs_checkpoint_reader *cpr,
                            hs_output *output)
{
  plugins->cfg = cfg;
  plugins->cpr = cpr;
  plugins->output = output;
  plugins->list = NULL;
  plugins->list_cnt = 0;
  plugins->list_cap = 0;
#ifdef HINDSIGHT_CLI
  plugins->terminated = false;
#endif
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
#ifdef HINDSIGHT_CLI
    if (!lsb_heka_is_running(p->hsb)) {
      plugins->terminated = true;
    }
#endif
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
  plugins->output = NULL;
  plugins->list = NULL;
  plugins->cfg = NULL;
  plugins->list_cnt = 0;
  plugins->list_cap = 0;
}


static void process_lua(hs_output_plugins *plugins, const char *name)
{
  hs_config *cfg = plugins->cfg;
  const char *lpath = cfg->load_path_output;
  const char *rpath = cfg->run_path_output;
  char lua_lpath[HS_MAX_PATH];
  char lua_rpath[HS_MAX_PATH];
  char cfg_lpath[HS_MAX_PATH];
  char cfg_rpath[HS_MAX_PATH];
  size_t tlen = strlen(hs_output_dir) + 1;

  // move the Lua to the run directory
  if (hs_get_fqfn(lpath, name, lua_lpath, sizeof(lua_lpath))) {
    hs_log(NULL, g_module, 0, "load lua path too long");
    exit(EXIT_FAILURE);
  }
  if (hs_get_fqfn(rpath, name, lua_rpath, sizeof(lua_rpath))) {
    hs_log(NULL, g_module, 0, "run lua path too long");
    exit(EXIT_FAILURE);
  }
  if (rename(lua_lpath, lua_rpath)) {
    hs_log(NULL, g_module, 3, "failed to move: %s to %s errno: %d",
           lua_lpath, lua_rpath, errno);
    return;
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


void hs_load_output_startup(hs_output_plugins *plugins)
{
  hs_config *cfg = plugins->cfg;
  const char *dir = cfg->run_path_output;
  hs_prune_err(dir);

  DIR *dp = opendir(dir);
  if (dp == NULL) {
    hs_log(NULL, g_module, 0, "%s: %s", dir, strerror(errno));
    exit(EXIT_FAILURE);
  }

  struct dirent *entry;
  while ((entry = readdir(dp))) {
    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(dir, entry->d_name, &sbc, &cfg->opd, 'o')) {
      hs_output_plugin *p = create_output_plugin(cfg, &sbc);
      if (p) {
        p->plugins = plugins;
        hs_init_input(&p->input, cfg->max_message_size, cfg->output_path,
                      p->name);
        hs_init_input(&p->analysis, cfg->max_message_size, cfg->output_path,
                      p->name);
        add_to_output_plugins(plugins, p, false);
      } else {
#ifdef HINDSIGHT_CLI
        pthread_mutex_lock(&plugins->list_lock);
        plugins->terminated = true;
        pthread_mutex_unlock(&plugins->list_lock);
#endif
        hs_log(NULL, g_module, 3, "%s create_output_plugin failed",
               sbc.cfg_name);
      }
      hs_free_sandbox_config(&sbc);
    }
  }
  closedir(dp);
}


static void remove_checkpoints(hs_output_plugins *plugins, const char *filename)
{
  char key[HS_MAX_PATH];
  int fnlen = strlen(filename);

  snprintf(key, HS_MAX_PATH, "%s->%s.%.*s", hs_input_dir,
           hs_output_dir, fnlen - HS_EXT_LEN, filename);
  hs_remove_checkpoint(plugins->cpr, key);

  snprintf(key, HS_MAX_PATH, "%s->%s.%.*s", hs_analysis_dir,
           hs_output_dir, fnlen - HS_EXT_LEN, filename);
  hs_remove_checkpoint(plugins->cpr, key);
}


void hs_load_output_dynamic(hs_output_plugins *plugins, const char *filename)
{
  hs_config *cfg = plugins->cfg;
  const char *lpath = cfg->load_path_output;
  const char *rpath = cfg->run_path_output;

  if (hs_has_ext(filename, hs_lua_ext)) {
    process_lua(plugins, filename);
    return;
  }

  switch (hs_process_load_cfg(lpath, rpath, filename)) {
  case 0: // stop
    if (remove_from_output_plugins(plugins, filename)) {
      remove_checkpoints(plugins, filename);
    }
    break;
  case 1: // load/reload
    {
      bool rm_cp_terminate = false;
      bool loaded = false;
      bool removed = remove_from_output_plugins(plugins, filename);
      hs_sandbox_config sbc;
      if (hs_load_sandbox_config(rpath, filename, &sbc, &cfg->opd, 'o')) {
        rm_cp_terminate = sbc.rm_cp_terminate;
        hs_output_plugin *p = create_output_plugin(cfg, &sbc);
        if (p) {
          p->plugins = plugins;
          hs_init_input(&p->input, cfg->max_message_size, cfg->output_path,
                        p->name);
          hs_init_input(&p->analysis, cfg->max_message_size, cfg->output_path,
                        p->name);
          add_to_output_plugins(plugins, p, true);
          loaded = true;
        } else {
#ifdef HINDSIGHT_CLI
          pthread_mutex_lock(&plugins->list_lock);
          plugins->terminated = true;
          pthread_mutex_unlock(&plugins->list_lock);
#endif
          hs_log(NULL, g_module, 3, "%s create_output_plugin failed",
                 sbc.cfg_name);
        }
        hs_free_sandbox_config(&sbc);
      }
      if (removed && !loaded && rm_cp_terminate) {
        remove_checkpoints(plugins, filename);
      }
    }
    break;
  default:
    hs_log(NULL, g_module, 7, "%s ignored %s", __func__, filename);
    break;
  }
}


void hs_stop_output_plugins(hs_output_plugins *plugins)
{
  pthread_mutex_lock(&plugins->list_lock);
  for (int i = 0; i < plugins->list_cap; ++i) {
    if (!plugins->list[i]) continue;
    pthread_mutex_lock(&plugins->list[i]->cp_lock);
    plugins->list[i]->stop = true;
    pthread_mutex_unlock(&plugins->list[i]->cp_lock);
  }
  pthread_mutex_unlock(&plugins->list_lock);
}
