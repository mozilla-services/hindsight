/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Sandbox structures and functions @file */

#ifndef hs_sandbox_h_
#define hs_sandbox_h_

#include "lsb.h"
#include "hs_config.h"

typedef struct hs_sandbox
{
  lua_sandbox* lsb;
  char* filename;
  char* state;
  int ticker_interval;
} hs_sandbox;

hs_sandbox* hs_create_sandbox(void* parent,
                              const char* file,
                              const char* cfg_template,
                              const hs_sandbox_config* cfg,
                              lua_State* env);

void hs_free_sandbox(hs_sandbox* p);

#endif
