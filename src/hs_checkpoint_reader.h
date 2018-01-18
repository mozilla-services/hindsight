/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight checkpoint reader functions and structures @file */

#ifndef hs_checkpoint_reader_h_
#define hs_checkpoint_reader_h_

#include <luasandbox/lua.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define HS_MAX_IP_CHECKPOINT 8192

typedef enum {
  HS_CP_NONE,
  HS_CP_NUMERIC,
  HS_CP_STRING
} hs_ip_checkpoint_type;

typedef struct hs_ip_checkpoint {
  pthread_mutex_t lock;
  hs_ip_checkpoint_type type;
  unsigned len;  // string checkpoint length
  unsigned cap;  // string checkpoint capacity
  union {
    double d; // numeric checkpoint
    char *s;  // string checkpoint
  } value;
} hs_ip_checkpoint;

typedef struct hs_checkpoint {
  unsigned long long id;
  size_t offset;
} hs_checkpoint;

typedef struct hs_checkpoint_pair
{
  hs_checkpoint input;
  hs_checkpoint analysis;
} hs_checkpoint_pair;

typedef struct hs_checkpoint_reader {
  lua_State *values;
  pthread_mutex_t lock;
} hs_checkpoint_reader;

void hs_init_checkpoint_reader(hs_checkpoint_reader *cpr, const char *path);

void hs_free_checkpoint_reader(hs_checkpoint_reader *cpr);

int hs_load_checkpoint(lua_State *L, int idx, hs_ip_checkpoint *cp);

void hs_lookup_checkpoint(hs_checkpoint_reader *cpr,
                          const char *key,
                          hs_ip_checkpoint *cp);

void hs_update_checkpoint(hs_checkpoint_reader *cpr,
                          const char *key,
                          hs_ip_checkpoint *cp);

void hs_lookup_input_checkpoint(hs_checkpoint_reader *cpr,
                                const char *key,
                                const char *path,
                                const char *subdir,
                                hs_checkpoint *cp);

void hs_update_input_checkpoint(hs_checkpoint_reader *cpr,
                                const char *subdir,
                                const char *key,
                                const hs_checkpoint *cp);

int hs_output_checkpoints(hs_checkpoint_reader *cpr, FILE *fh);

void hs_remove_checkpoint(hs_checkpoint_reader *cpr,
                          const char *key);

void hs_cleanup_checkpoints(hs_checkpoint_reader *cpr,
                            const char *run_path,
                            uint8_t analysis_threads);

#endif
