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
#include <stdbool.h>

#define HS_MIN_MSG_SIZE 26
#define HS_MAX_HDR_SIZE 255 + 3
extern int hs_max_msg_size;

typedef struct hs_input_buffer
{
  unsigned char* buf;
  char* name;
  size_t namesize;
  size_t bufsize;
  size_t readpos;
  size_t scanpos;
  size_t msglen;
  size_t max_message_size;
  hs_checkpoint cp;
} hs_input_buffer;

typedef struct hs_input
{
  FILE* fh;
  char* path;
  char* name;
  hs_input_buffer ib;
} hs_input;


void hs_init_input(hs_input* hsi, size_t max_message_size,
                   const char* path,
                   const char* name);
void hs_free_input(hs_input* hsi);

void hs_init_input_buffer(hs_input_buffer* b, size_t max_message_size);
void hs_free_input_buffer(hs_input_buffer* b);
bool hs_expand_input_buffer(hs_input_buffer* b, size_t len);

int hs_open_file(hs_input* hsi, const char* subdir, unsigned long long id);

size_t hs_read_file(hs_input* hsi);

#endif
