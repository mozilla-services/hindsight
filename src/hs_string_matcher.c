/*
** Modified Lua lstrlib.c for the Hindsight message matcher pattern-matching
*
* Copyright (C) 1994-2012 Lua.org, PUC-Rio.  All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining
* a copy of this software and associated documentation files (the
* "Software"), to deal in the Software without restriction, including
* without limitation the rights to use, copy, modify, merge, publish,
* distribute, sublicense, and/or sell copies of the Software, and to
* permit persons to whom the Software is furnished to do so, subject to
* the following conditions:
*
* The above copyright notice and this permission notice shall be
* included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
******************************************************************************/

#include "hs_string_matcher.h"

#include <ctype.h>

/* macro to `unsign' a character */
#define uchar(c)        ((unsigned char)(c))

typedef struct MatchState {
  const char* src_init;  /* init of source string */
  const char* src_end;  /* end (`\0') of source string */
} MatchState;


#define L_ESC   '%'
#define SPECIALS  "^$*+?.([%-"


static const char* classend(const char* p)
{
  switch (*p++) {
  case L_ESC:
    {
      if (*p == '\0') return NULL; // error pattern ends with a %
      return p + 1;
    }
  case '[':
    {
      if (*p == '^') p++;
      do {  /* look for a `]' */
        if (*p == '\0') return NULL; // error missing closing ]
        if (*(p++) == L_ESC && *p != '\0') p++;  /* skip escapes (e.g. `%]') */
      }
      while (*p != ']');
      return p + 1;
    }
  default:
    {
      return p;
    }
  }
}


static int match_class(int c, int cl)
{
  int res;
  switch (tolower(cl)) {
  case 'a' :
    res = isalpha(c); break;
  case 'c' :
    res = iscntrl(c); break;
  case 'd' :
    res = isdigit(c); break;
  case 'l' :
    res = islower(c); break;
  case 'p' :
    res = ispunct(c); break;
  case 's' :
    res = isspace(c); break;
  case 'u' :
    res = isupper(c); break;
  case 'w' :
    res = isalnum(c); break;
  case 'x' :
    res = isxdigit(c); break;
  case 'z' :
    res = (c == 0); break;
  default:
    return (cl == c);
  }
  return (islower(cl) ? res : !res);
}


static int matchbracketclass(int c, const char* p, const char* ec)
{
  int sig = 1;
  if (*(p + 1) == '^') {
    sig = 0;
    p++;  /* skip the `^' */
  }
  while (++p < ec) {
    if (*p == L_ESC) {
      p++;
      if (match_class(c, uchar(*p))) return sig;
    } else if ((*(p + 1) == '-') && (p + 2 < ec)) {
      p += 2;
      if (uchar(*(p - 2)) <= c && c <= uchar(*p)) return sig;
    } else if (uchar(*p) == c) return sig;
  }
  return !sig;
}


static int singlematch(int c, const char* p, const char* ep)
{
  switch (*p) {
  case '.':
    return 1;  /* matches any char */
  case L_ESC:
    return match_class(c, uchar(*(p + 1)));
  case '[':
    return matchbracketclass(c, p, ep - 1);
  default:
    return (uchar(*p) == c);
  }
}


static const char* match(MatchState* ms, const char* s, const char* p);


static const char* matchbalance(MatchState* ms, const char* s,
                                const char* p)
{
  if (*p == 0 || *(p + 1) == 0) return NULL; // ubalanced pattern;
  if (*s != *p) return NULL;
  else {
    int b = *p;
    int e = *(p + 1);
    int cont = 1;
    while (++s < ms->src_end) {
      if (*s == e) {
        if (--cont == 0) return s + 1;
      } else if (*s == b) cont++;
    }
  }
  return NULL;  /* string ends out of balance */
}


static const char* max_expand(MatchState* ms, const char* s,
                              const char* p, const char* ep)
{
  ptrdiff_t i = 0;  /* counts maximum expand for item */
  while ((s + i) < ms->src_end && singlematch(uchar(*(s + i)), p, ep)) i++;
  /* keeps trying to match with the maximum repetitions */
  while (i >= 0) {
    const char* res = match(ms, (s + i), ep + 1);
    if (res) return res;
    i--;  /* else didn't match; reduce 1 repetition to try again */
  }
  return NULL;
}


static const char* min_expand(MatchState* ms, const char* s,
                              const char* p, const char* ep)
{
  for (;;) {
    const char* res = match(ms, s, ep + 1);
    if (res != NULL) return res;
    else if (s < ms->src_end && singlematch(uchar(*s), p, ep)) s++;  /* try with one more repetition */
    else return NULL;
  }
}


static const char* match(MatchState* ms, const char* s, const char* p)
{
init: /* using goto's to optimize tail recursion */
  switch (*p) {
  case L_ESC:
    {
      switch (*(p + 1)) {
      case 'b':
        {  /* balanced string? */
          s = matchbalance(ms, s, p + 2);
          if (s == NULL) return NULL;
          p += 4; goto init;  /* else return match(ms, s, p+4); */
        }
      case 'f':
        {  /* frontier? */
          const char* ep; char previous;
          p += 2;
          if (*p != '[') return NULL; // missing [ after %f
          ep = classend(p);  /* points to what is next */
          if (ep == NULL) return NULL;
          previous = (s == ms->src_init) ? '\0' : *(s - 1);
          if (matchbracketclass(uchar(previous), p, ep - 1) ||
              !matchbracketclass(uchar(*s), p, ep - 1)) return NULL;
          p = ep; goto init;  /* else return match(ms, s, ep); */
        }
      default:
        {
          goto dflt;  /* case default */
        }
      }
    }
  case '\0':
    {  /* end of pattern */
      return s;  /* match succeeded */
    }
  case '$':
    {
      if (*(p+1)== '\0')  /* is the `$' the last char in pattern? */
      return (s == ms->src_end)? s : NULL;  /* check end of string */
      else goto dflt;
    }
  default:
    dflt: {  /* it is a pattern item */
      const char *ep = classend(p);  /* points to what is next */
      if (ep == NULL) return NULL;

      int m = s < ms->src_end && singlematch(uchar(*s), p, ep);
      switch (*ep){
      case '?':
        {  /* optional */
          const char *res;
          if (m &&((res = match(ms, s+1, ep+1))!= NULL))
          return res;
          p = ep+1; goto init;  /* else return match(ms, s, ep+1); */
        }
      case '*':
        {  /* 0 or more repetitions */
          return max_expand(ms, s, p, ep);
        }
      case '+':
        {  /* 1 or more repetitions */
          return (m ? max_expand(ms, s+1, p, ep): NULL);
        }
      case '-':
        {  /* 0 or more repetitions (minimum) */
          return min_expand(ms, s, p, ep);
        }
      default:
        {
          if (!m)return NULL;
          s++; p = ep; goto init;  /* else return match(ms, s+1, ep); */
        }
      }
    }
  }
}


bool hs_string_match(const char* s, size_t slen, const char* p)
{
  MatchState ms;
  int anchor = (*p == '^') ? (p++, 1) : 0;
  const char* s1 = s;
  ms.src_init = s;
  ms.src_end = s + slen;
  do {
    const char* res;
    if ((res = match(&ms, s1, p)) != NULL) {
      return true;
    }
  } while (s1++ < ms.src_end && !anchor);
  return false;
}
