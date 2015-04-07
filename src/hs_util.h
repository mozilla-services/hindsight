/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight utility functions @file */

#ifndef hs_util_h_
#define hs_util_h_

#include <stdbool.h>
#include <stdio.h>

/**
 * Test a file exists and can be opened for reading.
 *
 * @param fn
 *
 * @return bool
 */
bool hs_file_exists(const char* fn);


/**
 * Constructs a fully qualified filename from the provided components
 *
 * @param path Base path
 * @param name File name
 * @param fqfn Buffer to construct the string in
 * @param fqfn_len Length of the buffer
 *
 * @return bool true if string was successfully constructed
 */
bool hs_get_fqfn(const char* path,
                 const char* name,
                 char* fqfn,
                 size_t fqfn_len);

void hs_output_lua_string(FILE* fh, const char* s);

int hs_write_varint(char* buf, unsigned long long i);

unsigned const char*
hs_read_varint(unsigned const char* p, unsigned const char* e, long long* vi);

#endif
