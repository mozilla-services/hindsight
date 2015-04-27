/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight output implementation @file */

#include "hs_output.h"

#include <errno.h>
#include <luasandbox/lauxlib.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "hs_logger.h"
#include "hs_util.h"

static const char g_module[] = "output";


void hs_init_output(hs_output* output, const char* path, const char* subdir)
{
  output->fh = NULL;
  output->id = 0;
  output->offset = 0;
  size_t len = strlen(path) + strlen(subdir) + 2;
  output->path = malloc(len);
  if (!output->path) {
    hs_log(g_module, 0, "output path malloc failed");
    exit(EXIT_FAILURE);
  }
  snprintf(output->path, len, "%s/%s", path, subdir);

  int ret = mkdir(path, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP);
  if (ret && errno != EEXIST) {
    hs_log(g_module, 0, "output path could not be created: %s", path);
    exit(EXIT_FAILURE);
  }

  ret = mkdir(output->path, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP);
  if (ret && errno != EEXIST) {
    hs_log(g_module, 0, "output path could not be created: %s", output->path);
    exit(EXIT_FAILURE);
  }

  if (pthread_mutex_init(&output->lock, NULL)) {
    perror("output lock pthread_mutex_init failed");
    exit(EXIT_FAILURE);
  }
  hs_open_output_file(output);
}


void hs_free_output(hs_output* output)
{
  if (output->fh) fclose(output->fh);
  output->fh = NULL;

  free(output->path);
  output->path = NULL;

  pthread_mutex_destroy(&output->lock);
}


void hs_open_output_file(hs_output* output)
{
  static char fqfn[260];
  if (output->fh) {
    fclose(output->fh);
    output->fh = NULL;
  }
  int ret = snprintf(fqfn, sizeof(fqfn), "%s/%zu.log", output->path,
                     output->id);
  if (ret < 0 || ret > (int)sizeof(fqfn) - 1) {
    hs_log(g_module, 0, "output filename exceeds %zu", sizeof(fqfn));
    exit(EXIT_FAILURE);
  }
  output->fh = fopen(fqfn, "ab+");
  if (!output->fh) {
    hs_log(g_module, 0, "%s: %s", fqfn, strerror(errno));
    exit(EXIT_FAILURE);
  } else {
    fseek(output->fh, 0, SEEK_END);
  }
}
