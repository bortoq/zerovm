/*
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
 *
 * TODO(d'b): add byte order to (de)serializer
 */

#include <assert.h>
#include "src/main/zlog.h"
#include "src/main/manifest.h"
#include "src/channels/channel.h"
#include "src/channels/serializer.h"

#define GETSTATS(c) ((void*)(is_mft ? (c)->limits : (c)->counters))

/* copy name to names area */
INLINE static uint32_t CopyName(char *target, char *name)
{
  strcpy(target, name);
  return 1 + strlen(name); /* byte for ending '\0' */
}

/* copy channel limits or counts (for snapshot) */
INLINE static void CopyStats(int64_t *dst, int64_t *src)
{
  dst[GetsLimit] = src[GetsLimit];
  dst[GetSizeLimit] = src[GetSizeLimit];
  dst[PutsLimit] = src[PutsLimit];
  dst[PutSizeLimit] = src[PutSizeLimit];
}

/*
 * 1. for snapshot "alias" should have different addresses start
 * 2. snapshot and manifest has different meaning of "limits"
 * offset is: 0xfffe0000 = user manifest, 0 = snapshot
 * note: struct ChannelsSerial "size" field made 64-bit intentionally to
 *       avoid "alignement" problem
 */
struct ChannelsSerial *ChannelsSerializer(
    const struct Manifest *manifest, uintptr_t offset)
{
  struct ChannelRec *record;
  struct ChannelsSerial *buffer;
  char *names_pos;
  char *names;
  int i;
  int is_mft = offset ? 1 : 0;

  assert(manifest != NULL);

  /* calculate and allocated buffer */
  i = manifest->channels->len * (CHANNEL_NAME_MAX + sizeof *record);
  buffer = g_malloc(i + sizeof buffer->size);
  ZLOGFAIL(buffer == NULL, ZERR_MEM, "cannot allocate space for channels");

  /* set name area pointer */
  i = manifest->channels->len * sizeof *record + sizeof buffer->size;
  names = (char*)buffer + i;
  names_pos = names;

  /* recalculate offset */
  offset += manifest->channels->len * sizeof *record;

  /* loop through all channels and serialize them into buffer */
  for(i = 0; i < manifest->channels->len; ++i)
  {
    /* set type and limits (or counters) */
    buffer->channels[i].type = CHANNEL(manifest, i)->type;
    CopyStats(buffer->channels[i].limits, GETSTATS(CHANNEL(manifest, i)));

    /* set size */
    if(is_mft)
      buffer->channels[i].size =
        CH_RND_READABLE(CHANNEL(manifest, i)) || CH_RND_WRITEABLE(CHANNEL(manifest, i))
        ? CHANNEL(manifest, i)->size : 0;
    else
      buffer->channels[i].size = CHANNEL(manifest, i)->size;

    /* copy alias to names area */
    buffer->channels[i].name = (uintptr_t)names_pos - (uintptr_t)names;
    buffer->channels[i].name += offset;
    names_pos += CopyName(names_pos, CHANNEL(manifest, i)->alias);
  }

  /* set serialized channels size and align buffer */
  buffer->size = (uintptr_t)names_pos - (uintptr_t)buffer->channels;
  return g_realloc(buffer, (uintptr_t)names_pos - (uintptr_t)buffer);
}

int ChannelsDeserializer(struct Manifest *manifest, struct ChannelRec *channels)
{
  int i;

  /*
   * TODO(d'b): should be taken from arguments. for now only use case is
   * snapshot. however it would be nice to have generic solution
   */
  int is_mft = 0;
  char *name;

  assert(manifest != NULL);
  assert(channels != NULL);

  /* populate manifest with channels data */
  /* TODO(d'b): need solution to make both channels list to be sorted */
  name = TOPTR(channels) + manifest->channels->len * sizeof *channels;
  for(i = 0; i < manifest->channels->len; ++i)
  {
    /* ckeck alias */
    if(g_strcmp0(CHANNEL(manifest, i)->alias, name) != 0)
      return -1;

    /* rewind to the next alias */
    name += strlen(name);
    ++name; /* skip ending '\0' */

    /* copy fields */
    CopyStats(GETSTATS(CHANNEL(manifest, i)), channels[i].limits);
    CHANNEL(manifest, i)->size = channels[i].size;
    CHANNEL(manifest, i)->type = channels[i].type;
  }

  return 0;
}
