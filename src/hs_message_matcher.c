/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/** @brief Hindsight Heka message matcher implementation @file */

#include "hs_message_matcher.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "hs_logger.h"


static const char* grammar =
  "local l = require 'lpeg'\n"
  "l.locale(l)\n"
  "\n"
  "local sp            = l.space^0\n"
  "local eqne          = l.Cg(l.P'==' + '!=', 'op')\n"
  "local regexp        = l.Cg(l.P'=~' + '!~', 'op')\n"
  "local relational    = l.Cg(eqne + '>=' + '>' + '<=' + '<', 'op')\n"
  "local op_and        = l.C(l.P'&&')\n"
  "local op_or         = l.C(l.P'||')\n"
  "\n"
  "local string_vars   = l.Cg(l.P'Type' + 'Logger' + 'Hostname' + 'EnvVersion' + 'Payload' + 'Uuid', 'variable')\n"
  "local numeric_vars  = l.Cg(l.P'Severity' + 'Pid', 'variable')\n"
  "local boolean       = l.Cg(l.P'TRUE' / function() return true end + l.P'FALSE' / function() return false end, 'value')\n"
  "local null          = l.P'NIL'\n"
  "local index         = l.P'[' * (l.digit^1 / tonumber) * l.P']' + l.Cc'0' / tonumber\n"
  "local fields        = l.P'Fields[' * l.Cg((l.P(1) - ']')^0, 'variable') * ']' * l.Cg(index, 'fi') * l.Cg(index, 'ai')\n"
  "\n"
  "local string_value  = l.Cg(l.P'\"' * l.Cs(((l.P(1) - l.S'\\\\\"') + l.P'\\\\\"' / '\"')^0) * '\"'\n"
  "                    + l.P'\\'' * l.Cs(((l.P(1) - l.S'\\\\\\'') + l.P'\\\\\\'' / '\\'')^0) * '\\'', 'value')\n"
  "local regexp_value  = l.Cg(l.P'/' * l.Cs(((l.P(1) - l.S'\\/') + l.P'\\/' / '/')^0) * '/', 'value')\n"
  "local string_test   = l.Ct(string_vars * sp * (relational * sp * string_value + regexp * sp * regexp_value))\n"
  "\n"
  "local sign          = l.S'+-'\n"
  "local exponent      = l.S'eE' * sign^-1 * l.digit^1\n"
  "local float         = l.digit^0 * '.' * l.digit^1 * exponent^-1\n"
  "local numeric_value = l.Cg(sign^-1 * (float + l.digit^1) / tonumber, 'value')\n"
  "local numeric_test  = l.Ct(numeric_vars * sp * relational * sp * numeric_value)\n"
  "\n"
  "local field_test    = l.Ct(fields * sp * (relational * sp * (string_value + numeric_value)\n"
  "                    + regexp * sp * regexp_value\n"
  "                    + eqne * sp * (boolean + null)))\n"
  "\n"
  "local Exp, Term, Test = l.V'Exp', l.V'Term', l.V'Test'\n"
  "g = l.P{\n"
  "Exp,\n"
  "Exp = l.Ct(Term * (op_or * sp * Term)^0);\n"
  "Term = l.Ct(Test * (op_and * sp * Test)^0) * sp;\n"
  "Test = (string_test + numeric_test + field_test + l.Ct(l.Cg(boolean, 'op'))) * sp + l.P'(' * Exp * ')' * sp;\n"
  "}\n"
  "\n"
  "local function postfix(t, output, stack, stack_ptr)\n"
  "    local sp = stack_ptr\n"
  "    for j,v in ipairs(t) do\n"
  "        if type(v) == 'string' then\n"
  "            stack_ptr = stack_ptr + 1\n"
  "            stack[stack_ptr] = v\n"
  "        elseif type(v) == 'table' then\n"
  "            if v.op then\n"
  "                output[#output+1] = v\n"
  "            else\n"
  "                postfix(v, output, stack, stack_ptr)\n"
  "            end\n"
  "        end\n"
  "    end\n"
  "    for i=stack_ptr, sp+1, -1 do\n"
  "        output[#output+1] = stack[i]\n"
  "    end\n"
  "    stack_ptr = sp\n"
  "    return output\n"
  "end\n"
  "\n"
  "local grammar = l.Ct(sp * g * -1)\n"
  "\n"
  "function parse(s)\n"
  "    local output = {}\n"
  "    local stack = {}\n"
  "    local stack_ptr = 0\n"
  "    local t = grammar:match(s)\n"
  "    local len = 0\n"
  "    if t then\n"
  "        t = postfix(t, output, stack, stack_ptr)\n"
  "        len = #t\n"
  "    end\n"
  "    return t, len -- node array and length\n"
  "end\n";


typedef enum {
  OP_EQ,
  OP_NE,
  OP_GTE,
  OP_GT,
  OP_LTE,
  OP_LT,
  OP_RE,
  OP_NRE,
  OP_TRUE,
  OP_FALSE,
  OP_OR,
  OP_AND
} match_operation;


typedef enum {
  TYPE_STRING,
  TYPE_NUMERIC,
  TYPE_BOOLEAN,
  TYPE_REGEX,
  TYPE_NIL
} match_type;


typedef struct {
  match_type type;
  union
  {
    char* s;
    double d;
    // regex* r;
  } u;
} match_value;


typedef struct match_node {
  char* variable;
  struct match_node* left;
  struct match_node* right;
  match_operation op;
  hs_heka_pb_id pbid;
  unsigned fi;
  unsigned ai;
  match_type value_type;
  union
  {
    char* s;
    double d;
    // regex* r;
  } value;
} match_node;


struct hs_message_matcher
{
  int nodes_size;
  match_node nodes[];
};


bool string_test(match_node* n, const char* val, size_t len)
{
  if (!val && n->value_type == TYPE_NIL) return true;
  if (!val || (n->value_type != TYPE_STRING && n->value_type != TYPE_REGEX)) {
    return false;
  }

  switch (n->op) {
  case OP_EQ:
    return strncmp(val, n->value.s, len) == 0;
  case OP_NE:
    return strncmp(val, n->value.s, len) != 0;
  case OP_LT:
    return strncmp(val, n->value.s, len) < 0;
  case OP_LTE:
    return strncmp(val, n->value.s, len) <= 0;
  case OP_GT:
    return strncmp(val, n->value.s, len) > 0;
  case OP_GTE:
    return strncmp(val, n->value.s, len) >= 0;
  case OP_RE:
    return false; // todo implement
  case OP_NRE:
    return false; // todo implement
  default:
    break;
  }
  return false;
}


bool numeric_test(match_node* n, double val)
{
  if (!val && n->value_type == TYPE_NIL) return true;
  if (!val || n->value_type != TYPE_NUMERIC) return false;

  switch (n->op) {
  case OP_EQ:
    return val == n->value.d;
  case OP_NE:
    return val != n->value.d;
  case OP_LT:
    return val < n->value.d;
  case OP_LTE:
    return val <= n->value.d;
  case OP_GT:
    return val > n->value.d;
  case OP_GTE:
    return val > n->value.d;
  default:
    break;
  }
  return false;
}


bool eval_node(match_node* n, hs_heka_message* m)
{
  switch (n->op) {
  case OP_TRUE:
    return true;
  case OP_FALSE:
    return false;
  default:
    switch (n->pbid) {
    case HS_HEKA_UUID:
      string_test(n, m->uuid, 16);
      break;
    case HS_HEKA_TIMESTAMP:
      numeric_test(n, m->timestamp);
      break;
    case HS_HEKA_TYPE:
      string_test(n, m->type, m->type_len);
      break;
    case HS_HEKA_LOGGER:
      string_test(n, m->logger, m->logger_len);
      break;
    case HS_HEKA_SEVERITY:
      numeric_test(n, m->severity);
      break;
    case HS_HEKA_PAYLOAD:
      string_test(n, m->payload, m->payload_len);
      break;
    case HS_HEKA_ENV_VERSION:
      string_test(n, m->env_version, m->env_version_len);
      break;
    case HS_HEKA_PID:
      numeric_test(n, m->pid);
      break;
    case HS_HEKA_HOSTNAME:
      string_test(n, m->hostname, m->hostname_len);
      break;
    default:
      // todo implement field support
      break;
    }
    break;
  }
  return false;
}


bool eval_tree(match_node* n, hs_heka_message* m)
{
  bool match = false;
  if (n->left) {
    match = eval_tree(n->left, m);
  } else {
    match = eval_node(n, m);
  }

  if (match && n->op == OP_OR) {
    return match; // short circuit
  }

  if (!match && n->op == OP_AND) {
    return match; // short circuit
  }

  if (n->right) {
    match = eval_tree(n->right, m);
  }
  return match;
}


void load_op_node(lua_State* L, match_node* mn)
{
  const char* op = lua_tostring(L, -1);
  if (strcmp(op, "||") == 0) {
    mn->op = OP_OR;
  } else if (strcmp(op, "&&") == 0) {
    mn->op = OP_AND;
  } else if (strcmp(op, "TRUE") == 0) {
    mn->op = OP_TRUE;
  } else if (strcmp(op, "FALSE") == 0) {
    mn->op = OP_FALSE;
  } else {
    hs_log(HS_APP_NAME, 0, "message_matcher unknown op: %s", op);
    exit(EXIT_FAILURE);
  }
}


void load_expression_node(lua_State* L, match_node* mn)
{
  size_t len;
  const char* tmp;
  lua_getfield(L, -1, "variable");
  tmp = lua_tolstring(L, -1, &len);
  if (strcmp(tmp, "Uuid") == 0) {
    mn->pbid = HS_HEKA_UUID;
  } else if (strcmp(tmp, "Timestamp") == 0) {
    mn->pbid = HS_HEKA_TIMESTAMP;
  } else if (strcmp(tmp, "Type") == 0) {
    mn->pbid = HS_HEKA_TYPE;
  } else if (strcmp(tmp, "Logger") == 0) {
    mn->pbid = HS_HEKA_LOGGER;
  } else if (strcmp(tmp, "Severity") == 0) {
    mn->pbid = HS_HEKA_SEVERITY;
  } else if (strcmp(tmp, "Payload") == 0) {
    mn->pbid = HS_HEKA_PAYLOAD;
  } else if (strcmp(tmp, "EnvVersion") == 0) {
    mn->pbid = HS_HEKA_ENV_VERSION;
  } else if (strcmp(tmp, "Pid") == 0) {
    mn->pbid = HS_HEKA_PID;
  } else if (strcmp(tmp, "Hostname") == 0) {
    mn->pbid = HS_HEKA_HOSTNAME;
  } else {
    mn->pbid = HS_HEKA_FIELD;
    mn->variable = malloc(len + 1);
    memcpy(mn->variable, tmp, len + 1);
  }
  lua_pop(L, 1);

  lua_getfield(L, -1, "value");
  switch (lua_type(L, -1)) {
  case LUA_TSTRING:
    tmp = lua_tolstring(L, -1, &len);
    mn->value_type = TYPE_STRING;
    mn->value.s = malloc(len + 1);
    memcpy(mn->value.s, tmp, len + 1);
    break;
  case LUA_TNUMBER:
    mn->value_type = TYPE_NUMERIC;
    mn->value.d = lua_tonumber(L, -1);
    break;
  case LUA_TBOOLEAN:
    mn->value_type = TYPE_BOOLEAN;
    mn->value.d = lua_tonumber(L, -1);
    break;
  case LUA_TNIL:
    mn->value_type = TYPE_NIL;
    break;
  default:
    hs_log(HS_APP_NAME, 0, "message_matcher invalid value");
    exit(EXIT_FAILURE);
  }
  lua_pop(L, 1);

  lua_getfield(L, -1, "op");
  tmp = lua_tolstring(L, -1, &len);
  if (strcmp(tmp, "==") == 0) {
    mn->op = OP_EQ;
  } else if (strcmp(tmp, "!=") == 0) {
    mn->op = OP_NE;
  } else if (strcmp(tmp, ">=") == 0) {
    mn->op = OP_GTE;
  } else if (strcmp(tmp, ">") == 0) {
    mn->op = OP_GT;
  } else if (strcmp(tmp, "<=") == 0) {
    mn->op = OP_LTE;
  } else if (strcmp(tmp, "<") == 0) {
    mn->op = OP_LT;
  } else if (strcmp(tmp, "=~") == 0) {
    mn->value_type = TYPE_REGEX;
    mn->op = OP_RE;
  } else if (strcmp(tmp, "!~") == 0) {
    mn->value_type = TYPE_REGEX;
    mn->op = OP_NRE;
  } else {
    hs_log(HS_APP_NAME, 0, "message_matcher invalid op: %s", tmp);
    exit(EXIT_FAILURE);
  }
  lua_pop(L, 1);

  lua_getfield(L, -1, "fi");
  mn->fi = lua_tointeger(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, -1, "ai");
  mn->ai = lua_tointeger(L, -1);
  lua_pop(L, 1);
}


void hs_init_message_match_builder(hs_message_match_builder* mmb,
                                   const char* module_path)
{
  mmb->parser = luaL_newstate();
  if (!mmb->parser) {
    hs_log(HS_APP_NAME, 0, "message_matcher_builder luaL_newstate failed");
    exit(EXIT_FAILURE);
  }

  lua_createtable(mmb->parser, 0, 1);
#if defined(_WIN32)
  lua_pushfstring(mmb->parser, "%s\\?.dll", module_path);
  lua_pushfstring(mmb->parser, "%s\\?.lua", module_path);
#else
  lua_pushfstring(mmb->parser, "%s/?.so", module_path);
  lua_pushfstring(mmb->parser, "%s/?.lua", module_path);
#endif
  lua_setfield(mmb->parser, -3, "path");
  lua_setfield(mmb->parser, -2, "cpath");
  lua_setfield(mmb->parser, LUA_REGISTRYINDEX, "lsb_config");

  // load base module
  lua_pushcfunction(mmb->parser, luaopen_base);
  lua_pushstring(mmb->parser, "");
  lua_call(mmb->parser, 1, 1);
  lua_newtable(mmb->parser);
  lua_setmetatable(mmb->parser, -2);
  lua_pop(mmb->parser, 1);

  // load package module
  lua_pushcfunction(mmb->parser, luaopen_package);
  lua_pushstring(mmb->parser, LUA_LOADLIBNAME);
  lua_call(mmb->parser, 1, 1);
  lua_newtable(mmb->parser);
  lua_setmetatable(mmb->parser, -2);
  lua_pop(mmb->parser, 1);

  if (luaL_dostring(mmb->parser, grammar)) {
    hs_log(HS_APP_NAME, 0, "message_matcher_grammar error: %s",
           lua_tostring(mmb->parser, -1));
    exit(EXIT_FAILURE);
  }
}


void hs_free_message_match_builder(hs_message_match_builder* mmb)
{
  if (mmb->parser) {
    lua_close(mmb->parser);
    mmb->parser = NULL;
  }
}


hs_message_matcher* hs_create_message_matcher(hs_message_match_builder* mmb, const char* exp)
{
  lua_getglobal(mmb->parser, "parse");
  if (!lua_isfunction(mmb->parser, -1)) {
    hs_log(HS_APP_NAME, 0, "message_matcher error: %s",
           lua_tostring(mmb->parser, -1));
    exit(EXIT_FAILURE);
  }
  lua_pushstring(mmb->parser, exp);
  if (lua_pcall(mmb->parser, 1, 2, 0)) {
    hs_log(HS_APP_NAME, 0, "message_matcher error: %s",
           lua_tostring(mmb->parser, -1));
    exit(EXIT_FAILURE);
  }

  if (lua_type(mmb->parser, 1) != LUA_TTABLE) {
    hs_log(HS_APP_NAME, 4, "message_matcher parse failed: %s", exp);
    return NULL;
  }
  int size = lua_tointeger(mmb->parser, 2);

  hs_message_matcher* mm = calloc(sizeof(hs_message_matcher) +
                                  (sizeof(match_node) * size), 1);

  // load in reverse order so the root node will be first
  for (int i = size, j = 0; i > 0; --i, ++j) {
    lua_rawgeti(mmb->parser, 1, i);
    switch (lua_type(mmb->parser, -1)) {
    case LUA_TSTRING:
      load_op_node(mmb->parser, &mm->nodes[j]);
      break;
    case LUA_TTABLE:
      load_expression_node(mmb->parser, &mm->nodes[j]);
      break;
    default:
      hs_log(HS_APP_NAME, 0, "message_matcher error: invalid table returned");
      exit(EXIT_FAILURE);
    }
    lua_pop(mmb->parser, 1);
  }
  lua_pop(mmb->parser, 2);

  // turn the postfix stack into a executable tree
  match_node** stack = malloc(sizeof(match_node*) * size);
  if (!stack) {
    hs_log(HS_APP_NAME, 0, "message_matcher stack allocation failed");
    exit(EXIT_FAILURE);
  }

  int top = 0;
  for (int i = size - 1; i >= 0; --i) {
    if (mm->nodes[i].op != OP_AND && mm->nodes[i].op != OP_OR) {
      stack[top++] = &mm->nodes[i];
    } else {
      mm->nodes[i].right = stack[top--];
      mm->nodes[i].left = stack[top];
      stack[top] = &mm->nodes[i];
    }
  }
  free(stack);

  return mm;
}


void hs_free_message_matcher(hs_message_matcher* mm)
{
  for (int i = 0; i < mm->nodes_size; ++i) {
    free(mm->nodes[i].variable);
    switch (mm->nodes[i].value_type) {
    case TYPE_STRING:
      free(mm->nodes[i].value.s);
      break;
    case TYPE_REGEX:
      // todo
      break;
    default:
      // no action required
      break;
    }
  }
}


bool hs_eval_message_matcher(hs_message_matcher* mm, hs_heka_message* m)
{
  return eval_tree(mm->nodes, m);
}
