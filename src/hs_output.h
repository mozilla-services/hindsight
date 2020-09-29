/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/** Hindsight output functions and structures @file */

#ifndef hs_output_h_
#define hs_output_h_

#include "hs_checkpoint_reader.h"
#include "hs_config.h"

#include <luasandbox/lua.h>
#include <pthread.h>
#include <stdio.h>

typedef struct hs_output
{
  FILE *fh;
  char *path;
  unsigned long long min_cp_id;
  pthread_mutex_t lock;
  hs_checkpoint cp;
} hs_output;


void hs_init_output(hs_output *output, const char *path, const char *subdir);

void hs_free_output(hs_output *output);

void hs_open_output_file(hs_output *output);

#endif
