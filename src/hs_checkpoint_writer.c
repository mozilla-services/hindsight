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
}


void hs_free_checkpoint_writer(hs_checkpoint_writer* cpw)
{
  if (cpw->fh) fclose(cpw->fh);
  cpw->fh = NULL;
  cpw->analysis_plugins = NULL;
  cpw->input_plugins = NULL;
  cpw->output_plugins = NULL;
}


void hs_write_checkpoints(hs_checkpoint_writer* cpw, hs_checkpoint_reader* cpr)
{
  cpw->fh = freopen(NULL, "wb", cpw->fh);
  if (!cpw->fh) {
    hs_log(g_module, 1, "checkpoint_writer freopen() error: %d",
           ferror(cpw->fh));
    return;
  }
  if (cpw->input_plugins) {
    hs_input_plugin* p;
    pthread_mutex_lock(&cpw->input_plugins->list_lock);
    for (int i = 0; i < cpw->input_plugins->list_cap; ++i) {
      p = cpw->input_plugins->list[i];
      if (p) {
        pthread_mutex_lock(&p->cp.lock);
        hs_update_checkpoint(cpr, p->sb->filename, &p->cp);
        pthread_mutex_unlock(&p->cp.lock);
      }
    }
    pthread_mutex_unlock(&cpw->input_plugins->list_lock);

    pthread_mutex_lock(&cpw->input_plugins->output.lock);
    hs_update_id_checkpoint(cpr, "last_output_id_input",
                            cpw->input_plugins->output.id);
    fflush(cpw->input_plugins->output.fh);
    pthread_mutex_unlock(&cpw->input_plugins->output.lock);
  }

  if (cpw->analysis_plugins) {
    long offset = 0;
    size_t id = 0;
    pthread_mutex_lock(&cpw->analysis_plugins->cp_lock);
    id = cpw->analysis_plugins->cp_id;
    offset = cpw->analysis_plugins->cp_offset;
    pthread_mutex_unlock(&cpw->analysis_plugins->cp_lock);
    hs_update_input_checkpoint(cpr, hs_analysis_dir, hs_input_dir, id, offset);

    pthread_mutex_lock(&cpw->analysis_plugins->output.lock);
    hs_update_id_checkpoint(cpr, "last_output_id_analysis",
                            cpw->analysis_plugins->output.id);
    fflush(cpw->analysis_plugins->output.fh);
    pthread_mutex_unlock(&cpw->analysis_plugins->output.lock);
  }

  if (cpw->output_plugins) {
    pthread_mutex_lock(&cpw->output_plugins->list_lock);
    for (int i = 0; i < cpw->output_plugins->list_cap; ++i) {
      if (cpw->output_plugins->list[i]) {
        pthread_mutex_lock(&cpw->output_plugins->list[i]->cp_lock);
        hs_update_input_checkpoint(cpr,
                                   cpw->output_plugins->list[i]->sb->filename,
                                   hs_input_dir,
                                   cpw->output_plugins->list[i]->cp_id[0],
                                   cpw->output_plugins->list[i]->cp_offset[0]);
        hs_update_input_checkpoint(cpr,
                                   cpw->output_plugins->list[i]->sb->filename,
                                   hs_analysis_dir,
                                   cpw->output_plugins->list[i]->cp_id[1],
                                   cpw->output_plugins->list[i]->cp_offset[1]);
        pthread_mutex_unlock(&cpw->output_plugins->list[i]->cp_lock);
      }
    }
    pthread_mutex_unlock(&cpw->output_plugins->list_lock);
  }
  hs_output_checkpoints(cpr, cpw->fh);
  fflush(cpw->fh);
}
