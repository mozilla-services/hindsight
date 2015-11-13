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


void hs_init_checkpoint_writer(hs_checkpoint_writer* cpw,
                               hs_input_plugins* ip,
                               hs_analysis_plugins* ap,
                               hs_output_plugins* op,
                               const char* path)
{
  cpw->input_plugins = ip;
  cpw->analysis_plugins = ap;
  cpw->output_plugins = op;

  char fqfn[HS_MAX_PATH];
  if (!hs_get_fqfn(path, "hindsight.cp", fqfn, sizeof(fqfn))) {
    hs_log(g_module, 0, "checkpoint name exceeds the max length: %d",
           sizeof(fqfn));
    exit(EXIT_FAILURE);
  }

  cpw->fh = fopen(fqfn, "wb");
  if (!cpw->fh) {
    hs_log(g_module, 0, "%s: %s", fqfn, strerror(errno));
    exit(EXIT_FAILURE);
  }

  if (!hs_get_fqfn(path, "hindsight.tsv", fqfn, sizeof(fqfn))) {
    hs_log(g_module, 0, "tsv name exceeds the max length: %d",
           sizeof(fqfn));
    exit(EXIT_FAILURE);
  }
  cpw->tsv_path = malloc(strlen(fqfn) + 1);
  if (!cpw->tsv_path) {
    hs_log(g_module, 0, "tsv_path malloc failed");
    exit(EXIT_FAILURE);
  }
  strcpy(cpw->tsv_path, fqfn);
}


void hs_free_checkpoint_writer(hs_checkpoint_writer* cpw)
{
  if (cpw->fh) fclose(cpw->fh);
  cpw->fh = NULL;
  cpw->analysis_plugins = NULL;
  cpw->input_plugins = NULL;
  cpw->output_plugins = NULL;
  free(cpw->tsv_path);
  cpw->tsv_path = NULL;
}


void hs_write_checkpoints(hs_checkpoint_writer* cpw, hs_checkpoint_reader* cpr)
{
  static int cnt = 0;

  FILE* tsv = NULL;
  bool sample = (cnt % 6 == 0); // sample performance 10 times a minute
  if (cnt == 0) { // write the stats once a minute just after the load
    tsv = fopen(cpw->tsv_path, "wb");
    if (tsv) {
      fprintf(tsv, "Plugin\t"
              "Inject Message Count\tInject Message Bytes\t"
              "Process Message Count\tProcess Message Failures\t"
              "Current Memory\t"
              "Max Memory\tMax Output\tMax Instructions\t"
              "Message Matcher Avg (s)\tMessage Matcher SD (s)\t"
              "Process Message Avg (s)\tProcess Message SD (s)\t"
              "Timer Event Avg (s)\tTimer Event SD (s)\n");
    }
  }
  if (cpw->input_plugins) {
    hs_input_plugin* p;
    pthread_mutex_lock(&cpw->input_plugins->list_lock);
    for (int i = 0; i < cpw->input_plugins->list_cap; ++i) {
      p = cpw->input_plugins->list[i];
      if (p) {
        pthread_mutex_lock(&p->cp.lock);
        hs_update_checkpoint(cpr, p->sb->name, &p->cp);
        pthread_mutex_unlock(&p->cp.lock);

        if (tsv) {
          pthread_mutex_lock(&cpw->input_plugins->output.lock);
          fprintf(tsv, "%s\t"
                  "%zu\t%zu\t"
                  "%zu\t%zu\t"
                  "%u\t"
                  "%u\t%u\t%u\t"
                  "0\t0\t"
                  "0\t0\t"
                  "0\t0\t\n",
                  p->sb->name,
                  p->sb->stats.im_cnt, p->sb->stats.im_bytes,
                  p->sb->stats.pm_cnt, p->sb->stats.pm_failures,
                  p->sb->stats.cur_memory,
                  p->sb->stats.max_memory,
                  p->sb->stats.max_output,
                  p->sb->stats.max_instructions);
          pthread_mutex_unlock(&cpw->input_plugins->output.lock);
        }
      }
    }
    pthread_mutex_unlock(&cpw->input_plugins->list_lock);

    pthread_mutex_lock(&cpw->input_plugins->output.lock);
    fflush(cpw->input_plugins->output.fh);
    pthread_mutex_unlock(&cpw->input_plugins->output.lock);
  }

  if (cpw->analysis_plugins) {
    hs_checkpoint cp;

    for (int i = 0; i < cpw->analysis_plugins->thread_cnt; ++i) {
      hs_analysis_thread* at = &cpw->analysis_plugins->list[i];
      pthread_mutex_lock(&at->cp_lock);
      cp = at->cp;
      pthread_mutex_unlock(&at->cp_lock);
      hs_update_input_checkpoint(cpr, hs_input_dir, at->input.name, &cp);

      pthread_mutex_lock(&cpw->analysis_plugins->output.lock);
      cpw->analysis_plugins->sample = sample;
      fflush(cpw->analysis_plugins->output.fh);
      pthread_mutex_unlock(&cpw->analysis_plugins->output.lock);

      if (tsv) {
        hs_analysis_plugin* p;
        pthread_mutex_lock(&at->list_lock);
        for (int i = 0; i < at->list_cap; ++i) {
          p = at->list[i];
          if (!p) continue;
          fprintf(tsv, "%s\t"
                  "%zu\t%zu\t"
                  "%zu\t%zu\t"
                  "%zu\t%zu\t%zu\t%zu\t"
                  "%g\t%g\t"
                  "%g\t%g\t"
                  "%g\t%g\t\n",
                  p->sb->name,
                  p->sb->stats.im_cnt, p->sb->stats.im_bytes,
                  p->sb->stats.pm_cnt, p->sb->stats.pm_failures,
                  // the sandbox is not in use here, it is safe to grab the
                  // values directly
                  lsb_usage(p->sb->lsb, LSB_UT_MEMORY, LSB_US_CURRENT),
                  lsb_usage(p->sb->lsb, LSB_UT_MEMORY, LSB_US_MAXIMUM),
                  lsb_usage(p->sb->lsb, LSB_UT_OUTPUT, LSB_US_MAXIMUM),
                  lsb_usage(p->sb->lsb, LSB_UT_INSTRUCTION, LSB_US_MAXIMUM),
                  p->sb->stats.mm.mean, hs_sd_running_stats(&p->sb->stats.mm),
                  p->sb->stats.pm.mean, hs_sd_running_stats(&p->sb->stats.pm),
                  p->sb->stats.te.mean, hs_sd_running_stats(&p->sb->stats.te));
        }
        pthread_mutex_unlock(&at->list_lock);
      }
    }
  }

  if (cpw->output_plugins) {
    pthread_mutex_lock(&cpw->output_plugins->list_lock);
    for (int i = 0; i < cpw->output_plugins->list_cap; ++i) {
      hs_output_plugin* p = cpw->output_plugins->list[i];
      if (!p) continue;

      pthread_mutex_lock(&p->cp_lock);
      p->sample = sample;
      hs_update_input_checkpoint(cpr,
                                 hs_input_dir,
                                 p->sb->name,
                                 &p->cp.input);
      hs_update_input_checkpoint(cpr,
                                 hs_analysis_dir,
                                 p->sb->name,
                                 &p->cp.analysis);

      if (tsv) {
        fprintf(tsv, "%s\t"
                "%zu\t%zu\t"
                "%zu\t%zu\t"
                "%u\t"
                "%u\t%u\t%u\t"
                "%g\t%g\t"
                "%g\t%g\t"
                "%g\t%g\t\n",
                p->sb->name,
                p->sb->stats.im_cnt, p->sb->stats.im_bytes,
                p->sb->stats.pm_cnt, p->sb->stats.pm_failures,
                p->sb->stats.cur_memory,
                p->sb->stats.max_memory,
                p->sb->stats.max_output,
                p->sb->stats.max_instructions,
                p->sb->stats.mm.mean, hs_sd_running_stats(&p->sb->stats.mm),
                p->sb->stats.pm.mean, hs_sd_running_stats(&p->sb->stats.pm),
                p->sb->stats.te.mean, hs_sd_running_stats(&p->sb->stats.te));
      }

      pthread_mutex_unlock(&p->cp_lock);
    }
    pthread_mutex_unlock(&cpw->output_plugins->list_lock);
  }
  if (tsv) fclose(tsv);
  if (++cnt == 60) cnt = 0;

  cpw->fh = freopen(NULL, "wb", cpw->fh);
  if (!cpw->fh) {
    hs_log(g_module, 1, "checkpoint_writer freopen() error: %d",
           ferror(cpw->fh));
    return;
  }
  hs_output_checkpoints(cpr, cpw->fh);
  fflush(cpw->fh);
}
