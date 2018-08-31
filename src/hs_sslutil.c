/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight sslutil implementation @file */

#include "hs_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#ifdef WITH_OPENSSL
#include <openssl/crypto.h>

#define OPENSSL_REQUIRES_LOCKS (OPENSSL_VERSION_NUMBER < 0x10100003L)

#if OPENSSL_REQUIRES_LOCKS
static pthread_mutex_t *sslmtxlist;

static unsigned long hs_sslcallback_idfunc()
{
  return ((unsigned long)pthread_self());
}

static void hs_sslcallback_lockfunc(int m, int n, const char *f __attribute__((unused)),
  int line __attribute__((unused)))
{
  if (m & CRYPTO_LOCK)
    pthread_mutex_lock(&sslmtxlist[n]);
  else
    pthread_mutex_unlock(&sslmtxlist[n]);
}

static int hs_sslcallback_initlocks()
{
  int i;

  sslmtxlist = (pthread_mutex_t *)malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));
  if (!sslmtxlist)
    return 1;

  for (i = 0; i < CRYPTO_num_locks(); i++)
    pthread_mutex_init(&(sslmtxlist[i]), NULL);

  CRYPTO_set_id_callback(hs_sslcallback_idfunc);
  CRYPTO_set_locking_callback(hs_sslcallback_lockfunc);
  return 0;
}
#endif


int hs_sslcallback_init()
{
#if OPENSSL_REQUIRES_LOCKS
  if (hs_sslcallback_initlocks())
    return 1;
#endif
  return 0;
}
#endif
