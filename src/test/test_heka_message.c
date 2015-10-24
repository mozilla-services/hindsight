
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


static char* test_read_message()
{
  double d;
  const char* s;
  hs_heka_message m;
  hs_init_heka_message(&m, 8);
  mu_assert(hs_decode_heka_message(&m, pb, pblen-1), "decode failed");

  lua_State* lua = luaL_newstate();
  mu_assert(lua, "luaL_newstate failed");

  lua_pushstring(lua, "Timestamp");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  d = lua_tonumber(lua, -1);
  mu_assert(d == 1e9, "incorrect Timestamp: %g", d);
  lua_pop(lua, lua_gettop(lua));

  size_t len;
  lua_pushstring(lua, "Uuid");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  s = lua_tolstring(lua, -1, &len);
  mu_assert(s, "Uuid not set");
  mu_assert(len == HEKA_UUID_SIZE, "Uuid invalid len %zu", len);
  mu_assert(memcmp(s, "\x73\x1e\x36\x84\xec\x25\x42\x76\xa4\x01\x79\x6f\x17\xdd\x20\x63", len) == 0, "incorrect Uuid");

  lua_pop(lua, lua_gettop(lua));
  lua_pushstring(lua, "Type");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  s = lua_tostring(lua, -1);
  mu_assert(s, "Type not set");
  mu_assert(strcmp(s, "type") == 0, "incorrect Type: %s", s);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "Logger");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  s = lua_tostring(lua, -1);
  mu_assert(s, "Logger not set");
  mu_assert(strcmp(s, "logger") == 0, "incorrect Logger: %s", s);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "Payload");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  s = lua_tostring(lua, -1);
  mu_assert(s, "Payload not set");
  mu_assert(strcmp(s, "payload") == 0, "incorrect Payload: %s", s);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "EnvVersion");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  s = lua_tostring(lua, -1);
  mu_assert(s, "EnvVersion not set");
  mu_assert(strcmp(s, "env_version") == 0, "incorrect EnvVersion: %s", s);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "Hostname");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  s = lua_tostring(lua, -1);
  mu_assert(s, "Hostname not set");
  mu_assert(strcmp(s, "hostname") == 0, "incorrect Hostname: %s", s);
  lua_pop(lua, lua_gettop(lua));

  int n;
  lua_pushstring(lua, "Severity");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  n = lua_tointeger(lua, -1);
  mu_assert(n == 9, "incorrect Severity: %d", n);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "Pid");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  n = lua_tointeger(lua, -1);
  mu_assert(n == 0, "incorrect Pid: %d", n);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "raw");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  s = lua_tolstring(lua, -1, &len);
  mu_assert(s, "raw not set");
  mu_assert(len == pblen-1, "raw invalid len %zu", len);
  mu_assert(memcmp(s, pb, len) == 0, "incorrect raw");
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "Fields[string]");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  s = lua_tostring(lua, -1);
  mu_assert(s, "Fields[string] not set");
  mu_assert(strcmp(s, "string") == 0, "incorrect Fields[string]: %s", s);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "Fields[notfound]");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  n = lua_type(lua, -1);
  mu_assert(n == LUA_TNIL, "invalid type: %d", n);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "Fields[string"); // missing closing bracket
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  n = lua_type(lua, -1);
  mu_assert(n == LUA_TNIL, "invalid type: %d", n);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "morethan8");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  n = lua_type(lua, -1);
  mu_assert(n == LUA_TNIL, "invalid type: %d", n);
  lua_pop(lua, lua_gettop(lua));

  lua_pushstring(lua, "lt8");
  mu_assert(hs_read_message(lua, &m) == 1, "hs_read_message failed");
  n = lua_type(lua, -1);
  mu_assert(n == LUA_TNIL, "invalid type: %d", n);
  lua_pop(lua, lua_gettop(lua));

  hs_free_heka_message(&m);
  return NULL;
}


static char* test_read_message_field()
{
  hs_heka_message m;
  hs_init_heka_message(&m, 8);
  mu_assert(hs_decode_heka_message(&m, pb, pblen-1), "decode failed");

  hs_read_value v;
  mu_assert(hs_read_message_field(&m, "string", 6, 0, 0, &v), "standalone");
  mu_assert(v.type == HS_READ_STRING, "%d", v.type);
  mu_assert(strncmp(v.u.s, "string", v.len) == 0, "invalid value: %.*s",
            (int)v.len, v.u.s);

  mu_assert(hs_read_message_field(&m, "strings", 7, 0, 0, &v), "item 0");
  mu_assert(v.type == HS_READ_STRING, "%d", v.type);
  mu_assert(strncmp(v.u.s, "s1", v.len) == 0, "invalid value: %.*s",
            (int)v.len, v.u.s);

  mu_assert(hs_read_message_field(&m, "strings", 7, 0, 1, &v), "item 1");
  mu_assert(v.type == HS_READ_STRING, "%d", v.type);
  mu_assert(strncmp(v.u.s, "s2", v.len) == 0, "invalid value: %.*s",
            (int)v.len, v.u.s);

  mu_assert(hs_read_message_field(&m, "strings", 7, 0, 2, &v), "item 2");
  mu_assert(v.type == HS_READ_STRING, "%d", v.type);
  mu_assert(strncmp(v.u.s, "s3", v.len) == 0, "invalid value: %.*s",
            (int)v.len, v.u.s);

  mu_assert(hs_read_message_field(&m, "strings", 7, 0, 3, &v) == false,
            "no item 3");
  mu_assert(v.type == HS_READ_NIL, "%d", v.type);

  mu_assert(hs_read_message_field(&m, "number", 6, 0, 0, &v), "standalone");
  mu_assert(v.type == HS_READ_NUMERIC, "%d", v.type);
  mu_assert(v.u.d == 1, "invalid value: %g", v.u.d);

  mu_assert(hs_read_message_field(&m, "numbers", 7, 0, 0, &v), "item 0");
  mu_assert(v.type == HS_READ_NUMERIC, "%d", v.type);
  mu_assert(v.u.d == 1, "invalid value: %g", v.u.d);

  mu_assert(hs_read_message_field(&m, "numbers", 7, 0, 1, &v), "item 1");
  mu_assert(v.type == HS_READ_NUMERIC, "%d", v.type);
  mu_assert(v.u.d == 2, "invalid value: %g", v.u.d);

  mu_assert(hs_read_message_field(&m, "numbers", 7, 0, 2, &v), "item 2");
  mu_assert(v.type == HS_READ_NUMERIC, "%d", v.type);
  mu_assert(v.u.d == 3, "invalid value: %g", v.u.d);

  mu_assert(hs_read_message_field(&m, "numbers", 7, 0, 3, &v) == false,
            "no item 3");
  mu_assert(v.type == HS_READ_NIL, "%d", v.type);

  mu_assert(hs_read_message_field(&m, "bool", 4, 0, 0, &v), "standalone");
  mu_assert(v.type == HS_READ_BOOL, "%d", v.type);
  mu_assert(v.u.d == 1, "invalid value: %g", v.u.d);

  hs_free_heka_message(&m);
  return NULL;
}


static char* all_tests()
{
  mu_run_test(test_create_destroy);
  mu_run_test(test_decode);
  mu_run_test(test_read_message);
  mu_run_test(test_read_message_field);
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
