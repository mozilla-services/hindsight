/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/** Hindsight checkpoint functions and structures @file */

#ifndef hs_checkpoint_writer_h_
#define hs_checkpoint_writer_h_

#include <stdio.h>

#include "hs_analysis_plugins.h"
#include "hs_checkpoint_reader.h"
#include "hs_input_plugins.h"
#include "hs_output_plugins.h"
#include "hs_output.h"

typedef struct hs_checkpoint_writer {
  hs_analysis_plugins *analysis_plugins;
  hs_input_plugins *input_plugins;
  hs_output_plugins *output_plugins;
  char *cp_path;
  char *utsv_path;
  char *ptsv_path;
  char *cp_path_tmp;
  char *utsv_path_tmp;
  char *ptsv_path_tmp;
} hs_checkpoint_writer;

void hs_init_checkpoint_writer(hs_checkpoint_writer *cpw,
                               hs_input_plugins *ip,
                               hs_analysis_plugins *ap,
                               hs_output_plugins *op,
                               const char *path);

void hs_free_checkpoint_writer(hs_checkpoint_writer *cpw);

void hs_write_checkpoints(hs_checkpoint_writer *cpw, hs_checkpoint_reader *cpr);

#endif
