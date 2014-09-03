/*
 * the manifest file format: key = value. parser ignores white spaces
 * each line can only contain single key=value
 *
 * the api is very simple: just call GetValuByKey() after
 * manifest is constructed
 *
 * Copyright (c) 2012, LiteStack, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * manifest parser. input: manifest file name. output: manifest structure
 * TODO(d'b): since networking removed from zerovm, all net stuff need no
 * more and must be removed (addresses, nodes, server e.t.c. just the string
 * which are part of broker's api)
 */
#include <stdio.h>
#include <assert.h>
#include <sys/un.h>
#include <arpa/inet.h> /* convert ip to int */
#include "src/main/manifest.h"
#include "src/main/etag.h"
#include "src/main/zlog.h"
#include "src/channels/channel.h"

#define MFTFAIL ZLogTag("MANIFEST", cline), FailIf

/* general */
#define MIN_MFT_SIZE 124 /* can be calculated, but it will be complex */
#define MANIFEST_VERSION "20140509"
#define MANIFEST_SIZE_LIMIT 0x80000
#define MANIFEST_LINES_LIMIT 0x2000
#define MANIFEST_TOKENS_LIMIT 0x10
#define NAME_PREFIX_NET "opaque"

/* DEPRECATED. API version 1. channel definition contains extra field etag */
#ifndef REMOVE_DEPRECATED
#define OLD_VERSION "20130611"
#endif

/* delimiters */
#define LINE_DELIMITER "\n"
#define KEY_DELIMITER "="
#define VALUE_DELIMITER ","

/* key/value tokens */
typedef enum {
  Key,
  Value,
  KeyValueTokensNumber
} KeyValueTokens;

/* memory tokens */
typedef enum {
  MemorySize,
  MemoryTag,
  MemoryTokensNumber
} MemoryTokens;

/* DEPRECATED. API version 1. channel definition contains extra field etag */
#ifndef REMOVE_DEPRECATED
/* deprecated channel tokens */
typedef enum {
  OldName,
  OldAlias,
  OldType,
  OldEtag, /* will be ignored */
  OldGets,
  OldGetSize,
  OldPuts,
  OldPutSize,
  OldChannelTokensNumber
} OldChannelTokens;
#endif

/* channel tokens */
typedef enum {
  Name,
  Alias,
  Type,
  Gets,
  GetSize,
  Puts,
  PutSize,
  ChannelTokensNumber
} ChannelTokens;

/* DEPRECATED. API version 1. includes removed "Program" */
#ifndef REMOVE_DEPRECATED
/* (x-macro): manifest keywords (name, obligatory, singleton) */
#define KEYWORDS \
  X(Channel, 1, 0) \
  X(Version, 1, 1) \
  X(Boot, 0, 1) \
  X(Program, 0, 1) \
  X(Memory, 1, 1) \
  X(Timeout, 1, 1) \
  X(Node, 0, 1) \
  X(Job, 0, 1) \
  X(Broker, 0, 1)
#else
/* (x-macro): manifest keywords (name, obligatory, singleton) */
#define KEYWORDS \
  X(Channel, 1, 0) \
  X(Version, 1, 1) \
  X(Boot, 0, 1) \
  X(Memory, 1, 1) \
  X(Timeout, 1, 1) \
  X(Node, 0, 1) \
  X(Job, 0, 1) \
  X(Broker, 0, 1)
#endif

/* (x-macro): manifest enumeration, array and statistics */
#define XENUM(a) enum ENUM_##a {a};
#define X(a, o, s) Key ## a,
  XENUM(KEYWORDS)
#undef X

#define XARRAY(a) static char *ARRAY_##a[] = {a};
#define X(a, o, s) #a,
  XARRAY(KEYWORDS)
#undef X

#define XCHECK(a) static uint8_t CHECK_##a[] = {a};
#define X(a, o, s) o | (s << 1),
  XCHECK(KEYWORDS)
#undef X

/*
 * LOCAL variables and functions
 */
static int cline = 0;

/* DEPRECATED. API version 1 */
#ifndef REMOVE_DEPRECATED
static int deprecated = 0;
#endif

int64_t ToInt(char *a)
{
  int64_t result;
  char dbg[BIG_ENOUGH_STRING];

  ZLOGFAIL(a == NULL, ZERR_MFT_DEF, "empty numeric value '%s'", a);

  errno = 0;
  a = g_strstrip(a);
  strncpy(dbg, a, ARRAY_SIZE(dbg));

  result = g_ascii_strtoll(a, &a, 0);
  ZLOGFAIL(*a != '\0' || errno != 0, ZERR_MFT_DEF,
      "invalid numeric value '%s'", dbg);

  return result;
}

/*
 * return keyword and update given "value" with extracted value
 * spaces will be trimmed from "value"
 */
static XTYPE(KEYWORDS) GetKey(char *key)
{
  XTYPE(KEYWORDS) k;

  key = g_strstrip(key);
  for(k = 0; k < XSIZE(KEYWORDS); ++k)
    if(g_strcmp0(XSTR(KEYWORDS, k), key) == 0) return k;
  return -1;
}

#ifndef REMOVE_DEPRECATED
/* test manifest version */
static void Version(struct Manifest *manifest, char *value)
{
  manifest->version = g_strdup(g_strstrip(value));
  if(g_strcmp0(MANIFEST_VERSION, manifest->version) != 0)
    if(g_strcmp0(OLD_VERSION, manifest->version) != 0)
      MFTFAIL(1, ZERR_MFT_DEF, "invalid manifest version");
}
#else
/* test manifest version */
static void Version(struct Manifest *manifest, char *value)
{
  MFTFAIL(g_strcmp0(MANIFEST_VERSION, g_strstrip(value)) != 0,
      ZERR_MFT_DEF, "invalid manifest version");
}
#endif

/* set program field (should be g_free later) */
static void Boot(struct Manifest *manifest, char *value)
{
  manifest->boot = g_strdup(g_strstrip(value));
}

/* DEPRECATED. API version 1. includes removed "Program" */
#ifndef REMOVE_DEPRECATED
static void Program(struct Manifest *manifest, char *value)
{
  struct ChannelDesc *channel = g_malloc0(sizeof *channel);

  /* set "/boot/elf" fields */
  channel->alias = g_strdup("/boot/elf");
  channel->limits[GetsLimit] = 0x1000000;
  channel->limits[GetSizeLimit] = 0x1000000;
  channel->name = g_strdup(g_strstrip(value));
  channel->type = RGetSPut;

  /* add it to user channels */
  g_ptr_array_add(manifest->channels, channel);
}
#endif

/* set mem_size and mem_tag field */
static void Memory(struct Manifest *manifest, char *value)
{
  char **tokens;
  int tag;

  /* parse value */
  tokens = g_strsplit(value, VALUE_DELIMITER, MemoryTokensNumber);
  MFTFAIL(tokens[MemoryTokensNumber] != NULL || tokens[MemoryTag] == NULL,
      ZERR_MFT_STX, "invalid memory token");

  manifest->mem_size = ToInt(tokens[MemorySize]);
  tag = ToInt(tokens[MemoryTag]);

  /* initialize manifest field */
  MFTFAIL(tag != 0 && tag != 1, ZERR_MFT_DEF, "invalid memory etag token");

  manifest->mem_tag = tag == 0 ? NULL : TagCtor();
  g_strfreev(tokens);
}

static void Timeout(struct Manifest *manifest, char *value)
{
  manifest->timeout = ToInt(value);
}

static void Node(struct Manifest *manifest, char *value)
{
  manifest->node = g_strdup(g_strstrip(value));
}

static void Broker(struct Manifest *manifest, char *value)
{
  manifest->broker = g_strdup(g_strstrip(value));
}

static void Job(struct Manifest *manifest, char *value)
{
  MFTFAIL(strlen(value) > UNIX_PATH_MAX, ZERR_MFT_DEF, "too long Job path");
  manifest->job = g_strdup(g_strstrip(value));
}

/*
 * DEPRECATED. API version 1. channel definition contains extra field etag
 * the code contains doubling. hopefully deprecated API/ABI/manifest support
 * will be removed soon together with this code, otherwise zerovm code will
 * be re-factored
 */
#ifndef REMOVE_DEPRECATED
static int CountTokens(char **tokens)
{
  int i;
  for(i = 0; tokens[i] != NULL; ++i)
    ;
  return i;
}
/* old channel parser */
static void OldChannel(struct Manifest *manifest, char **tokens)
{
  int i;
  struct ChannelDesc *channel = g_malloc0(sizeof *channel);

  MFTFAIL(CountTokens(tokens) != OldChannelTokensNumber,
      ZERR_MFT_STX, "invalid channel tokens number");

  /* strip passed tokens */
  for(i = 0; tokens[i] != NULL; ++i)
    g_strstrip(tokens[i]);

  /*
   * set name and protocol (network or local). exact protocol
   * for the local channel will be set later
   */
  if(g_path_is_absolute(tokens[OldName]))
  {
    channel->protocol = ProtoRegular;
    channel->name = g_strdup(tokens[OldName]);
  }
  else
  {
    MFTFAIL(g_str_has_prefix(tokens[OldName], NAME_PREFIX_NET) == 0,
        ZERR_MFT_DEF, "only absolute path channels are allowed");
    channel->protocol = ProtoOpaque;
    channel->name = g_strdup(tokens[OldName] + sizeof NAME_PREFIX_NET);
  }

  /* initialize the rest of fields */
  channel->alias = g_strdup(tokens[OldAlias]);
  channel->handle = NULL;
  channel->type = ToInt(tokens[OldType]);

  /* parse limits */
  for(i = 0; i < LimitsNumber; ++i)
  {
    channel->limits[i] = ToInt(tokens[i + OldGets]);
    MFTFAIL(channel->limits[i] < 0, ZERR_MFT_DEF,
        "negative limits for %s", channel->alias);
  }

  /* append a new channel */
  g_ptr_array_add(manifest->channels, channel);
  g_strfreev(tokens);
}

/* new channel parser */
static void Channel(struct Manifest *manifest, char *value)
{
  int i;
  char **tokens;
  struct ChannelDesc *channel = g_malloc0(sizeof *channel);

  /* get tokens from channel description */
  tokens = g_strsplit(value, VALUE_DELIMITER, MANIFEST_TOKENS_LIMIT);

  /* deprecated manifest? than switch to old channel parser */
  if(CountTokens(tokens) != ChannelTokensNumber)
  {
    deprecated = 1;
    OldChannel(manifest, tokens);
    return;
  }

  MFTFAIL(deprecated, ZERR_MFT_STX, "mixed version manifest detected");
  for(i = 0; tokens[i] != NULL; ++i)
    g_strstrip(tokens[i]);

  /*
   * set name and protocol (network or local). exact protocol
   * for the local channel will be set later
   */
  if(g_path_is_absolute(tokens[OldName]))
  {
    channel->protocol = ProtoRegular;
    channel->name = g_strdup(tokens[OldName]);
  }
  else
  {
    MFTFAIL(g_str_has_prefix(tokens[OldName], NAME_PREFIX_NET) == 0,
        ZERR_MFT_STX, "only absolute path channels are allowed");
    channel->protocol = ProtoOpaque;
    channel->name = g_strdup(tokens[OldName] + sizeof NAME_PREFIX_NET);
  }

  /* initialize the rest of fields */
  channel->alias = g_strdup(tokens[Alias]);
  channel->handle = NULL;
  channel->type = ToInt(tokens[Type]);

  /* parse limits */
  for(i = 0; i < LimitsNumber; ++i)
  {
    channel->limits[i] = ToInt(tokens[i + Gets]);
    MFTFAIL(channel->limits[i] < 0, ZERR_MFT_DEF,
        "negative limits for %s", channel->alias);
  }

  /* append a new channel */
  g_ptr_array_add(manifest->channels, channel);
  g_strfreev(tokens);
}
#else
/* set channels field */
static void Channel(struct Manifest *manifest, char *value)
{
  int i;
  char **tokens;
  struct ChannelDesc *channel = g_malloc0(sizeof *channel);

  /* get tokens from channel description */
  tokens = g_strsplit(value, VALUE_DELIMITER, ChannelTokensNumber + 1);
  for(i = 0; tokens[i] != NULL; ++i)
    g_strstrip(tokens[i]);

  MFTFAIL(tokens[ChannelTokensNumber] != NULL || tokens[PutSize] == NULL,
      ZERR_MFT_STX, "invalid channel tokens number");

  /*
   * set name and protocol (network or local). exact protocol
   * for the local channel will be set later
   */
  if(g_path_is_absolute(tokens[OldName]))
  {
    channel->protocol = ProtoRegular;
    channel->name = g_strdup(tokens[OldName]);
  }
  else
  {
    MFTFAIL(g_str_has_prefix(tokens[OldName], NAME_PREFIX_NET) == 0,
        ZERR_MFT_DEF, "only absolute path channels are allowed");
    channel->protocol = ProtoOpaque;
    channel->name = g_strdup(tokens[OldName] + sizeof NAME_PREFIX_NET);
  }

  /* initialize the rest of fields */
  channel->alias = g_strdup(tokens[Alias]);
  channel->handle = NULL;
  channel->type = ToInt(tokens[Type]);

  /* parse limits */
  for(i = 0; i < LimitsNumber; ++i)
  {
    channel->limits[i] = ToInt(tokens[i + Gets]);
    MFTFAIL(channel->limits[i] < 0, ZERR_MFT_DEF,
        "negative limits for %s", channel->alias);
  }

  /* append a new channel */
  g_ptr_array_add(manifest->channels, channel);
  g_strfreev(tokens);
}
#endif

/*
 * check if obligatory keywords appeared and check if the fields
 * which should appear only once did so
 */
static void CheckCounters(int *counters, int n)
{
  while(--n)
  {
    ZLOGFAIL(counters[n] == 0 && (CHECK_KEYWORDS[n] & 1),
        ZERR_MFT_STX, "%s not specified", XSTR(KEYWORDS, n));
    ZLOGFAIL(counters[n] > 1 && (CHECK_KEYWORDS[n] & 2),
        ZERR_MFT_STX, "duplicate %s keyword", XSTR(KEYWORDS, n));
  }
}

struct Manifest *ManifestTextCtor(char *text)
{
  struct Manifest *manifest = g_malloc0(sizeof *manifest);
  int counters[XSIZE(KEYWORDS)] = {0};
  char **lines;
  int i;

  /* initialize channels */
  manifest->channels = g_ptr_array_new();

  /* extract all lines */
  lines = g_strsplit(text, LINE_DELIMITER, MANIFEST_LINES_LIMIT);

  /* parse each line */
  for(i = 0; lines[i] != NULL; ++i)
  {
    char **tokens;
    cline = i + 1;

    tokens = g_strsplit(lines[i], KEY_DELIMITER, MANIFEST_TOKENS_LIMIT);
    if(strlen(lines[i]) > 0 && tokens[Key] != NULL
        && tokens[Value] != NULL && tokens[KeyValueTokensNumber] == NULL)
    {
      /* switch invoking functions by the keyword */
#define XSWITCH(a) switch(GetKey(tokens[Key])) {a};
#define X(a, o, s) case Key##a: ++counters[Key##a]; a(manifest, tokens[Value]); \
    MFTFAIL(*tokens[Value] == 0, ZERR_MFT_STX, "%s has no value", tokens[Key]); break;
      XSWITCH(KEYWORDS)
#undef X
    }
    g_strfreev(tokens);
  }

  /* check obligatory and singleton keywords */
  g_strfreev(lines);
  CheckCounters(counters, XSIZE(KEYWORDS));

  /* "Broker" depends on "Node" existence */
  ZLOGFAIL(manifest->broker != NULL && manifest->node == NULL,
      ZERR_MFT_STX, "broker specified but node is absent");

  return manifest;
}

struct Manifest *ManifestCtor(const char *name)
{
  int size;
  char buf[MANIFEST_SIZE_LIMIT] = {0};
  FILE *h = fopen(name, "r");

  ZLOGFAIL(h == NULL, ZERR_IO, "manifest: %s", strerror(errno));
  size = fread(buf, 1, MANIFEST_SIZE_LIMIT, h);
  ZLOGFAIL(size < 1, ZERR_IO, "manifest: %s", strerror(errno));
  ZLOGFAIL(size < MIN_MFT_SIZE, ZERR_MFT_STX, "manifest is too small");
  fclose(h);

  return ManifestTextCtor(buf);
}

void ManifestDtor(struct Manifest *manifest)
{
  int i;

  if(manifest == NULL) return;

  /* channels */
  for(i = 0; i < manifest->channels->len; ++i)
  {
    struct ChannelDesc *channel = g_ptr_array_index(manifest->channels, i);

    g_free(channel->alias);
    g_free(channel->name);
    g_free(channel);
  }
  g_ptr_array_free(manifest->channels, TRUE);

  /* other */
  g_free(manifest->broker);
  TagDtor(manifest->mem_tag);
  g_free(manifest->boot);
  g_free(manifest);
}

struct Command *CommandPtr()
{
  static struct Command command;
  return &command;
}
