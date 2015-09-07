/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Calculates the running count, sum, mean, variance, and standard deviation
 *  @file */

#ifndef hs_running_stats_h_
#define hs_running_stats_h_

typedef struct hs_running_stats
{
  double count;
  double mean;
  double sum;
} hs_running_stats;

void hs_init_running_stats(hs_running_stats* s);
void hs_update_running_stats(hs_running_stats* s, double d);
double hs_sd_running_stats (hs_running_stats* s);

#endif
