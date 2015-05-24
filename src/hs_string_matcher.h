/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight string pattern matcher @file */

#ifndef hs_string_matcher_h_
#define hs_string_matcher_h_

#include <stdbool.h>
#include <stddef.h>

/**
 * Matches a string using a Lua string match pattern
 *
 * @param s String to match
 * @param slen Length of the string
 * @param p Lua match pattern
 *          http://www.lua.org/manual/5.1/manual.html#pdf-string.match
 *
 * @return bool
 */
bool hs_string_match(const char* s, size_t slen, const char* p);

#endif
