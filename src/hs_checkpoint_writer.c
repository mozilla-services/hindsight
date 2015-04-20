/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight checkpoint_writer implementation @file */

#include "hs_checkpoint_writer.h"

#include <errno.h>
#include <lauxlib.h>
#include <stdlib.h>
#include <string.h>

#include "hs_analysis_plugins.h"
#include "hs_input_plugins.h"
#include "hs_logger.h"
#include "hs_output.h"
#include "hs_output_plugins.h"
#include "hs_util.h"

static const char g_module[] = "hs_checkpoint_writer";

void hs_init_checkpoint_writer(hs_checkpoint_writer* cp,
                        hs_input_plugins* ip,
                        hs_analysis_plugins* ap,
                        hs_output_plugins* op,
                        const char* path)
{
  cp->input_plugins = ip;
  cp->analysis_plugins = ap;
  cp->output_plugins = op;

  char fqfn[HS_MAX_PATH];
  if (!hs_get_fqfn(path, "hindsight.cp", fqfn, sizeof(fqfn))) {
    hs_log(g_module, 0, "checkpoint name exceeds the max length: %d",
           sizeof(fqfn));
    exit(EXIT_FAILURE);
  }

  cp->fh = fopen(fqfn, "wb");
  if (!cp->fh) {
    hs_log(g_module, 0, "%s: %s", fqfn, strerror(errno));
    exit(EXIT_FAILURE);
  }
}


void hs_free_checkpoint_writer(hs_checkpoint_writer* cp)
{
  if (cp->fh) fclose(cp->fh);
  cp->fh = NULL;
  cp->analysis_plugins = NULL;
  cp->input_plugins = NULL;
  cp->output_plugins = NULL;
}


void hs_write_checkpoints(hs_checkpoint_writer* cp)
{
  if (fseek(cp->fh, 0, SEEK_SET)) {
    hs_log(g_module, 3, "checkpoint_writer fseek() error: %d", ferror(cp->fh));
    return;
  }
  if (cp->input_plugins) {
    hs_input_plugin* p;
    pthread_mutex_lock(&cp->input_plugins->list_lock);
    for (int i = 0; i < cp->input_plugins->list_cap; ++i) {
      p = cp->input_plugins->list[i];
      if (p) {
        pthread_mutex_lock(&p->cp_lock);
        if (p->cp_string) {
          fprintf(cp->fh, "_G['%s'] = '", p->sb->filename);
          hs_output_lua_string(cp->fh, p->cp_string);
          fwrite("'\n", 2, 1, cp->fh);
        }
        pthread_mutex_unlock(&p->cp_lock);
      }
    }
    pthread_mutex_unlock(&cp->input_plugins->list_lock);

    pthread_mutex_lock(&cp->input_plugins->output.lock);
    fprintf(cp->fh, "last_output_id_input = %zu\n",
            cp->input_plugins->output.id);
    fflush(cp->input_plugins->output.fh);
    pthread_mutex_unlock(&cp->input_plugins->output.lock);
  }

  if (cp->analysis_plugins) {
    long pos = 0;
    size_t id = 0;
    hs_input* hsi = &cp->analysis_plugins->input;
    pthread_mutex_lock(&cp->analysis_plugins->input.lock);
    if (hsi->fh) {
      pos = hsi->offset - (hsi->readpos - hsi->scanpos);
      id = hsi->id;
    }
    pthread_mutex_unlock(&cp->analysis_plugins->input.lock);
    fprintf(cp->fh, "analysis_input = '%zu:%ld'\n", id, pos);

    pthread_mutex_lock(&cp->analysis_plugins->output.lock);
    fprintf(cp->fh, "last_output_id_analysis = %zu\n",
            cp->analysis_plugins->output.id);
    fflush(cp->analysis_plugins->output.fh);
    pthread_mutex_unlock(&cp->analysis_plugins->output.lock);
  }

  if (cp->output_plugins) {
    pthread_mutex_lock(&cp->output_plugins->list_lock);
    for (int i = 0; i < cp->output_plugins->list_cap; ++i) {
      if (cp->output_plugins->list[i]) {
        pthread_mutex_lock(&cp->output_plugins->list[i]->cp_lock);
        fprintf(cp->fh, "_G['%s'] = '%zu:%zu'\n",
                cp->output_plugins->list[i]->sb->filename,
                cp->output_plugins->list[i]->cp_id[0],
                cp->output_plugins->list[i]->cp_offset[0]);
        fprintf(cp->fh, "_G['analysis %s'] = '%zu:%zu'\n",
                cp->output_plugins->list[i]->sb->filename,
                cp->output_plugins->list[i]->cp_id[1],
                cp->output_plugins->list[i]->cp_offset[1]);
        pthread_mutex_unlock(&cp->output_plugins->list[i]->cp_lock);
      }
    }
    pthread_mutex_unlock(&cp->output_plugins->list_lock);
  }
  fflush(cp->fh);
}
