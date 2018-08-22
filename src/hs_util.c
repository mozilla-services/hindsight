/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight util implementation @file */

#include "hs_util.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>


bool hs_file_exists(const char *fn)
{
  FILE *fh = fopen(fn, "re");
  if (fh) {
    fclose(fh);
    return 1;
  }
  return 0;
}


int hs_get_fqfn(const char *path,
                const char *name,
                char *fqfn,
                size_t fqfn_len)
{
  int rv = snprintf(fqfn, fqfn_len, "%s/%s", path, name);
  return (rv < 0 || rv > (int)fqfn_len - 1);
}


bool hs_find_lua(const hs_config *cfg,
                 const hs_sandbox_config *sbc,
                 const char *ptype,
                 char *fqfn,
                 size_t fqfn_len)
{
  int rv = snprintf(fqfn, fqfn_len, "%s/%s", sbc->dir, sbc->filename);
  if (rv < 0 || rv > (int)fqfn_len - 1) return false;
  if (hs_file_exists(fqfn)) return true;

  rv = snprintf(fqfn, fqfn_len, "%s/%s/%s", cfg->install_path, ptype, sbc->filename);
  if (rv < 0 || rv > (int)fqfn_len - 1) return false;
  return hs_file_exists(fqfn);
}


int hs_output_lua_string(FILE *fh, const char *s)
{
  int rv = 1;
  size_t len = strlen(s);
  for (unsigned i = 0; i < len && rv == 1; ++i) {
    switch (s[i]) {
    case '\n':
      rv = fwrite("\\n", 2, 1, fh);
      break;
    case '\r':
      rv = fwrite("\\r", 2, 1, fh);
      break;
    case '"':
      rv = fwrite("\\\"", 2, 1, fh);
      break;
    case '\\':
      fwrite("\\\\", 2, 1, fh);
      break;
    default:
      rv = fwrite(s + i, 1, 1, fh);
      break;
    }
  }
  return rv == 1 ? 0 : 1;
}


bool hs_has_ext(const char *fn, const char *ext)
{
  size_t flen = strlen(fn);
  size_t elen = strlen(ext);
  if (flen <= elen) return false; // a fn with only an extension is invalid
  return strcmp(fn + flen - elen, ext) == 0 ? true : false;
}


unsigned hs_disk_free_ob(const char *path, unsigned ob_size)
{
  struct statfs buf;
  if (ob_size == 0 || statfs(path, &buf)) return 0;
  return buf.f_bsize * buf.f_bavail / ob_size;
}



static FILE* common_termination_err(const char *path, const char *name)
{
  if (!path || !name) return NULL;
  const char *pos = strchr(name, '.');
  if (!pos) return NULL;

  char fn[HS_MAX_PATH];
  int ret = snprintf(fn, sizeof(fn), "%s/%.*s/%s%s", path,
                     (int)(pos - name), name,
                     pos + 1, hs_err_ext);
  if (ret < 0 || ret > (int)sizeof(fn) - 1) return NULL;

  FILE *fh = fopen(fn, "w+e");
  if (fh) {
    time_t t = time(NULL);
    struct tm tms;
    if (gmtime_r(&t, &tms)) {
      fprintf(fh, "%04d-%02d-%02dT%02d:%02d:%02d\t",
              tms.tm_year + 1900, tms.tm_mon + 1, tms.tm_mday, tms.tm_hour,
              tms.tm_min, tms.tm_sec);
    }
  }
  return fh;
}


void hs_prune_err(const char *dir)
{
  DIR *dp = opendir(dir);
  if (!dp) {
    return;
  }

  struct dirent *entry;
  while ((entry = readdir(dp))) {
    if (hs_has_ext(entry->d_name, hs_err_ext)) {
      char fqfn[HS_MAX_PATH];
      if (!hs_get_fqfn(dir, entry->d_name, fqfn, sizeof(fqfn))) {
        unlink(fqfn);
      }
    }
  }
  closedir(dp);
}


void hs_save_termination_err(const char *path,
                             const char *name,
                             const char *err)
{
  if (!err) return;

  FILE *fh = common_termination_err(path, name);
  if (fh) {
    fprintf(fh, "%s\n", err);
    fclose(fh);
  }
}


void hs_save_termination_err_vfmt(const char *path,
                                 const char *name,
                                 const char *fmt,
                                 va_list arg)
{
  FILE *fh = common_termination_err(path, name);
  if (fh) {
    vfprintf(fh, fmt, arg);
    fputc('\n', fh);
    fclose(fh);
  }
}


bool hs_remove_file(const char *path, const char *file)
{
  char fqfn[HS_MAX_PATH];
  if (hs_get_fqfn(path, file, fqfn, sizeof(fqfn))) {
    return false;
  }
  return unlink(fqfn) == 0 ? true : false;
}
