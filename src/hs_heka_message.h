/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** Hindsight Heka message representation @file */

#ifndef hs_heka_message_h_
#define hs_heka_message_h_

#include <luasandbox/lauxlib.h>
#include <luasandbox/lua.h>
#include <stdbool.h>
#include <stddef.h>

#include "hs_input.h"

#define HS_HEKA_UUID_SIZE 16

#define HS_HEKA_UUID_KEY "Uuid"
#define HS_HEKA_TIMESTAMP_KEY "Timestamp"
#define HS_HEKA_TYPE_KEY "Type"
#define HS_HEKA_LOGGER_KEY "Logger"
#define HS_HEKA_SEVERITY_KEY "Severity"
#define HS_HEKA_PAYLOAD_KEY "Payload"
#define HS_HEKA_ENV_VERSION_KEY "EnvVersion"
#define HS_HEKA_PID_KEY "Pid"
#define HS_HEKA_HOSTNAME_KEY "Hostname"
#define HS_HEKA_FIELDS_KEY "Fields"

typedef enum {
  HS_FIELD_STRING,
  HS_FIELD_BYTES,
  HS_FIELD_INTEGER,
  HS_FIELD_DOUBLE,
  HS_FIELD_BOOL
} hs_field_value_type;


typedef enum {
  HS_HEKA_UUID = 1,
  HS_HEKA_TIMESTAMP = 2,
  HS_HEKA_TYPE = 3,
  HS_HEKA_LOGGER = 4,
  HS_HEKA_SEVERITY = 5,
  HS_HEKA_PAYLOAD = 6,
  HS_HEKA_ENV_VERSION = 7,
  HS_HEKA_PID = 8,
  HS_HEKA_HOSTNAME = 9,
  HS_HEKA_FIELD = 10
} hs_heka_pb_id;


typedef struct hs_heka_field
{
  const char* name;
  const char* representation;
  const unsigned char* value;

  unsigned name_len;
  unsigned representation_len;
  unsigned value_len;
  hs_field_value_type value_type;
} hs_heka_field;


typedef struct hs_heka_message
{
  const unsigned char* msg;
  const char* uuid;
  const char* type;
  const char* logger;
  const char* payload;
  const char* env_version;
  const char* hostname;
  hs_heka_field* fields;

  long long timestamp;
  int severity;
  int pid;

  unsigned msg_len;
  unsigned type_len;
  unsigned logger_len;
  unsigned payload_len;
  unsigned env_version_len;
  unsigned hostname_len;
  unsigned fields_len;
  unsigned fields_size;
} hs_heka_message;


typedef enum {
  HS_READ_NIL,
  HS_READ_NUMERIC,
  HS_READ_STRING,
  HS_READ_BOOL
} hs_read_type;


typedef struct {
  union
  {
    const char* s;
    double d;
  } u;
  size_t len;
  hs_read_type type;
} hs_read_value;


void hs_init_heka_message(hs_heka_message* m, size_t size);
void hs_free_heka_message(hs_heka_message* m);

void hs_clear_heka_message(hs_heka_message* m);
bool hs_find_message(hs_heka_message* m, hs_input_buffer* hsib, bool decode);
bool hs_decode_heka_message(hs_heka_message* m,
                            const unsigned char* buf,
                            size_t len);
bool hs_read_message_field(hs_heka_message* m,
                          const char* name,
                          size_t nlen,
                          int fi,
                          int ai,
                          hs_read_value *val);
int hs_read_message(lua_State* lua, hs_heka_message* m);
#endif
