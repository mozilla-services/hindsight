/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight string pattern matcher unit tests @file */

#include "test.h"

#include <stddef.h>
#include <string.h>

#include "../hs_string_matcher.h"

typedef struct testcase {
   const char* s;
   const char* p;
} testcase;

static char* test_true_matcher()
{
  testcase tests[] = {
    {"test", "test"}
    , {"test", ".est"}
    , {"test", "%aest"}
    , {"\btest", "%ctest"}
    , {"1test", "%dtest"}
    , {"abc123", "%f[%d]123"}
    , {"test", "%lest"}
    , {"#test", "%ptest"}
    , {" test", "%stest"}
    , {"Test", "%uest"}
    , {"Test", "%w"}
    , {"0test", "%xtest"}
    , {"%test", "%%test"}
    , {"test", "[Tt]est"}
    , {"test", "[^B]est"}
    , {"", "%a*"}
    , {"test", "%a+"}
    , {"test", "%a-"}
    , {"t", "%a?"}
    , {"1", "%a?"}
    , {"(test)", "%b()"}
    , {"test", "^t"}
    , {"test", "t$"}
    , {NULL, NULL}
  };

  for (int i = 0; tests[i].s; ++i) {

    mu_assert(hs_string_match(tests[i].s,
                              strlen(tests[i].s),
                              tests[i].p), "%s", tests[i].p);
  }
  mu_assert(hs_string_match("\0test", 5, "%ztest"), "null test");
  return NULL;
}


static char* test_false_matcher()
{
  testcase tests[] = {
    {"test", "abcd"}
    , {"test", ".bcd"}
    , {"\n", "%a"}
    , {"t", "%c"}
    , {"t", "%d"}
    , {"abc1", "%f[%d]2"}
    , {"1", "%l"}
    , {"t", "%p"}
    , {"t", "%s"}
    , {"t", "%u"}
    , {"#", "%w"}
    , {"t", "%x"}
    , {"t", "%T"}
    , {"a", "[Tt]"}
    , {"t", "[^Tt]"}
    , {"###", "%a+"}
    , {"test", "%b()"}
    , {"test", "^T"}
    , {"test", "T$"}
    , {"t", "%z"}
    , {"test", "%b(]"} // invalid pattern
    , {"test", "%"} // invalid pattern
    , {"test", "%ft"} // invalid pattern
    , {"test", "[Tt"} // invalid pattern
    , {NULL, NULL}
  };

  for (int i = 0; tests[i].s; ++i) {

    mu_assert(!hs_string_match(tests[i].s,
                              strlen(tests[i].s),
                              tests[i].p), "%s", tests[i].p);
  }
  return NULL;
}


static char* all_tests()
{
  mu_run_test(test_true_matcher);
  mu_run_test(test_false_matcher);
  return NULL;
}


int main()
{
  char* result = all_tests();
  if (result) {
    printf("%s\n", result);
  } else {
    printf("ALL TESTS PASSED\n");
  }
  printf("Tests run: %d\n", mu_tests_run);

  return result != 0;
}
