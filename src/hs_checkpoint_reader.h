/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight checkpoint reader functions and structures @file */

#ifndef hs_checkpoint_reader_h_
#define hs_checkpoint_reader_h_

#include <lua.h>

#define HS_MAX_PATH 260

typedef struct hs_checkpoint_reader {
  lua_State* values;
} hs_checkpoint_reader;

void hs_init_checkpoint_reader(hs_checkpoint_reader* cp, const char* path);
void hs_free_checkpoint_reader(hs_checkpoint_reader* cp);

void hs_lookup_checkpoint(hs_checkpoint_reader* cp,
                          const char* key, char** s,
                          unsigned* scap);
void hs_lookup_input_checkpoint(hs_checkpoint_reader* cp,
                                const char* key,
                                const char* path,
                                const char* subdir,
                                size_t* id,
                                size_t* offset);

#endif
