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
#include <luasandbox/util/protobuf.h>
#include <luasandbox_output.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
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
  if (p->im_limit == 0) return LSB_HEKA_IM_LIMIT;
  --p->im_limit;
  bool bp;
  pthread_mutex_lock(&p->at->plugins->output.lock);
  int len = lsb_pb_output_varint(header + 3, pb_len);
  int tlen = 4 + len + pb_len;
  header[0] = 0x1e;
  header[1] = (char)(len + 1);
  header[2] = 0x08;
  header[3 + len] = 0x1f;
  if (fwrite(header, 4 + len, 1, p->at->plugins->output.fh) == 1
      && fwrite(pb, pb_len, 1, p->at->plugins->output.fh) == 1) {
    p->at->plugins->output.cp.offset += tlen;
    if (p->at->plugins->output.cp.offset >= p->at->plugins->cfg->output_size) {
      ++p->at->plugins->output.cp.id;
      hs_open_output_file(&p->at->plugins->output);
      if (p->at->plugins->cfg->backpressure
          && p->at->plugins->output.cp.id - p->at->plugins->output.min_cp_id
          > p->at->plugins->cfg->backpressure) {
        backpressure = true;
        hs_log(NULL, g_module, 4, "applying backpressure (checkpoint)");
      }
      if (!backpressure && p->at->plugins->cfg->backpressure_df) {
        unsigned df = hs_disk_free_ob(p->at->plugins->output.path,
                                      p->at->plugins->cfg->output_size);
        if (df <= p->at->plugins->cfg->backpressure_df) {
          backpressure = true;
          hs_log(NULL, g_module, 4, "applying backpressure (disk)");
        }
      }
    }
    if (backpressure) {
      bool release_dfbp = true;
      if (p->at->plugins->cfg->backpressure_df) {
        unsigned df = hs_disk_free_ob(p->at->plugins->output.path,
                                      p->at->plugins->cfg->output_size);
        release_dfbp = (df > p->at->plugins->cfg->backpressure_df);
      }
      // even if we triggered on disk space continue to backpressure
      // until the queue is caught up too
      if (p->at->plugins->output.cp.id == p->at->plugins->output.min_cp_id
          && release_dfbp) {
        backpressure = false;
        hs_log(NULL, g_module, 4, "releasing backpressure");
      }
    }
  } else {
    hs_log(NULL, g_module, 0, "inject_message fwrite failed: %s",
           strerror(ferror(p->at->plugins->output.fh)));
    exit(EXIT_FAILURE);
  }
  bp = backpressure;
  pthread_mutex_unlock(&p->at->plugins->output.lock);

  if (bp) {
    usleep(100000); // throttle to 10 messages per second
  }
  return LSB_HEKA_IM_SUCCESS;
}


static void destroy_analysis_plugin(hs_analysis_plugin *p)
{
  if (!p) return;
  char *msg = lsb_heka_destroy_sandbox(p->hsb);
  if (msg) {
    hs_log(NULL, p->name, 3, "lsb_heka_destroy_sandbox failed: %s", msg);
    free(msg);
  }
  lsb_destroy_message_matcher(p->mm);
  free(p->name);
  free(p);
}


static hs_analysis_plugin*
create_analysis_plugin(const hs_config *cfg, hs_sandbox_config *sbc)
{
  char lua_file[HS_MAX_PATH];
  if (!hs_find_lua(cfg, sbc, hs_analysis_dir, lua_file, sizeof(lua_file))) {
    hs_log(NULL, g_module, 3, "%s failed to find the specified lua filename: %s"
           , sbc->cfg_name, sbc->filename);
    return NULL;
  }

  hs_analysis_plugin *p = calloc(1, sizeof(hs_analysis_plugin));
  if (!p) {
    hs_log(NULL, g_module, 2, "%s hs_analysis_plugin memory allocation failed",
           sbc->cfg_name);
    return NULL;
  }

  p->pm_im_limit = sbc->pm_im_limit;
  p->te_im_limit = sbc->te_im_limit;
  p->shutdown_terminate = sbc->shutdown_terminate;
  p->ticker_interval = sbc->ticker_interval;
  int stagger = p->ticker_interval > 60 ? 60 : p->ticker_interval;
  // distribute when the timer_events will fire
  if (stagger) {
    p->ticker_expires = time(NULL) + rand() % stagger;
#ifdef HINDSIGHT_CLI
    p->ticker_expires = 0;
#endif
  }

  p->mm = lsb_create_message_matcher(sbc->message_matcher);
  if (!p->mm) {
    hs_log(NULL, g_module, 3, "%s invalid message_matcher: %s", sbc->cfg_name,
           sbc->message_matcher);
    destroy_analysis_plugin(p);
    return NULL;
  }

  size_t len = strlen(sbc->cfg_name) + 1;
  p->name = malloc(len);
  if (!p->name) {
    hs_log(NULL, g_module, 2, "%s name memory allocation failed",
           sbc->cfg_name);
    destroy_analysis_plugin(p);
  }
  memcpy(p->name, sbc->cfg_name, len);

  char *state_file = NULL;
  if (sbc->preserve_data) {
    size_t len = strlen(cfg->output_path) + strlen(sbc->cfg_name) + 7;
    state_file = malloc(len);
    if (!state_file) {
      hs_log(NULL, g_module, 2, "%s state_file memory allocation failed",
             sbc->cfg_name);
      destroy_analysis_plugin(p);
      return NULL;
    }
    int ret = snprintf(state_file, len, "%s/%s.data", cfg->output_path,
                       sbc->cfg_name);
    if (ret < 0 || ret > (int)len - 1) {
      hs_log(NULL, g_module, 3, "%s failed to construct the state_file path",
             sbc->cfg_name);
      destroy_analysis_plugin(p);
      free(state_file);
      return NULL;
    }
  }
  lsb_output_buffer ob;
  if (lsb_init_output_buffer(&ob, strlen(sbc->cfg_lua) + (8 * 1024))) {
    hs_log(NULL, g_module, 3, "%s configuration memory allocation failed",
           sbc->cfg_name);
    free(state_file);
    destroy_analysis_plugin(p);
    return NULL;
  }
  if (!hs_output_runtime_cfg(&ob, 'a', cfg, sbc)) {
    hs_log(NULL, g_module, 3, "failed to write %s/%s%s", cfg->output_path,
           sbc->cfg_name, hs_rtc_ext);
    lsb_free_output_buffer(&ob);
    free(state_file);
    destroy_analysis_plugin(p);
    return NULL;
  }
  lsb_logger logger = { .context = NULL, .cb = hs_log };
  p->hsb = lsb_heka_create_analysis(p, lua_file, state_file, ob.buf, &logger,
                                    inject_message);
  lsb_free_output_buffer(&ob);
  free(sbc->cfg_lua);
  sbc->cfg_lua = NULL;
  free(state_file);
  if (!p->hsb) {
    hs_log(NULL, g_module, 3, "%s lsb_heka_create_analysis failed",
           sbc->cfg_name);
    destroy_analysis_plugin(p);
    return NULL;
  }

  return p;
}


static void remove_plugin(hs_analysis_thread *at, int idx)
{
  hs_log(NULL, at->list[idx]->name, 6, "removing from thread: %d", at->tid);
  hs_analysis_plugin *p = at->list[idx];
  at->list[idx] = NULL;
  destroy_analysis_plugin(p);
  p = NULL;
  --at->list_cnt;
  at->max_mps = 0; // invalidate the measure and switch back to the estimate
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
  hs_log(NULL, p->name, 6, "adding to thread: %d", at->tid);
  at->list[idx] = p;
  ++at->list_cnt;
}


static void add_to_analysis_plugins(const hs_sandbox_config *cfg,
                                    hs_analysis_plugins *plugins,
                                    hs_analysis_plugin *p)
{
  int idx = -1;
  int thread = cfg->thread % plugins->cfg->analysis_threads;
  hs_analysis_thread *at = &plugins->list[thread];
  p->at = at;

  pthread_mutex_lock(&at->list_lock);
  // todo shrink it down if there are a lot of empty slots
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) {
      idx = i;
      break;
    }
  }
  if (idx != -1) {
    add_plugin(at, p, idx);
  } else {
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
      hs_log(NULL, g_module, 0, "plugins realloc failed");
      exit(EXIT_FAILURE);
    }
  }
  at->max_mps = 0; // invalidate the measure and switch back to the estimate
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
  at->stop = false;
  at->sample = false;
  at->mm_delta_cnt = 0;
  at->max_mps = 0;

#ifdef HINDSIGHT_CLI
  at->terminated = false;
#endif

  char name[255];
  int n = snprintf(name, sizeof name, "%s%d", hs_analysis_dir, tid);
  if (n < 0 || n >= (int)sizeof name) {
    hs_log(NULL, g_module, 0, "name exceeded the buffer length: %s%d",
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
#ifdef HINDSIGHT_CLI
  at->terminated = true;
#endif
  const char *err = lsb_heka_get_error(at->list[i]->hsb);
  hs_log(NULL, at->list[i]->name, 3, "terminated: %s", err);
  hs_save_termination_err(at->plugins->cfg, at->list[i]->name, err);
  if (at->list[i]->shutdown_terminate) {
    hs_log(NULL, at->list[i]->name, 6, "shutting down on terminate");
    kill(getpid(), SIGTERM);
  }
  remove_plugin(at, i);
}


static void analyze_message(hs_analysis_thread *at, bool sample)
{
  hs_analysis_plugin *p = NULL;
  int ret;

  pthread_mutex_lock(&at->list_lock);
  for (int i = 0; i < at->list_cap; ++i) {
    if (!at->list[i]) continue;
    p = at->list[i];

    ret = 0;
    unsigned long long start;

    if (at->msg->raw.s) { // non idle/empty message
      if (sample) start = lsb_get_time();
      bool matched = lsb_eval_message_matcher(p->mm, at->msg);
      if (sample) {
        lsb_update_running_stats(&p->mms, lsb_get_time() - start);
      }
      if (matched) {
        p->im_limit = p->pm_im_limit;
        ++p->pm_delta_cnt;
        ret = lsb_heka_pm_analysis(p->hsb, at->msg, sample);
        if (ret < 0) {
          const char *err = lsb_heka_get_error(p->hsb);
          if (strlen(err) > 0) {
            hs_log(NULL, p->name, 4, "received: %d msg: %s", ret, err);
          }
        }
      }
    }
    if (sample) {
      p->stats = lsb_heka_get_stats(p->hsb);
    }

    if (ret <= 0 && p->ticker_interval && at->current_t >= p->ticker_expires) {
      p->im_limit = p->te_im_limit;
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

    at->list[i]->im_limit = at->list[i]->te_im_limit;
    if (lsb_heka_timer_event(at->list[i]->hsb, at->current_t, true)) {
      terminate_sandbox(at, i);
    }
  }
  pthread_mutex_unlock(&at->list_lock);
}


static void* input_thread(void *arg)
{
  hs_analysis_thread *at = (hs_analysis_thread *)arg;
  hs_log(NULL, g_module, 6, "starting thread: %d", at->tid);

  lsb_heka_message msg;
  lsb_init_heka_message(&msg, 8);

  size_t discarded_bytes;
  size_t bytes_read = 0;
  lsb_logger logger = { .context = NULL, .cb = hs_log };
  bool stop = false;
  bool sample = false;
#ifdef HINDSIGHT_CLI
  long long cli_ns = 0;
  bool input_stop = false;
  bool next = false;
  while (!(stop && input_stop)) {
#else
  while (!stop) {
#endif
    pthread_mutex_lock(&at->cp_lock);
    stop = at->stop;
    sample = at->sample;
    pthread_mutex_unlock(&at->cp_lock);

    if (at->input.fh) {
      if (lsb_find_heka_message(&msg, &at->input.ib, true, &discarded_bytes,
                                &logger)) {
        at->msg = &msg;
#ifdef HINDSIGHT_CLI
        if (at->msg->timestamp > cli_ns) {
          cli_ns = at->msg->timestamp;
          at->current_t = cli_ns / 1000000000LL;
        }
#else
        at->current_t = time(NULL);
#endif
        analyze_message(at, sample);

        // advance the checkpoint
        pthread_mutex_lock(&at->cp_lock);
        ++at->mm_delta_cnt;
        at->cp.id = at->input.cp.id;
        at->cp.offset = at->input.cp.offset -
            (at->input.ib.readpos - at->input.ib.scanpos);
        if (sample) at->sample = false;
        pthread_mutex_unlock(&at->cp_lock);
      } else {
        bytes_read = hs_read_file(&at->input);
        // When the read gets to the end it will always check once for the next
        // available file just incase the output_size was increased on the last
        // restart.
#ifdef HINDSIGHT_CLI
        next = false;
        if (!bytes_read
            && (at->input.cp.offset >= at->plugins->cfg->output_size)) {
          next = hs_open_file(&at->input, hs_input_dir, at->input.cp.id + 1);
        }
        if (!bytes_read && !next && stop) {
          input_stop = true;
        }
#else
        if (!bytes_read
            && (at->input.cp.offset >= at->plugins->cfg->output_size)) {
          hs_open_file(&at->input, hs_input_dir, at->input.cp.id + 1);
        }
#endif
      }
    } else { // still waiting on the first file
#ifdef HINDSIGHT_CLI
      next = hs_open_file(&at->input, hs_input_dir, at->input.cp.id);
      if (!next && stop) {
        input_stop = true;
      }
#else
      hs_open_file(&at->input, hs_input_dir, at->input.cp.id);
#endif
    }

    if (bytes_read || at->msg) {
      at->msg = NULL;
    } else {
      // trigger any pending timer events
      lsb_clear_heka_message(&msg); // create an idle/empty message
      at->msg = &msg;
#ifdef HINDSIGHT_CLI
      at->current_t = cli_ns / 1000000000LL;
#else
      at->current_t = time(NULL);
#endif
      analyze_message(at, sample);
      if (sample) {
        pthread_mutex_lock(&at->cp_lock);
        at->sample = false;
        pthread_mutex_unlock(&at->cp_lock);
      }
      at->msg = NULL;
      sleep(1);
    }
  }
  shutdown_timer_event(at);
  lsb_free_heka_message(&msg);
  hs_log(NULL, g_module, 6, "exiting thread: %d", at->tid);
  pthread_exit(NULL);
}


void hs_init_analysis_plugins(hs_analysis_plugins *plugins,
                              hs_config *cfg,
                              hs_checkpoint_reader *cpr)

{
  hs_init_output(&plugins->output, cfg->output_path, hs_analysis_dir);

  plugins->thread_cnt = cfg->analysis_threads;
  plugins->cfg = cfg;
  plugins->cpr = cpr;

#ifdef HINDSIGHT_CLI
  plugins->terminated = false;
#endif

  plugins->list = malloc(sizeof(hs_analysis_thread) * cfg->analysis_threads);
  for (unsigned i = 0; i < cfg->analysis_threads; ++i) {
    init_analysis_thread(plugins, i);
  }
  plugins->threads = malloc(sizeof(pthread_t *) * (cfg->analysis_threads));
}


void hs_stop_analysis_plugins(hs_analysis_plugins *plugins)
{
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    hs_analysis_thread *at = &plugins->list[i];
    pthread_mutex_lock(&at->cp_lock);
    at->stop = true;
    pthread_mutex_unlock(&at->cp_lock);
  }
}


void hs_wait_analysis_plugins(hs_analysis_plugins *plugins)
{
  void *thread_result;
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    if (pthread_join(plugins->threads[i], &thread_result)) {
      hs_log(NULL, g_module, 3, "thread could not be joined");
    }
#ifdef HINDSIGHT_CLI
    if (plugins->list[i].terminated) {
      plugins->terminated = true;
    }
#endif
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
}


static void process_lua(hs_analysis_plugins *plugins, const char *name)
{
  hs_config *cfg = plugins->cfg;
  const char *lpath = cfg->load_path_analysis;
  const char *rpath = cfg->run_path_analysis;
  char lua_lpath[HS_MAX_PATH];
  char lua_rpath[HS_MAX_PATH];
  char cfg_lpath[HS_MAX_PATH];
  char cfg_rpath[HS_MAX_PATH];
  size_t tlen = strlen(hs_analysis_dir) + 1;

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
    pthread_mutex_unlock(&at->list_lock);
  }
}


static unsigned get_tid(const char *path, const char *name)
{
  unsigned tid = UINT_MAX;
  char fqfn[HS_MAX_PATH];
  if (hs_get_fqfn(path, name, fqfn, sizeof(fqfn))) return tid;

  lua_State *L = luaL_newstate();
  if (!L) return tid;

  if (!luaL_dofile(L, fqfn)) {
    lua_getglobal(L, "thread");
    if (lua_type(L, -1) == LUA_TNUMBER) {
      tid = (unsigned)lua_tointeger(L, -1);
    }
  }
  lua_close(L);
  return tid;
}


static unsigned get_previous_tid(const char *opath, const char *name)
{
  size_t len = strlen(hs_analysis_dir) + strlen(name) + 1;
  char rtc[len + 1];
  snprintf(rtc, sizeof(rtc), "%s.%s", hs_analysis_dir, name);
  strcpy(rtc + len - HS_EXT_LEN, hs_rtc_ext);
  return get_tid(opath, rtc);
}


void hs_load_analysis_startup(hs_analysis_plugins *plugins)
{
  const int threads = plugins->thread_cnt;
  int plugins_per_thread[threads];
  memset(plugins_per_thread, 0, sizeof(int) * threads);

  hs_config *cfg = plugins->cfg;
  const char *dir = cfg->run_path_analysis;
  DIR *dp = opendir(dir);
  if (dp == NULL) {
    hs_log(NULL, g_module, 0, "%s: %s", dir, strerror(errno));
    exit(EXIT_FAILURE);
  }

  struct dirent *entry;
  // todo remove this code after the migration
  // migrate any existing rtc files to the new location
  while ((entry = readdir(dp))) {
    if ((hs_has_ext(entry->d_name, hs_rtc_ext))) {
      char ofn[HS_MAX_PATH];
      char nfn[HS_MAX_PATH];
      if (hs_get_fqfn(dir, entry->d_name, ofn, sizeof(ofn))) {
        hs_log(NULL, g_module, 0, "path too long");
        exit(EXIT_FAILURE);
      }
      int rv = snprintf(nfn, sizeof(nfn), "%s/%s.%s", cfg->output_path,
                        hs_analysis_dir, entry->d_name);
      if (rv < 0 || rv > (int)(sizeof(nfn) - 1)) {
        hs_log(NULL, g_module, 0, "path too long");
        exit(EXIT_FAILURE);
      }
      if (rename(ofn, nfn) != 0) {
        hs_log(NULL, g_module, 0, "rename failed %s %s", strerror(errno), ofn);
        exit(EXIT_FAILURE);
      }
    }
  }
  rewinddir(dp);
  // end todo

  // pre count the provisioned threads to get better distribution of the
  // dynamically provisioned threads
  while ((entry = readdir(dp))) {
    if ((hs_has_ext(entry->d_name, hs_cfg_ext))) {
      unsigned tid = get_tid(dir, entry->d_name);
      if (tid == UINT_MAX) {
        tid = get_previous_tid(cfg->output_path, entry->d_name);
      }
      if (tid != UINT_MAX) {
        ++plugins_per_thread[tid % threads];
      }
    }
  }
  rewinddir(dp);

  while ((entry = readdir(dp))) {
    hs_sandbox_config sbc;
    if (hs_load_sandbox_config(dir, entry->d_name, &sbc, &cfg->apd, 'a')) {
      if (sbc.thread == UINT_MAX) {
        sbc.thread = get_previous_tid(cfg->output_path, entry->d_name);
        if (sbc.thread == UINT_MAX) {
          int min_cnt = INT_MAX;
          for (int i = 0; i < threads; ++i) {
            if (plugins_per_thread[i] == 0) {
              sbc.thread = i;
              break;
            }
            if (plugins_per_thread[i] < min_cnt) {
              min_cnt = plugins_per_thread[i];
              sbc.thread = i;
            }
          }
          ++plugins_per_thread[sbc.thread % threads];
        }
      }
      hs_analysis_plugin *p = create_analysis_plugin(cfg, &sbc);
      if (p) {
        add_to_analysis_plugins(&sbc, plugins, p);
      } else {
#ifdef HINDSIGHT_CLI
        plugins->terminated = true;
#endif
        hs_log(NULL, g_module, 3, "%s create_analysis_plugin failed",
               sbc.cfg_name);
      }
      hs_free_sandbox_config(&sbc);
    }
  }
  closedir(dp);
}


static unsigned least_used_thread_id(hs_analysis_plugins *plugins)
{
  unsigned tid  = 0;
  int min_util  = INT_MAX;
  int min_cnt   = INT_MAX;

  for (int i = 0; i < plugins->thread_cnt; ++i) {
    hs_analysis_thread *at = &plugins->list[i];
    pthread_mutex_lock(&at->list_lock);
    if (at->utilization < min_util ||
        (at->utilization == min_util && at->list_cnt < min_cnt)) {
      min_util = at->utilization;
      min_cnt = at->list_cnt;
      tid = (unsigned)i;
    }
    pthread_mutex_unlock(&at->list_lock);
  }
  return tid;
}


static bool ext_exists(const char *dir, const char *name, const char *ext)
{
  char path[HS_MAX_PATH];
  if (hs_get_fqfn(dir, name, path, sizeof(path))) {
    hs_log(NULL, g_module, 0, "path too long");
    exit(EXIT_FAILURE);
  }
  size_t pos = strlen(path) - HS_EXT_LEN;
  strcpy(path + pos, ext);
  return hs_file_exists(path);
}


static int get_thread_id(hs_config *cfg, const char *name, unsigned *tid)
{
  if (hs_has_ext(name, hs_cfg_ext)) {
    *tid = get_tid(cfg->load_path_analysis, name);
    unsigned otid = *tid;
    if (!ext_exists(cfg->run_path_analysis, name, hs_off_ext)
        && !ext_exists(cfg->run_path_analysis, name, hs_err_ext)) {
      otid = get_previous_tid(cfg->output_path, name);
      if (*tid == UINT_MAX) {
        *tid = otid;
      }
    }

    if (otid != *tid) { // mis-matched cfgs so remove the load .cfg
      char path[HS_MAX_PATH];
      if (hs_get_fqfn(cfg->load_path_analysis, name, path, sizeof(path))) {
        hs_log(NULL, g_module, 0, "load off path too long");
        exit(EXIT_FAILURE);
      }
      if (unlink(path)) {
        hs_log(NULL, g_module, 3, "failed to delete: %s errno: %d", path,
               errno);
      }
      hs_log(NULL, g_module, 3, "plugin cannot be restarted on a different "
             "thread: %s", name);
      return false;
    }
    return true;
  } else if (hs_has_ext(name, hs_off_ext)) {
    char path[HS_MAX_PATH];
    strcpy(path, name);
    strcpy(path + strlen(name) - HS_EXT_LEN, hs_cfg_ext);
    *tid = get_previous_tid(cfg->output_path, path);
    if (*tid != UINT_MAX) {
      return true;
    }

    // no .rtc was found so remove the .off flag
    if (hs_get_fqfn(cfg->load_path_analysis, name, path, sizeof(path))) {
      hs_log(NULL, g_module, 0, "load off path too long");
      exit(EXIT_FAILURE);
    }
    if (unlink(path)) {
      hs_log(NULL, g_module, 3, "failed to delete: %s errno: %d", path, errno);
    }
  }
  return false;
}


void hs_load_analysis_dynamic(hs_analysis_plugins *plugins, const char *name)
{
  hs_config *cfg = plugins->cfg;
  const char *lpath = cfg->load_path_analysis;
  const char *rpath = cfg->run_path_analysis;

  if (hs_has_ext(name, hs_lua_ext)) {
    process_lua(plugins, name);
    return;
  }

  unsigned tid = UINT_MAX;
  if (!get_thread_id(cfg, name, &tid)) {
    hs_log(NULL, g_module, 7, "%s ignored %s", __func__, name);
    return;
  }

  bool dynamic = tid == UINT_MAX;
  if (dynamic) {
    tid  = least_used_thread_id(plugins);
  }
  int tidx = tid % plugins->thread_cnt;

  switch (hs_process_load_cfg(lpath, rpath, name)) {
  case 0: // remove
    remove_from_analysis_plugins(&plugins->list[tidx], name);
    break;
  case 1: // load
    {
      if (!dynamic) {
        remove_from_analysis_plugins(&plugins->list[tidx], name);
      }
      hs_sandbox_config sbc;
      if (hs_load_sandbox_config(rpath, name, &sbc, &cfg->apd, 'a')) {
        if (sbc.thread == UINT_MAX) {
          sbc.thread = tid;
        }
        hs_analysis_plugin *p = create_analysis_plugin(cfg, &sbc);
        if (p) {
          add_to_analysis_plugins(&sbc, plugins, p);
        } else {
#ifdef HINDSIGHT_CLI
          plugins->terminated = true;
#endif
          hs_log(NULL, g_module, 3, "%s create_analysis_plugin failed",
                 sbc.cfg_name);
        }
        hs_free_sandbox_config(&sbc);
      }
    }
    break;
  default:
    hs_log(NULL, g_module, 7, "%s ignored %s", __func__, name);
    break;
  }
}


void hs_start_analysis_threads(hs_analysis_plugins *plugins)
{
  for (int i = 0; i < plugins->thread_cnt; ++i) {
    hs_analysis_thread *at = &plugins->list[i];
    hs_lookup_input_checkpoint(at->plugins->cpr,
                               hs_input_dir,
                               at->input.name,
                               at->plugins->cfg->output_path,
                               &at->input.cp);
    at->cp.id = at->input.cp.id;
    at->cp.offset = at->input.cp.offset;
    if (pthread_create(&plugins->threads[i], NULL, input_thread,
                       (void *)at)) {
      perror("hs_start_analysis_threads pthread_create failed");
      exit(EXIT_FAILURE);
    }
  }
}
