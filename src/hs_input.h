/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight input facility @file */

#ifndef hs_input_h_
#define hs_input_h_

#include "hs_config.h"

#include <pthread.h>
#include <stdio.h>

typedef struct hs_input
{
  char path[230];
  char file[260];
  size_t id;
  size_t offset;
  FILE* fh;
} hs_input;


void hs_init_input(hs_input* input);
void hs_free_input(hs_input* input);

void* hs_read_input_thread(void* arg);

#endif
