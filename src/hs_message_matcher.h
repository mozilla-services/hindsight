/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight Heka message matcher @file */

#ifndef hs_message_matcher_h_
#define hs_message_matcher_h_

#include <lua.h>
#include <stdbool.h>

#include "hs_heka_message.h"

typedef struct hs_message_matcher hs_message_matcher;

typedef struct hs_message_match_builder {
  lua_State* parser;
} hs_message_match_builder;


void hs_init_message_match_builder(hs_message_match_builder* mmb,
                                   const char* module_path);
void hs_free_message_match_builder(hs_message_match_builder* mmb);

hs_message_matcher*
hs_create_message_matcher(const hs_message_match_builder* mmb,
                          const char* exp);
void hs_free_message_matcher(hs_message_matcher* mm);
bool hs_eval_message_matcher(hs_message_matcher* mm, hs_heka_message* m);

#endif
