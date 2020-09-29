/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/** Hindsight utility functions for sandboxes using OpenSSL @file */

#ifndef hs_sslutil_h_
#define hs_sslutil_h_

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "hs_config.h"

/**
 * Bootstrap OpenSSL threading callbacks
 *
 * @return 0 if callback initialization succeeded
 */
int hs_sslcallback_init();

#endif
