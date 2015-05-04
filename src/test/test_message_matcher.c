/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight message matcher unit tests @file */

#include "test.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hs_heka_message.h"
#include "../hs_message_matcher.h"

#include "../hs_logger.h"

// {"Logger":"GoSpec","Uuid":"xxx","Pid":32157,"Severity":6,"EnvVersion":"0.8","Fields":[{""value":["bar"],"name":"foo","value_type":0},{"value":[64],"name":"number","value_type":2},{"value":["data"],"name":"bytes","value_type":1},{"value":[999,1024],"name":"int","value_type":2},{"value":[99.9],"name":"double","value_type":3},{"value":[true],"name":"bool","value_type":4},{"value":["alternate"],"name":"foo","value_type":0},{"value":["name=test;type=web;"],"name":"Payload","value_type":0},{"representation":"date-time","value":["Mon Jan 02 15:04:05 -0700 2006"],"name":"Timestamp","value_type":0},{"value":[0],"name":"zero","value_type":2},{"value":["43"],"name":"string","value_type":0}],"Payload":"Test Payload","Timestamp":1.428773426113e+18,"Hostname":"trink-x230","Type":"TEST"}

unsigned char pb[] = "\x0a\x10\x27\x88\x81\xdc\x32\x6f\x4e\x3f\x9a\x5a\x93\x86\xa3\x7e\x24\x6f\x10\xe4\x9e\xf1\xff\xc6\xbb\x81\xea\x13\x1a\x04\x54\x45\x53\x54\x22\x06\x47\x6f\x53\x70\x65\x63\x28\x06\x32\x0c\x54\x65\x73\x74\x20\x50\x61\x79\x6c\x6f\x61\x64\x3a\x03\x30\x2e\x38\x40\x9d\xfb\x01\x4a\x0a\x74\x72\x69\x6e\x6b\x2d\x78\x32\x33\x30\x52\x0c\x0a\x03\x66\x6f\x6f\x10\x00\x22\x03\x62\x61\x72\x52\x0d\x0a\x06\x6e\x75\x6d\x62\x65\x72\x10\x02\x32\x01\x40\x52\x0f\x0a\x05\x62\x79\x74\x65\x73\x10\x01\x2a\x04\x64\x61\x74\x61\x52\x0d\x0a\x03\x69\x6e\x74\x10\x02\x32\x04\xe7\x07\x80\x08\x52\x14\x0a\x06\x64\x6f\x75\x62\x6c\x65\x10\x03\x3a\x08\x9a\x99\x99\x99\x99\xf9\x58\x40\x52\x0b\x0a\x04\x62\x6f\x6f\x6c\x10\x04\x42\x01\x01\x52\x12\x0a\x03\x66\x6f\x6f\x10\x00\x22\x09\x61\x6c\x74\x65\x72\x6e\x61\x74\x65\x52\x20\x0a\x07\x50\x61\x79\x6c\x6f\x61\x64\x10\x00\x22\x13\x6e\x61\x6d\x65\x3d\x74\x65\x73\x74\x3b\x74\x79\x70\x65\x3d\x77\x65\x62\x3b\x52\x38\x0a\x09\x54\x69\x6d\x65\x73\x74\x61\x6d\x70\x10\x00\x1a\x09\x64\x61\x74\x65\x2d\x74\x69\x6d\x65\x22\x1e\x4d\x6f\x6e\x20\x4a\x61\x6e\x20\x30\x32\x20\x31\x35\x3a\x30\x34\x3a\x30\x35\x20\x2d\x30\x37\x30\x30\x20\x32\x30\x30\x36\x52\x0b\x0a\x04\x7a\x65\x72\x6f\x10\x02\x32\x01\x00\x52\x0e\x0a\x06\x73\x74\x72\x69\x6e\x67\x10\x00\x22\x02\x34\x33";
size_t pblen = sizeof(pb);

static char* test_true_matcher()
{
  char* tests[] = {
    "TRUE"
    , "Timestamp > 1.428773420000e+18"
    , "Timestamp < 1.428773426999e18"
    , "Timestamp == 1428773426113040228"
    , "(Severity == 7 || Payload == 'Test Payload') && Type == 'TEST'"
    , "EnvVersion == \"0.8\""
    , "EnvVersion == '0.8'"
    , "EnvVersion != '0.9'"
    , "EnvVersion > '0.7'"
    , "EnvVersion >= '0.8'"
    , "EnvVersion < '0.9'"
    , "EnvVersion <= '0.8'"
    , "Hostname != ''"
    , "Logger == 'GoSpec'"
    , "Pid != 0"
    , "Severity != 5"
    , "Severity < 7"
    , "Severity <= 7"
    , "Severity <= 6"
    , "Severity == 6"
    , "Severity > 5"
    , "Severity >= 5"
    , "Severity >= 6"
    , "Timestamp > 0"
    , "Type != 'test'"
    , "Type == 'TEST' && Severity == 6"
    , "Type == 'test' && Severity == 7 || Payload == 'Test Payload'"
    , "Type == 'TEST'"
    , "Type == 'foo' || Type == 'bar' || Type == 'TEST'"
    , "Fields[foo] == 'bar'"
    , "Fields[foo][0] == 'bar'"
    , "Fields[foo][0][0] == 'bar'"
    , "Fields[foo][1] == 'alternate'"
    , "Fields[foo][1][0] == 'alternate'"
    , "Fields[foo] <= 'barx'"
    , "Fields[foo] < 'barx'"
    , "Fields[foo] >= 'bar'"
    , "Fields[foo] > 'baq'"
    , "Fields[foo] != 'bara'"
    , "Fields[bytes] == 'data'"
    , "Fields[int] == 999"
    , "Fields[int][0][1] == 1024"
    , "Fields[double] == 99.9"
    , "Fields[bool] == TRUE"
    , "Fields[int] != NIL"
    , "Fields[int][0][1] != NIL"
    , "Fields[int][0][2] == NIL"
    , "Fields[missing] == NIL"
// todo implement regex
//    , "Type =~ /TEST/"
//    , "Type !~ /bogus/"
//    , "Type =~ /TEST/ && Payload =~ /Payload/"
//    , "Fields[foo][1] =~ /alt/"
//    , "Fields[Payload] =~ /name=\\w+/"
//    , "Type =~ /(ST)/"
//    , "Type =~ /^TE/"
//    , "Type =~ /ST$/"
//    , "Type !~ /^te/"
//    , "Type !~ /st$/"
    , NULL };

  hs_heka_message m;
  hs_message_match_builder mmb;
  hs_init_heka_message(&m, 16);
  mu_assert(hs_decode_heka_message(&m, pb, pblen - 1), "decode failed");
  hs_init_message_match_builder(&mmb, TEST_LUA_PATH, TEST_LUA_CPATH);
  for (int i = 0; tests[i]; ++i) {
    hs_message_matcher* mm = hs_create_message_matcher(&mmb, tests[i]);
    mu_assert(mm, "failed to create the matcher");
    mu_assert(hs_eval_message_matcher(mm, &m), "%s", tests[i]);
    hs_free_message_matcher(mm);
    free(mm);
  }
  hs_free_message_match_builder(&mmb);
  hs_free_heka_message(&m);
  return NULL;
}


static char* test_false_matcher()
{
  char* tests[] = {
    "FALSE"
    , "Timestamp == 1e9"
    , "Type == 'test'&&(Severity==7||Payload=='Test Payload')"
    , "EnvVersion == '0.9'"
    , "EnvVersion != '0.8'"
    , "EnvVersion > '0.9'"
    , "EnvVersion >= '0.9'"
    , "EnvVersion < '0.8'"
    , "EnvVersion <= '0.7'"
    , "Severity == 5"
    , "Severity != 6"
    , "Severity < 6"
    , "Severity <= 5"
    , "Severity > 6"
    , "Severity >= 7"
    , "Fields[foo] == 'ba'"
    , "Fields[foo][1] == 'bar'"
    , "Fields[foo][0][1] == 'bar'"
    , "Fields[bool] == FALSE"
    , "Fields[foo] > 'bara'"
    , "Fields[foo] >= 'bara'"
    , "Fields[foo] == 'bara'"
    , "Type =~ /Test/"
    , "Type !~ /TEST/"
    , "Payload =~ /^Payload/"
    , "Type == \"te'st\""
    , "Type == 'te\"st'"
    , "Fields[int] =~ /999/"
    , "Fields[zero] == \"0\""
    , "Fields[string] == 43"
    , "Fields[int] == NIL"
    , "Fields[int][0][1] == NIL"
    , "Fields[missing] != NIL"
    , "Type =~ /^te/"
    , "Type =~ /st$/"
    , "Type !~ /^TE/"
    , "Type !~ /ST$/"
    , "Logger =~ /./ && Type =~ /^anything/"
    , NULL };

  hs_heka_message m;
  hs_message_match_builder mmb;
  hs_init_heka_message(&m, 8);
  mu_assert(hs_decode_heka_message(&m, pb, pblen - 1), "decode failed");
  hs_init_message_match_builder(&mmb, TEST_LUA_PATH, TEST_LUA_CPATH);
  for (int i = 0; tests[i]; ++i) {
    hs_message_matcher* mm = hs_create_message_matcher(&mmb, tests[i]);
    mu_assert(mm, "failed to create the matcher");
    mu_assert(hs_eval_message_matcher(mm, &m) == false, "%s", tests[i]);
    hs_free_message_matcher(mm);
    free(mm);
  }
  hs_free_message_match_builder(&mmb);
  hs_free_heka_message(&m);
  return NULL;
}


static char* test_malformed_matcher()
{
  char* tests[] = {
    ""
    , "bogus"
    , "Type = 'test'"                                               // invalid operator
    , "Pid == 'test='"                                              // Pid is not a string
    , "Type == 'test' && (Severity==7 || Payload == 'Test Payload'" // missing paren
    , "Invalid == 'bogus'"                                          // unknown variable name
    , "Fields[]"                                                    // empty name key
    , "Fields[test][]"                                              // empty field index
    , "Fields[test][a]"                                             // non numeric field index
    , "Fields[test][0][]"                                           // empty array index
    , "Fields[test][0][a]"                                          // non numeric array index
    , "Fields[test][0][0][]"                                        // extra index dimension
    , "Fields[test][xxxx"                                           // unmatched bracket
    , "Pid =~ /6/"                                                  // regex not allowed on numeric
    , "Pid !~ /6/"                                                  // regex not allowed on numeric
    , "Type =~ /test"                                               // unmatched slash
    , "Type == /test/"                                              // incorrect operator
    , "Type =~ 'test'"                                              // string instead of regexp
    , "Type =~ /\\ytest/"                                           // invalid escape character
    , "Type != 'test\""                                             // mis matched quote types
    , "Pid =~ 6"                                                    // number instead of regexp
    , "NIL"                                                         // invalid use of constant
    , "Type == NIL"                                                 // existence check only works on fields
    , "Fields[test] > NIL"                                          // existence check only works with equals and not equals
    , "TRUE FALSE"                                                  // missing operator
    , NULL };

  hs_heka_message m;
  hs_message_match_builder mmb;
  hs_init_heka_message(&m, 8);
  mu_assert(hs_decode_heka_message(&m, pb, pblen - 1), "decode failed");
  hs_init_message_match_builder(&mmb, TEST_LUA_PATH, TEST_LUA_CPATH);
  for (int i = 0; tests[i]; ++i) {
    hs_message_matcher* mm = hs_create_message_matcher(&mmb, tests[i]);
    mu_assert(mm == NULL, "created malformed matcher");
  }
  hs_free_message_match_builder(&mmb);
  hs_free_heka_message(&m);
  return NULL;
}


static char* benchmark_matcher_create()
{
  int iter = 100000;
  const char* exp = "Type == 'TEST' && Severity == 6";

  hs_message_match_builder mmb;
  hs_init_message_match_builder(&mmb, TEST_LUA_PATH, TEST_LUA_CPATH);

  clock_t t = clock();
  for (int x = 0; x < iter; ++x) {
    hs_message_matcher* mm = hs_create_message_matcher(&mmb, exp);
    mu_assert(mm, "hs_create_message_matcher failed");
    hs_free_message_matcher(mm);
    free(mm);
  }
  t = clock() - t;
  hs_log("benchmark", 7, "benchmark_matcher_create: %g", ((float)t)
         / CLOCKS_PER_SEC / iter);
  return NULL;
}

static char* benchmark_match()
{
  int iter = 1000000;

  char* tests[] = {
    "Type == 'TEST' && Severity == 6"
    , "Fields[foo] == 'bar' && Severity == 6"
    , "Fields[number] == 64 && Severity == 6"
    , "Fields[missing] == NIL"
    , "Fields[int] != NIL"
    , NULL };

  hs_heka_message m;
  hs_init_heka_message(&m, 8);
  mu_assert(hs_decode_heka_message(&m, pb, pblen - 1), "decode failed");
  hs_message_match_builder mmb;
  hs_init_message_match_builder(&mmb, TEST_LUA_PATH, TEST_LUA_CPATH);

  for (int i = 0; tests[i]; i++) {
    hs_message_matcher* mm = hs_create_message_matcher(&mmb, tests[i]);
    mu_assert(mm, "hs_create_message_matcher failed: %s", tests[i]);
    clock_t t = clock();
    for (int x = 0; x < iter; ++x) {
      mu_assert(hs_eval_message_matcher(mm, &m),
                "hs_eval_message_matcher failed");
    }
    t = clock() - t;
    hs_free_message_matcher(mm);
    free(mm);
    hs_log("benchmark", 7, "matcher: '%s': %g", tests[i], ((float)t)
           / CLOCKS_PER_SEC / iter);
  }
  hs_free_message_match_builder(&mmb);
  hs_free_heka_message(&m);
  return NULL;
}

static char* all_tests()
{
  mu_run_test(test_true_matcher);
  mu_run_test(test_false_matcher);
  mu_run_test(test_malformed_matcher);

  mu_run_test(benchmark_matcher_create);
  mu_run_test(benchmark_match);
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
