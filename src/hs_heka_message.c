/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight Heka message implementation @file */

#include "hs_heka_message.h"

#include <stdlib.h>
#include <string.h>

#include "hs_logger.h"

#define MAX_VARINT_BYTES 10

static unsigned const char*
read_key(unsigned const char* p, int* tag, int* wiretype)
{
  *wiretype = 7 & *p;
  *tag = *p >> 3;
  return ++p;
}


static unsigned const char*
read_length(unsigned const char* p, unsigned const char* e, size_t* vi)
{
  *vi = 0;
  unsigned i, shift = 0;
  for (i = 0; p != e && i < MAX_VARINT_BYTES; i++) {
    *vi |= ((unsigned long long)p[i] & 0x7f) << shift;
    shift += 7;
    if ((p[i] & 0x80) == 0) break;
  }
  if (i == MAX_VARINT_BYTES) {
    return NULL;
  }
  return p + i + 1;
}


static unsigned const char*
read_varint(unsigned const char* p, unsigned const char* e, long long* vi)
{
  *vi = 0;
  unsigned i, shift = 0;
  for (i = 0; p != e && i < MAX_VARINT_BYTES; i++) {
    *vi |= ((unsigned long long)p[i] & 0x7f) << shift;
    shift += 7;
    if ((p[i] & 0x80) == 0) break;
  }
  if (i == MAX_VARINT_BYTES) {
    return NULL;
  }
  return p + i + 1;
}


static unsigned const char*
read_string(int wiretype,
            unsigned const char* p,
            unsigned const char* e,
            const char** s,
            size_t* len)
{
  if (wiretype != 2) {
    return NULL;
  }

  *len = 0;
  p = read_length(p, e, len);
  if (!p || p + *len > e) {
    return NULL;
  }
  *s = (const char*)p;
  p += *len;
  return p;
}


static unsigned const char*
process_varint(int wiretype,
               unsigned const char* p,
               unsigned const char* e,
               long long* val)
{
  if (wiretype != 0) {
    return NULL;
  }
  *val = 0;
  p = read_varint(p, e, val);
  if (!p) {
    return NULL;
  }
  return p;
}


static unsigned const char*
process_fields(hs_heka_field* f,
               unsigned const char* p,
               unsigned const char* e)
{
  int tag = 0;
  int wiretype = 0;
  long long val = 0;
  size_t len = 0;

  p = read_length(p, e, &len);
  if (!p || p + len > e) {
    return NULL;
  }
  e = p + len; // only process to the end of the current field record

  const char* s;
  size_t sl;

  do {
    p = read_key(p, &tag, &wiretype);

    switch (tag) {
    case 1:
      p = read_string(wiretype, p, e, &s, &sl);
      if (p) {
        f->name = s;
        f->name_len = (int)sl;
      }
      break;

    case 2:
      p = process_varint(wiretype, p, e, &val);
      if (p) {
        f->value_type = (int)val;
      }
      break;

    case 3:
      p = read_string(wiretype, p, e, &s, &sl);
      if (p) {
        f->representation = s;
        f->representation_len = (int)sl;
      }
      break;

      // don't bother with the value(s) until we actually need them
      // since this stream is created by Hindsight
      // - tags are guaranteed to be properly ordered (values at the end)
      // - there won't be repeated tags for packed values
    case 4: // value_string
    case 5: // value_bytes
      if (wiretype != 2) {
        p = NULL;
        break;
      }
      f->value = p - 1;
      f->value_len = (int)(e - f->value);
      p = e;
      break;
    case 6: // value_integer
    case 8: // value_bool
      if (wiretype != 0 && wiretype != 2) {
        p = NULL;
        break;
      }
      // fall thru
    case 7: // value_double
      if (tag == 7 && wiretype != 1 && wiretype != 2) {
        p = NULL;
        break;
      }
      if (wiretype == 2) {
        p = read_length(p, e, &len);
        if (!p || p + len > e) {
          p = NULL;
          break;
        }
      }
      f->value = p;
      f->value_len = (int)(e - f->value);
      p = e;
      break;
    default:
      p = NULL; // don't allow unknown tags
      break;
    }
  }
  while (p && p < e);

  return f->name ? p : NULL;
}

static bool
read_string_value(const unsigned char* p, const unsigned char* e, int ai,
                  hs_read_value* val)
{
  int acnt = 0;
  int tag = 0;
  int wiretype = 0;
  while (p && p < e) {
    val->type = HS_READ_NIL;
    p = read_key(p, &tag, &wiretype);
    p = read_string(wiretype, p, e, &val->u.s, &val->len);
    if (p) {
      if (ai == acnt++) {
        val->type = HS_READ_STRING;
        return true;
      }
    }
  }
  return false;
}


static bool
read_integer_value(const unsigned char* p, const unsigned char* e, int ai,
                   hs_read_value* val)
{
  int acnt = 0;
  long long ll = 0;
  while (p && p < e) {
    p = read_varint(p, e, &ll);
    if (p) {
      if (ai == acnt++) {
        val->type = HS_READ_NUMERIC;
        val->u.d = (double)ll;
        return true;
      }
    }
  }
  return false;
}


static bool
read_double_value(const unsigned char* p, const unsigned char* e, int ai,
                  hs_read_value* val)
{
  if (p + (sizeof(double) * (ai + 1)) > e) {
    return false;
  }
  val->type = HS_READ_NUMERIC;
  p += sizeof(double) * ai;
  memcpy(&val->u.d, p, sizeof(double));
  return true;
}


bool hs_decode_heka_message(hs_heka_message* m,
                            const unsigned char* buf,
                            size_t len)
{
  unsigned const char* cp, *lp, *ep;
  cp = buf; // current position
  lp = cp; // last position
  ep = buf + len; // end position

  const char* s;
  size_t sl;
  long long val;
  int wiretype = 0;
  int tag = 0;
  hs_clear_heka_message(m);

  do {
    cp = read_key(cp, &tag, &wiretype);

    switch (tag) {
    case HS_HEKA_UUID:
      cp = read_string(wiretype, cp, ep, &s, &sl);
      if (cp && sl == 16) {
        m->uuid = s;
      } else {
        cp = NULL;
      }
      break;

    case HS_HEKA_TIMESTAMP:
      cp = process_varint(wiretype, cp, ep, &val);
      if (cp) {
        m->timestamp = val;
      }
      break;

    case HS_HEKA_TYPE:
      cp = read_string(wiretype, cp, ep, &s, &sl);
      if (cp) {
        m->type = s;
        m->type_len = (int)sl;
      }
      break;

    case HS_HEKA_LOGGER:
      cp = read_string(wiretype, cp, ep, &s, &sl);
      if (cp) {
        m->logger = s;
        m->logger_len = (int)sl;
      }
      break;

    case HS_HEKA_SEVERITY:
      cp = process_varint(wiretype, cp, ep, &val);
      if (cp) {
        m->severity = (int32_t)val;
      }
      break;

    case HS_HEKA_PAYLOAD:
      cp = read_string(wiretype, cp, ep, &s, &sl);
      if (cp) {
        m->payload = s;
        m->payload_len = (int)sl;
      }
      break;

    case HS_HEKA_ENV_VERSION:
      cp = read_string(wiretype, cp, ep, &s, &sl);
      if (cp) {
        m->env_version = s;
        m->env_version_len = (int)sl;
      }
      break;

    case HS_HEKA_PID:
      cp = process_varint(wiretype, cp, ep, &val);
      if (cp) {
        m->pid = (int32_t)val;
      }
      break;

    case HS_HEKA_HOSTNAME:
      cp = read_string(wiretype, cp, ep, &s, &sl);
      if (cp) {
        m->hostname = s;
        m->hostname_len = (int)sl;
      }
      break;

    case HS_HEKA_FIELD:
      if (wiretype != 2) {
        cp = NULL;
        break;
      }
      ++m->fields_len;
      if (m->fields_len >= m->fields_size) {
        m->fields_size += 8;
        hs_heka_field* tmp = realloc(m->fields,
                                     m->fields_size * sizeof(hs_heka_field));
        if (!tmp) {
          hs_log(HS_APP_NAME, 0, "message fields reallocation failed");
          exit(EXIT_FAILURE);
        }
        m->fields = tmp;
      }
      cp = process_fields(&m->fields[m->fields_len - 1], cp, ep);
      break;

    default:
      cp = NULL; // don't allow unknown tags
      break;
    }
    if (cp) lp = cp;
  }
  while (cp && cp < ep);

  if (!cp) {
    hs_log(HS_APP_NAME, 4, "error in tag: %d wiretype: %d offset: %d", tag,
           wiretype, lp - buf);
    return false;
  }

  if (!m->uuid) {
    hs_log(HS_APP_NAME, 4, "missing uuid");
    return false;
  }

  if (!m->timestamp) {
    hs_log(HS_APP_NAME, 4, "missing timestamp");
    return false;
  }

  return true;
}


void hs_init_heka_message(hs_heka_message* m, size_t size)
{
  m->fields_size = (int)size;
  m->fields = malloc(m->fields_size * sizeof(hs_heka_field));
  if (!m->fields) {
    hs_log(HS_APP_NAME, 0, "message fields allocation failed");
    exit(EXIT_FAILURE);
  }
  hs_clear_heka_message(m);
}


void hs_clear_heka_message(hs_heka_message* m)
{
  m->uuid = NULL;
  m->type = NULL;
  m->logger = NULL;
  m->payload = NULL;
  m->env_version = NULL;
  m->hostname = NULL;
  if (m->fields) {
    memset(m->fields, 0, m->fields_size * sizeof(hs_heka_field));
  }
  m->timestamp = 0;
  m->severity = 7;
  m->pid = 0;
  m->type_len = 0;
  m->logger_len = 0;
  m->payload_len = 0;
  m->env_version_len = 0;
  m->hostname_len = 0;
  m->fields_len = 0;
}


void hs_free_heka_message(hs_heka_message* m)
{
  free(m->fields);
  m->fields = NULL;
  m->fields_size = 0;
  hs_clear_heka_message(m);
}


bool hs_read_message_field(hs_heka_message* m, const char* name, size_t nlen,
                           int fi, int ai, hs_read_value* val)
{
  int fcnt = 0;
  const unsigned char* p, *e;
  val->type = HS_READ_NIL;

  for (int i = 0; i < m->fields_len; ++i) {
    if (nlen == (size_t)m->fields[i].name_len
        && strncmp(name, m->fields[i].name, m->fields[i].name_len) == 0) {
      if (fi == fcnt++) {
        p = m->fields[i].value;
        e = p + m->fields[i].value_len;
        switch (m->fields[i].value_type) {
        case HS_FIELD_STRING:
        case HS_FIELD_BYTES:
          return read_string_value(p, e, ai, val);
        case HS_FIELD_INTEGER:
        case HS_FIELD_BOOL:
          return read_integer_value(p, e, ai, val);
        case HS_FIELD_DOUBLE:
          return read_double_value(p, e, ai, val);
        default:
          return false;
        }
      }
    }
  }
  return false;
}
