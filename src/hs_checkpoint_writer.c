/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight checkpoint_writer implementation @file */

#include "hs_checkpoint_writer.h"

#include <errno.h>
#include <luasandbox/lauxlib.h>
#include <stdlib.h>
#include <string.h>

#include "hs_analysis_plugins.h"
#include "hs_input_plugins.h"
#include "hs_logger.h"
#include "hs_output.h"
#include "hs_output_plugins.h"
#include "hs_util.h"

static const char g_module[] = "checkpoint_writer";


static void
allocate_filename(const char *path, const char *name, char **filename)
{
  char fqfn[HS_MAX_PATH];
  if (hs_get_fqfn(path, name, fqfn, sizeof(fqfn))) {
    hs_log(NULL, g_module, 0, "%s/%s exceeds the max length: %d", path, name,
           sizeof(fqfn));
    exit(EXIT_FAILURE);
  }
  *filename = malloc(strlen(fqfn) + 1);
  if (!*filename) {
    hs_log(NULL, g_module, 0, "%s/%s malloc failed", path, name);
    exit(EXIT_FAILURE);
  }
  strcpy(*filename, fqfn);
}


void hs_init_checkpoint_writer(hs_checkpoint_writer *cpw,
                               hs_input_plugins *ip,
                               hs_analysis_plugins *ap,
                               hs_output_plugins *op,
                               const char *path)
{
  cpw->input_plugins = ip;
  cpw->analysis_plugins = ap;
  cpw->output_plugins = op;
  allocate_filename(path, "hindsight.cp", &cpw->cp_path);
  allocate_filename(path, "hindsight.cp.tmp", &cpw->cp_path_tmp);
  allocate_filename(path, "hindsight.tsv", &cpw->tsv_path);
  allocate_filename(path, "hindsight.tsv.tmp", &cpw->tsv_path_tmp);
}


void hs_free_checkpoint_writer(hs_checkpoint_writer *cpw)
{
  cpw->analysis_plugins = NULL;
  cpw->input_plugins = NULL;
  cpw->output_plugins = NULL;
  free(cpw->cp_path);
  cpw->cp_path = NULL;
  free(cpw->tsv_path);
  cpw->tsv_path = NULL;
  free(cpw->cp_path_tmp);
  cpw->cp_path_tmp = NULL;
  free(cpw->tsv_path_tmp);
  cpw->tsv_path_tmp = NULL;
}


void hs_write_checkpoints(hs_checkpoint_writer *cpw, hs_checkpoint_reader *cpr)
{
  static int cnt = 0;
  static bool sample = false;
  unsigned long long min_input_id = ULLONG_MAX, min_analysis_id = ULLONG_MAX;

  FILE *tsv = NULL; // any stat write failures are non critical and will be
                    // ignored
  if (sample) { // write the stats after the sample
    tsv = fopen(cpw->tsv_path_tmp, "we");
    if (tsv) {
      fprintf(tsv, "Plugin\t"
              "Inject Message Count\tInject Message Bytes\t"
              "Process Message Count\tProcess Message Failures\t"
              "Current Memory\t"
              "Max Memory\tMax Output\tMax Instructions\t"
              "Message Matcher Avg (ns)\tMessage Matcher SD (ns)\t"
              "Process Message Avg (ns)\tProcess Message SD (ns)\t"
              "Timer Event Avg (ns)\tTimer Event SD (ns)\n");
    }
  }
  sample = (cnt % 6 == 0); // sample performance 10 times a minute
  if (cpw->input_plugins) {
    hs_input_plugin *p;
    pthread_mutex_lock(&cpw->input_plugins->list_lock);
    for (int i = 0; i < cpw->input_plugins->list_cap; ++i) {
      p = cpw->input_plugins->list[i];
      if (p) {
        hs_update_checkpoint(cpr, p->name, &p->cp);
        pthread_mutex_lock(&p->cp.lock);
        if (!p->sample) p->sample = sample;
        if (tsv) {
          fprintf(tsv, "%s\t"
                  "%llu\t%llu\t"
                  "%llu\t%llu\t"
                  "%llu\t%llu\t%llu\t%llu\t"
                  "0\t0\t"
                  "%.0f\t%.0f\t"
                  "%.0f\t%.0f\n",
                  p->name,
                  p->stats.im_cnt, p->stats.im_bytes,
                  p->stats.pm_cnt, p->stats.pm_failures,
                  p->stats.mem_cur, p->stats.mem_max,
                  p->stats.out_max, p->stats.ins_max,
                  // no message matcher p->stats
                  p->stats.pm_avg, p->stats.pm_sd,
                  p->stats.te_avg, p->stats.te_sd);
        }
        pthread_mutex_unlock(&p->cp.lock);
      }
    }
    pthread_mutex_unlock(&cpw->input_plugins->list_lock);

    pthread_mutex_lock(&cpw->input_plugins->output.lock);
    if (fflush(cpw->input_plugins->output.fh)) {
      hs_log(NULL, g_module, 0, "input queue fflush failed");
      exit(EXIT_FAILURE);
    }
    pthread_mutex_unlock(&cpw->input_plugins->output.lock);
  }

  if (cpw->analysis_plugins) {
    hs_checkpoint cp;

    for (int i = 0; i < cpw->analysis_plugins->thread_cnt; ++i) {
      hs_analysis_thread *at = &cpw->analysis_plugins->list[i];
      pthread_mutex_lock(&at->cp_lock);
      cp = at->cp;
      if (!at->sample) {
        at->sample = sample;
      }
      pthread_mutex_unlock(&at->cp_lock);
      if (cp.id < min_input_id) {
        min_input_id = cp.id;
      }
      hs_update_input_checkpoint(cpr, hs_input_dir, at->input.name, &cp);

      pthread_mutex_lock(&cpw->analysis_plugins->output.lock);
      if (fflush(cpw->analysis_plugins->output.fh)) {
        hs_log(NULL, g_module, 0, "analysis queue fflush failed");
        exit(EXIT_FAILURE);
      }
      pthread_mutex_unlock(&cpw->analysis_plugins->output.lock);

      if (tsv) {
        hs_analysis_plugin *p;
        pthread_mutex_lock(&at->list_lock);
        for (int i = 0; i < at->list_cap; ++i) {
          p = at->list[i];
          if (!p) continue;

          fprintf(tsv, "%s\t"
                  "%llu\t%llu\t"
                  "%llu\t%llu\t"
                  "%llu\t%llu\t%llu\t%llu\t"
                  "%.0f\t%.0f\t"
                  "%.0f\t%.0f\t"
                  "%.0f\t%.0f\n",
                  p->name,
                  p->stats.im_cnt, p->stats.im_bytes,
                  p->stats.pm_cnt, p->stats.pm_failures,
                  p->stats.mem_cur, p->stats.mem_max,
                  p->stats.out_max, p->stats.ins_max,
                  p->mms.mean, lsb_sd_running_stats(&p->mms),
                  p->stats.pm_avg, p->stats.pm_sd,
                  p->stats.te_avg, p->stats.te_sd);
        }
        pthread_mutex_unlock(&at->list_lock);
      }
    }
  }

  if (cpw->output_plugins) {
    pthread_mutex_lock(&cpw->output_plugins->list_lock);
    for (int i = 0; i < cpw->output_plugins->list_cap; ++i) {
      hs_output_plugin *p = cpw->output_plugins->list[i];
      if (!p) continue;

      pthread_mutex_lock(&p->cp_lock);
      // use the current read checkpoints to prevent batching from causing
      // backpressure
      if (p->cur.input.id < min_input_id) {
        min_input_id = p->cur.input.id;
      }
      if (p->cur.analysis.id < min_analysis_id) {
        min_analysis_id = p->cur.analysis.id;
      }
      if (!p->sample) p->sample = sample;
      hs_update_input_checkpoint(cpr,
                                 hs_input_dir,
                                 p->name,
                                 &p->cp.input);
      hs_update_input_checkpoint(cpr,
                                 hs_analysis_dir,
                                 p->name,
                                 &p->cp.analysis);
      if (tsv) {
        fprintf(tsv, "%s\t"
                "%llu\t%llu\t"
                "%llu\t%llu\t"
                "%llu\t%llu\t%llu\t%llu\t"
                "%.0f\t%.0f\t"
                "%.0f\t%.0f\t"
                "%.0f\t%.0f\n",
                p->name,
                p->stats.im_cnt, p->stats.im_bytes,
                p->stats.pm_cnt, p->stats.pm_failures,
                p->stats.mem_cur, p->stats.mem_max,
                p->stats.out_max, p->stats.ins_max,
                p->mms.mean, lsb_sd_running_stats(&p->mms),
                p->stats.pm_avg, p->stats.pm_sd,
                p->stats.te_avg, p->stats.te_sd);
      }
      pthread_mutex_unlock(&p->cp_lock);
    }
    pthread_mutex_unlock(&cpw->output_plugins->list_lock);
  }
  if (tsv) {
    if (!fclose(tsv)) rename(cpw->tsv_path_tmp, cpw->tsv_path);
  }
  if (++cnt == 60) cnt = 0;

  if (cpw->input_plugins) {
    cpw->input_plugins->output.min_cp_id = min_input_id;
  }

  if (cpw->analysis_plugins) {
    cpw->analysis_plugins->output.min_cp_id = min_analysis_id;
  }

  FILE *cp = fopen(cpw->cp_path_tmp, "we");
  if (!cp) {
    hs_log(NULL, g_module, 0, "%s: %s", cpw->cp_path_tmp, strerror(errno));
    exit(EXIT_FAILURE);
  }
  int rv = hs_output_checkpoints(cpr, cp);
  if (fclose(cp) || rv) {
    hs_log(NULL, g_module, 0, "checkpoint write failure");
    exit(EXIT_FAILURE);
  } else {
    rename(cpw->cp_path_tmp, cpw->cp_path);
  }
}
