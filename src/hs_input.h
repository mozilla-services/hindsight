/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/** Hindsight input facility @file */

#ifndef hs_input_h_
#define hs_input_h_

#include <luasandbox/util/input_buffer.h>

#include "hs_checkpoint_reader.h"
#include "hs_config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>

typedef struct hs_input
{
  FILE              *fh;
  char              *path;
  char              *name;
  char              *fn;
  size_t            fn_size;
  lsb_input_buffer  ib;
  hs_checkpoint     cp;
} hs_input;


void hs_init_input(hs_input *hsi, size_t max_message_size,
                   const char *path,
                   const char *name);
void hs_free_input(hs_input *hsi);

bool hs_open_file(hs_input *hsi, const char *subdir, unsigned long long id);

size_t hs_read_file(hs_input *hsi);

#endif
