
/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight unit tests @file */

#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hs_heka_message.h"
#include "../hs_logger.h"

  // {Uuid="" Timestamp = 1e9, Type="type", Logger="logger", Payload="payload", EnvVersion="env_version", Hostname="hostname", Severity=9, Fields = {number=1,numbers={value={1,2,3}, representation="count"},string="string",strings={"s1","s2","s3"}, bool=true, bools={true,false,false}}}
unsigned char pb[] = "\x0a\x10\x73\x1e\x36\x84\xec\x25\x42\x76\xa4\x01\x79\x6f\x17\xdd\x20\x63\x10\x80\x94\xeb\xdc\x03\x1a\x04\x74\x79\x70\x65\x22\x06\x6c\x6f\x67\x67\x65\x72\x28\x09\x32\x07\x70\x61\x79\x6c\x6f\x61\x64\x3a\x0b\x65\x6e\x76\x5f\x76\x65\x72\x73\x69\x6f\x6e\x4a\x08\x68\x6f\x73\x74\x6e\x61\x6d\x65\x52\x13\x0a\x06\x6e\x75\x6d\x62\x65\x72\x10\x03\x39\x00\x00\x00\x00\x00\x00\xf0\x3f\x52\x2c\x0a\x07\x6e\x75\x6d\x62\x65\x72\x73\x10\x03\x1a\x05\x63\x6f\x75\x6e\x74\x3a\x18\x00\x00\x00\x00\x00\x00\xf0\x3f\x00\x00\x00\x00\x00\x00\x00\x40\x00\x00\x00\x00\x00\x00\x08\x40\x52\x0e\x0a\x05\x62\x6f\x6f\x6c\x73\x10\x04\x42\x03\x01\x00\x00\x52\x0a\x0a\x04\x62\x6f\x6f\x6c\x10\x04\x40\x01\x52\x10\x0a\x06\x73\x74\x72\x69\x6e\x67\x22\x06\x73\x74\x72\x69\x6e\x67\x52\x15\x0a\x07\x73\x74\x72\x69\x6e\x67\x73\x22\x02\x73\x31\x22\x02\x73\x32\x22\x02\x73\x33";
size_t pblen = sizeof(pb);

static char* test_create_destroy()
{
  hs_heka_message m;
  hs_init_heka_message(&m, 8);
  hs_clear_heka_message(&m);
  hs_free_heka_message(&m);
  return NULL;
}

static char* test_decode()
{
  hs_heka_message m;
  hs_init_heka_message(&m, 8);
  mu_assert(hs_decode_heka_message(&m, pb, pblen-1), "decode failed");
  hs_free_heka_message(&m);
  return NULL;
}


static char* test_read()
{
  hs_heka_message m;
  hs_init_heka_message(&m, 8);
  mu_assert(hs_decode_heka_message(&m, pb, pblen-1), "decode failed");

  const char* val;
  double d;
  size_t len;
  mu_assert(hs_read_string_field(&m, "string", 6, 0, 0, &val, &len), "standalone");
  mu_assert(strncmp(val, "string", len) == 0, "invalid value: %.*s", (int)len, val);

  mu_assert(hs_read_string_field(&m, "strings", 7, 0, 0, &val, &len), "item 0");
  mu_assert(strncmp(val, "s1", len) == 0, "invalid value: %.*s", (int)len, val);

  mu_assert(hs_read_string_field(&m, "strings", 7, 0, 1, &val, &len), "item 1");
  mu_assert(strncmp(val, "s2", len) == 0, "invalid value: %.*s", (int)len, val);

  mu_assert(hs_read_string_field(&m, "strings", 7, 0, 2, &val, &len), "item 2");
  mu_assert(strncmp(val, "s3", len) == 0, "invalid value: %.*s", (int)len, val);

  mu_assert(hs_read_string_field(&m, "strings", 7, 0, 3, &val, &len) == false, "no item 3");

  mu_assert(hs_read_numeric_field(&m, "number", 6, 0, 0, &d), "standalone");
  mu_assert(d == 1, "invalid value: %g", d);

  mu_assert(hs_read_numeric_field(&m, "numbers", 7, 0, 0, &d), "item 0");
  mu_assert(d == 1, "invalid value: %g", d);

  mu_assert(hs_read_numeric_field(&m, "numbers", 7, 0, 1, &d), "item 1");
  mu_assert(d == 2, "invalid value: %g", d);

  mu_assert(hs_read_numeric_field(&m, "numbers", 7, 0, 2, &d), "item 2");
  mu_assert(d == 3, "invalid value: %g", d);

  mu_assert(hs_read_numeric_field(&m, "numbers", 7, 0, 3, &d) == false, "no item 3");

  mu_assert(hs_read_numeric_field(&m, "bool", 4, 0, 0, &d), "standalone");
  mu_assert(d == 1, "invalid value: %g", d);

  hs_free_heka_message(&m);
  return NULL;
}

static char* all_tests()
{
  mu_run_test(test_create_destroy);
  mu_run_test(test_decode);
  mu_run_test(test_read);
  return NULL;
}


int main()
{
  hs_init_log(7);
  char* result = all_tests();
  if (result) {
    printf("%s\n", result);
  } else {
    printf("ALL TESTS PASSED\n");
  }
  printf("Tests run: %d\n", mu_tests_run);
  hs_free_log();

  return result != 0;
}
