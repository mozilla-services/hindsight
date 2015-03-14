/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight sandbox loader @file */

#ifndef hindsight_sandbox_loader_h_
#define hindsight_sandbox_loader_h_

#include "lsb.h"

#include "hindsight.h"
#include "hindsight_config.h"

/**
 * Loads the sandboxes from the specified path
 *
 * @param path
 * @param cfg
 * @param plugins
 */
void hs_load_sandboxes(const char* path, const hindsight_config* cfg,
                       hs_plugins* plugins);

/**
 * Sends a stop signal to long running (input) sandboxes
 *
 * @param lsb
 */
void hs_stop_sandbox(lua_sandbox* lsb);

#endif
