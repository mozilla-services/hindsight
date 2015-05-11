/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight Heka stream reader structures @file */

#ifndef hs_heka_stream_reader_h_
#define hs_heka_stream_reader_h_

#include <luasandbox/lua.h>

#include "hs_input.h"
#include "hs_heka_message.h"

extern const char* mozsvc_heka_stream_reader;

typedef struct heka_stream_reader
{
  hs_heka_message msg;
  hs_input_buffer buf;
} heka_stream_reader;

int luaopen_heka_stream_reader(lua_State* lua);
#endif
