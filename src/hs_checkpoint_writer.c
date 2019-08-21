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
static const int  sample_sec = 6; // sample measurements 10 times a minute

struct checkpoint_info {
  FILE *ptsv;
  FILE *utsv;
  unsigned long long  min_input_id;
  unsigned long long  min_analysis_id;
  hs_checkpoint       cp;
  int                 input_delta_cnt;
  int                 sample_cnt;
  bool                sample;
  bool                tsv_error;
};

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
  allocate_filename(path, "utilization.tsv", &cpw->utsv_path);
  allocate_filename(path, "utilization.tsv.tmp", &cpw->utsv_path_tmp);
  allocate_filename(path, "plugins.tsv", &cpw->ptsv_path);
  allocate_filename(path, "plugins.tsv.tmp", &cpw->ptsv_path_tmp);
}


void hs_free_checkpoint_writer(hs_checkpoint_writer *cpw)
{
  cpw->analysis_plugins = NULL;
  cpw->input_plugins = NULL;
  cpw->output_plugins = NULL;

  free(cpw->cp_path);
  cpw->cp_path = NULL;
  free(cpw->cp_path_tmp);
  cpw->cp_path_tmp = NULL;

  free(cpw->utsv_path);
  cpw->utsv_path = NULL;
  free(cpw->utsv_path_tmp);
  cpw->utsv_path_tmp = NULL;

  free(cpw->ptsv_path);
  cpw->ptsv_path = NULL;
  free(cpw->ptsv_path_tmp);
  cpw->ptsv_path_tmp = NULL;
}


static int round_percentage(long long n, long long d)
{
  if (d == 0) {
    return 0;
  }

  int p = n * 1000 / d;
  int r = p % 10;
  p /= 10;

  if (r > 5 || (r == 5 && p % 2)) {
    ++p;
  }
  return p;
}


static void input_stats(hs_checkpoint_writer *cpw, hs_checkpoint_reader *cpr,
                        struct checkpoint_info *cpi)
{
  hs_input_plugin *p;
  pthread_mutex_lock(&cpw->input_plugins->list_lock);
  for (int i = 0; i < cpw->input_plugins->list_cap; ++i) {
    p = cpw->input_plugins->list[i];
    if (p) {
      hs_update_checkpoint(cpr, p->name, &p->cp);
      pthread_mutex_lock(&p->cp.lock);
      if (!p->sample) p->sample = cpi->sample;
      if (cpi->ptsv && cpi->utsv) {
        fprintf(cpi->ptsv, "%s\t"
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
        fprintf(cpi->utsv, "%s\t%d\t-1\t-1\t-1\t-1\n", p->name,
                p->im_delta_cnt);
        cpi->input_delta_cnt += p->im_delta_cnt;
        p->im_delta_cnt = 0;
      } else if (cpi->tsv_error) {
        p->im_delta_cnt = 0;
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
  cpi->cp = cpw->input_plugins->output.cp;
  pthread_mutex_unlock(&cpw->input_plugins->output.lock);
  hs_update_input_checkpoint(cpr, hs_input_dir, NULL, &cpi->cp);
}


static int get_max_mps(long long tt, int amps, int max_mps)
{
  if (tt && amps) {
    int tmps = tt / amps;
    int emps = tmps ? 1000000000LL / tmps + 1 : 1;
    if (emps > max_mps) {
      max_mps = emps;
    }
    if (amps > max_mps) {
      max_mps = amps;
    }
  }
  return max_mps;
}


static void analysis_stats(hs_checkpoint_writer *cpw, hs_checkpoint_reader *cpr,
                           struct checkpoint_info *cpi)
{
  for (int i = 0; i < cpw->analysis_plugins->thread_cnt; ++i) {
    hs_analysis_thread *at = &cpw->analysis_plugins->list[i];
    pthread_mutex_lock(&at->cp_lock);
    cpi->cp = at->cp;
    if (!at->sample) {
      at->sample = cpi->sample;
    }
    pthread_mutex_unlock(&at->cp_lock);
    if (cpi->cp.id < cpi->min_input_id) {
      cpi->min_input_id = cpi->cp.id;
    }
    hs_update_input_checkpoint(cpr, hs_input_dir, at->input.name, &cpi->cp);

    pthread_mutex_lock(&cpw->analysis_plugins->output.lock);
    if (fflush(cpw->analysis_plugins->output.fh)) {
      hs_log(NULL, g_module, 0, "analysis queue fflush failed");
      exit(EXIT_FAILURE);
    }
    cpi->cp = cpw->analysis_plugins->output.cp;
    pthread_mutex_unlock(&cpw->analysis_plugins->output.lock);
    hs_update_input_checkpoint(cpr, hs_analysis_dir, NULL, &cpi->cp);

    hs_analysis_plugin *p;
    if (cpi->ptsv && cpi->utsv) {
      long long mmt = 0;
      long long pmt = 0;
      long long tet = 0;
      pthread_mutex_lock(&at->list_lock);
      for (int i = 0; i < at->list_cap; ++i) {
        p = at->list[i];
        if (!p) continue;
        mmt += p->mms.mean * at->mm_delta_cnt;
        pmt += p->stats.pm_avg * p->pm_delta_cnt;
        if (p->ticker_interval > 0) {
          tet += p->stats.te_avg * (sample_sec * 1.0 / p->ticker_interval);
        }
      }

      long long tt = mmt + pmt + tet;
      int amps = at->mm_delta_cnt / sample_sec;
      int imps = cpi->input_delta_cnt / sample_sec;
      int mps  = (imps > amps) ? imps : amps;
      at->max_mps = get_max_mps(tt, amps, at->max_mps);
      int utilization = round_percentage(mps, at->max_mps);
      at->utilization = utilization > UINT8_MAX ? UINT8_MAX : utilization;
      fprintf(cpi->utsv, "analysis%d\t%d\t%d\t%d\t%d\t%d\n", i,
              at->mm_delta_cnt,
              at->utilization,
              round_percentage(mmt, tt),
              round_percentage(pmt, tt),
              round_percentage(tet, tt));

      for (int i = 0; i < at->list_cap; ++i) {
        p = at->list[i];
        if (!p) continue;

        fprintf(cpi->ptsv, "%s\t"
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

        long long mmtp = p->mms.mean * at->mm_delta_cnt;
        long long pmtp = p->stats.pm_avg * p->pm_delta_cnt;
        long long tetp = 0;
        if (p->ticker_interval > 0) {
          tetp = p->stats.te_avg * (sample_sec * 1.0 / p->ticker_interval);
        }
        long long ttp = mmtp + pmtp + tetp;
        if (tt == 0 || ttp == 0) {
          fprintf(cpi->utsv, "%s\t0\t0\t0\t0\t0\n", p->name);
        } else {
          fprintf(cpi->utsv, "%s\t%d\t%d\t%d\t%d\t%d\n", p->name,
                  p->pm_delta_cnt,
                  round_percentage(ttp, tt),
                  round_percentage(mmtp, ttp),
                  round_percentage(pmtp, ttp),
                  round_percentage(tetp, ttp));
        }
        p->pm_delta_cnt = 0;
      }
      at->mm_delta_cnt = 0;
      pthread_mutex_unlock(&at->list_lock);
    } else if (cpi->tsv_error) {
      pthread_mutex_lock(&at->list_lock);
      for (int i = 0; i < at->list_cap; ++i) {
        p = at->list[i];
        if (!p) continue;
        p->pm_delta_cnt = 0;
      }
      at->mm_delta_cnt = 0;
      pthread_mutex_unlock(&at->list_lock);
    }
  }
}


static void output_stats(hs_checkpoint_writer *cpw, hs_checkpoint_reader *cpr,
                         struct checkpoint_info *cpi)
{
  pthread_mutex_lock(&cpw->output_plugins->list_lock);
  for (int i = 0; i < cpw->output_plugins->list_cap; ++i) {
    hs_output_plugin *p = cpw->output_plugins->list[i];
    if (!p) continue;

    pthread_mutex_lock(&p->cp_lock);
    // use the current read checkpoints to prevent batching from causing
    // backpressure
    int imps = 0;
    if (!p->sample) p->sample = cpi->sample;
    if (p->read_queue >= 'b') {
      if (p->cur.input.id < cpi->min_input_id) {
        cpi->min_input_id = p->cur.input.id;
      }
      hs_update_input_checkpoint(cpr,
                                 hs_input_dir,
                                 p->name,
                                 &p->cp.input);
      imps = cpi->input_delta_cnt / sample_sec;
    }
    if (p->read_queue <= 'b') {
      if (p->cur.analysis.id < cpi->min_analysis_id) {
        cpi->min_analysis_id = p->cur.analysis.id;
      }
      hs_update_input_checkpoint(cpr,
                                 hs_analysis_dir,
                                 p->name,
                                 &p->cp.analysis);
    }
    if (cpi->ptsv && cpi->utsv) {
      long long mmt = 0;
      long long pmt = 0;
      long long tet = 0;
      fprintf(cpi->ptsv, "%s\t"
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
      mmt = p->mms.mean * p->mm_delta_cnt;
      pmt = p->stats.pm_avg * p->pm_delta_cnt;
      if (p->ticker_interval > 0) {
        tet = p->stats.te_avg * (sample_sec * 1.0 / p->ticker_interval);
      }

      long long tt = mmt + pmt + tet;
      int amps = p->mm_delta_cnt / sample_sec;
      int mps  = (imps > amps) ? imps : amps;
      p->max_mps = get_max_mps(tt, amps, p->max_mps);
      fprintf(cpi->utsv, "%s\t%d\t%d\t%d\t%d\t%d\n", p->name,
              p->pm_delta_cnt,
              round_percentage(mps, p->max_mps),
              round_percentage(mmt, tt),
              round_percentage(pmt, tt),
              round_percentage(tet, tt));
      p->mm_delta_cnt = 0;
      p->pm_delta_cnt = 0;
    } else if (cpi->tsv_error) {
      p->mm_delta_cnt = 0;
      p->pm_delta_cnt = 0;
    }
    pthread_mutex_unlock(&p->cp_lock);
  }
  pthread_mutex_unlock(&cpw->output_plugins->list_lock);
}


void hs_write_checkpoints(hs_checkpoint_writer *cpw, hs_checkpoint_reader *cpr)
{
  static struct checkpoint_info cpi = {
    NULL, NULL, ULLONG_MAX, ULLONG_MAX,{ 0, 0 }, 0, 0, false, false };

  // any stat write failures are non critical and will be ignored
  cpi.utsv = NULL;
  cpi.ptsv = NULL;
  cpi.min_input_id = ULLONG_MAX;
  cpi.min_analysis_id = ULLONG_MAX;
  cpi.input_delta_cnt = 0;
  cpi.tsv_error = false;

  if (cpi.sample) { // write the stats after the sample
    cpi.utsv = fopen(cpw->utsv_path_tmp, "we");
    if (cpi.utsv) {
      fprintf(cpi.utsv, "Plugin\tMessages Processed\t"
              "%% Utilization\t%% Message Matcher\t"
              "%% Process Message\t%% Timer Event"
              "\n");
    }

    cpi.ptsv = fopen(cpw->ptsv_path_tmp, "we");
    if (cpi.ptsv) {
      fprintf(cpi.ptsv, "Plugin\t"
              "Inject Message Count\tInject Message Bytes\t"
              "Process Message Count\tProcess Message Failures\t"
              "Current Memory\t"
              "Max Memory\tMax Output\tMax Instructions\t"
              "Message Matcher Avg (ns)\tMessage Matcher SD (ns)\t"
              "Process Message Avg (ns)\tProcess Message SD (ns)\t"
              "Timer Event Avg (ns)\tTimer Event SD (ns)\n");
    }
    cpi.tsv_error = !(cpi.utsv && cpi.ptsv);
  }
  cpi.sample = (cpi.sample_cnt % sample_sec == 0);
  if (cpw->input_plugins) {
    input_stats(cpw, cpr, &cpi);
  }

  if (cpw->analysis_plugins) {
    analysis_stats(cpw, cpr, &cpi);
  }

  if (cpw->output_plugins) {
    output_stats(cpw, cpr, &cpi);
  }

  if (cpi.ptsv) {
    if (!fclose(cpi.ptsv)) rename(cpw->ptsv_path_tmp, cpw->ptsv_path);
  }
  if (cpi.utsv) {
    if (!fclose(cpi.utsv)) rename(cpw->utsv_path_tmp, cpw->utsv_path);
  }

  if (cpw->input_plugins) {
    cpw->input_plugins->output.min_cp_id = cpi.min_input_id;
  }

  if (cpw->analysis_plugins) {
    cpw->analysis_plugins->output.min_cp_id = cpi.min_analysis_id;
  }

  if (++cpi.sample_cnt == 60) cpi.sample_cnt = 0;

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
