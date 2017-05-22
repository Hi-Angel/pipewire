/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/timerfd.h>

#include <spa/type-map.h>
#include <spa/clock.h>
#include <spa/log.h>
#include <spa/loop.h>
#include <spa/node.h>
#include <spa/param-alloc.h>
#include <spa/list.h>
#include <spa/format-builder.h>
#include <lib/props.h>

typedef struct {
  uint32_t node;
  uint32_t clock;
  uint32_t format;
  uint32_t props;
  uint32_t prop_live;
  uint32_t prop_pattern;
  SpaTypeMeta meta;
  SpaTypeData data;
  SpaTypeEventNode event_node;
  SpaTypeCommandNode command_node;
  SpaTypeParamAllocBuffers param_alloc_buffers;
  SpaTypeParamAllocMetaEnable param_alloc_meta_enable;
} Type;

static inline void
init_type (Type *type, SpaTypeMap *map)
{
  type->node = spa_type_map_get_id (map, SPA_TYPE__Node);
  type->clock = spa_type_map_get_id (map, SPA_TYPE__Clock);
  type->format = spa_type_map_get_id (map, SPA_TYPE__Format);
  type->props = spa_type_map_get_id (map, SPA_TYPE__Props);
  type->prop_live = spa_type_map_get_id (map, SPA_TYPE_PROPS__live);
  type->prop_pattern = spa_type_map_get_id (map, SPA_TYPE_PROPS__patternType);
  spa_type_meta_map (map, &type->meta);
  spa_type_data_map (map, &type->data);
  spa_type_event_node_map (map, &type->event_node);
  spa_type_command_node_map (map, &type->command_node);
  spa_type_param_alloc_buffers_map (map, &type->param_alloc_buffers);
  spa_type_param_alloc_meta_enable_map (map, &type->param_alloc_meta_enable);
}

typedef struct _SpaFakeSrc SpaFakeSrc;

typedef struct {
  bool live;
  uint32_t pattern;
} SpaFakeSrcProps;

#define MAX_BUFFERS 16
#define MAX_PORTS 1

typedef struct _Buffer Buffer;

struct _Buffer {
  SpaBuffer *outbuf;
  bool outstanding;
  SpaMetaHeader *h;
  SpaList link;
};

struct _SpaFakeSrc {
  SpaHandle handle;
  SpaNode node;
  SpaClock clock;

  Type type;
  SpaTypeMap *map;
  SpaLog *log;
  SpaLoop *data_loop;

  uint8_t props_buffer[512];
  SpaFakeSrcProps props;

  SpaNodeCallbacks callbacks;
  void *user_data;

  SpaSource timer_source;
  struct itimerspec timerspec;

  SpaPortInfo info;
  uint8_t params_buffer[1024];
  SpaPortIO *io;

  bool have_format;
  uint8_t format_buffer[1024];

  Buffer buffers[MAX_BUFFERS];
  uint32_t  n_buffers;

  bool started;
  uint64_t start_time;
  uint64_t elapsed_time;

  uint64_t buffer_count;
  SpaList empty;
  bool underrun;
};

#define CHECK_PORT_NUM(this,d,p)  ((d) == SPA_DIRECTION_OUTPUT && (p) < MAX_PORTS)
#define CHECK_PORT(this,d,p)      (CHECK_PORT_NUM(this,d,p) && this->io)

#define DEFAULT_LIVE false
#define DEFAULT_PATTERN 0

static void
reset_fakesrc_props (SpaFakeSrc *this, SpaFakeSrcProps *props)
{
  props->live = DEFAULT_LIVE;
  props->pattern = DEFAULT_PATTERN;
}

#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)                                                 \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)                                               \
          SPA_POD_PROP (f,key, SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)                                             \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)

static SpaResult
spa_fakesrc_node_get_props (SpaNode       *node,
                            SpaProps     **props)
{
  SpaFakeSrc *this;
  SpaPODBuilder b = { NULL,  };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (props != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_pod_builder_init (&b, this->props_buffer, sizeof (this->props_buffer));
  spa_pod_builder_props (&b, &f[0], this->type.props,
    PROP    (&f[1], this->type.prop_live,      SPA_POD_TYPE_BOOL, this->props.live),
    PROP_EN (&f[1], this->type.prop_pattern,   SPA_POD_TYPE_ID,  1,
                                                        this->props.pattern,
                                                        this->props.pattern));

  *props = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaProps);

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_set_props (SpaNode         *node,
                            const SpaProps  *props)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  if (props == NULL) {
    reset_fakesrc_props (this, &this->props);
  } else {
    spa_props_query (props,
        this->type.prop_live,     SPA_POD_TYPE_BOOL,   &this->props.live,
        this->type.prop_pattern,  SPA_POD_TYPE_ID,     &this->props.pattern,
        0);
  }

  if (this->props.live)
    this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;
  else
    this->info.flags &= ~SPA_PORT_INFO_FLAG_LIVE;

  return SPA_RESULT_OK;
}

static SpaResult
fill_buffer (SpaFakeSrc *this, Buffer *b)
{
  return SPA_RESULT_OK;
}

static void
set_timer (SpaFakeSrc *this, bool enabled)
{
  if (this->callbacks.have_output || this->props.live) {
    if (enabled) {
      if (this->props.live) {
        uint64_t next_time = this->start_time + this->elapsed_time;
        this->timerspec.it_value.tv_sec = next_time / SPA_NSEC_PER_SEC;
        this->timerspec.it_value.tv_nsec = next_time % SPA_NSEC_PER_SEC;
      } else {
        this->timerspec.it_value.tv_sec = 0;
        this->timerspec.it_value.tv_nsec = 1;
      }
    } else {
      this->timerspec.it_value.tv_sec = 0;
      this->timerspec.it_value.tv_nsec = 0;
    }
    timerfd_settime (this->timer_source.fd, TFD_TIMER_ABSTIME, &this->timerspec, NULL);
  }
}

static inline void
read_timer (SpaFakeSrc *this)
{
  uint64_t expirations;

  if (this->callbacks.have_output || this->props.live) {
    if (read (this->timer_source.fd, &expirations, sizeof (uint64_t)) < sizeof (uint64_t))
      perror ("read timerfd");
  }
}

static SpaResult
fakesrc_make_buffer (SpaFakeSrc *this)
{
  Buffer *b;
  SpaPortIO *io = this->io;
  int n_bytes;

  read_timer (this);

  if (spa_list_is_empty (&this->empty)) {
    set_timer (this, false);
    this->underrun = true;
    spa_log_error (this->log, "fakesrc %p: out of buffers", this);
    return SPA_RESULT_OUT_OF_BUFFERS;
  }
  b = spa_list_first (&this->empty, Buffer, link);
  spa_list_remove (&b->link);
  b->outstanding = true;

  n_bytes = b->outbuf->datas[0].maxsize;

  spa_log_trace (this->log, "fakesrc %p: dequeue buffer %d", this, b->outbuf->id);

  fill_buffer (this, b);

  b->outbuf->datas[0].chunk->offset = 0;
  b->outbuf->datas[0].chunk->size = n_bytes;
  b->outbuf->datas[0].chunk->stride = n_bytes;

  if (b->h) {
    b->h->seq = this->buffer_count;
    b->h->pts = this->start_time + this->elapsed_time;
    b->h->dts_offset = 0;
  }

  this->buffer_count++;
  this->elapsed_time = this->buffer_count;
  set_timer (this, true);

  io->buffer_id = b->outbuf->id;
  io->status = SPA_RESULT_HAVE_BUFFER;

  return SPA_RESULT_HAVE_BUFFER;
}

static void
fakesrc_on_output (SpaSource *source)
{
  SpaFakeSrc *this = source->data;
  SpaResult res;

  res = fakesrc_make_buffer (this);

  if (res == SPA_RESULT_HAVE_BUFFER && this->callbacks.have_output)
    this->callbacks.have_output (&this->node, this->user_data);
}

static SpaResult
spa_fakesrc_node_send_command (SpaNode    *node,
                               SpaCommand *command)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (command != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  if (SPA_COMMAND_TYPE (command) == this->type.command_node.Start) {
    struct timespec now;

    if (!this->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (this->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    if (this->started)
      return SPA_RESULT_OK;

    clock_gettime (CLOCK_MONOTONIC, &now);
    if (this->props.live)
      this->start_time = SPA_TIMESPEC_TO_TIME (&now);
    else
      this->start_time = 0;
    this->buffer_count = 0;
    this->elapsed_time = 0;

    this->started = true;
    set_timer (this, true);
  }
  else if (SPA_COMMAND_TYPE (command) == this->type.command_node.Pause) {
    if (!this->have_format)
      return SPA_RESULT_NO_FORMAT;

    if (this->n_buffers == 0)
      return SPA_RESULT_NO_BUFFERS;

    if (!this->started)
      return SPA_RESULT_OK;

    this->started = false;
    set_timer (this, false);
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_set_callbacks (SpaNode                *node,
                                const SpaNodeCallbacks *callbacks,
                                size_t                  callbacks_size,
                                void                   *user_data)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  if (this->data_loop == NULL && callbacks->have_output != NULL) {
    spa_log_error (this->log, "a data_loop is needed for async operation");
    return SPA_RESULT_ERROR;
  }
  this->callbacks = *callbacks;
  this->user_data = user_data;

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_get_n_ports (SpaNode       *node,
                              uint32_t      *n_input_ports,
                              uint32_t      *max_input_ports,
                              uint32_t      *n_output_ports,
                              uint32_t      *max_output_ports)
{
  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (n_input_ports)
    *n_input_ports = 0;
  if (n_output_ports)
    *n_output_ports = 1;
  if (max_input_ports)
    *max_input_ports = 0;
  if (max_output_ports)
    *max_output_ports = 1;

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_get_port_ids (SpaNode       *node,
                               uint32_t       n_input_ports,
                               uint32_t      *input_ids,
                               uint32_t       n_output_ports,
                               uint32_t      *output_ids)
{
  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (n_output_ports > 0 && output_ids != NULL)
    output_ids[0] = 0;

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_add_port (SpaNode        *node,
                           SpaDirection    direction,
                           uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_fakesrc_node_remove_port (SpaNode        *node,
                              SpaDirection    direction,
                              uint32_t        port_id)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_fakesrc_node_port_enum_formats (SpaNode          *node,
                                    SpaDirection      direction,
                                    uint32_t          port_id,
                                    SpaFormat       **format,
                                    const SpaFormat  *filter,
                                    uint32_t          index)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  return SPA_RESULT_ENUM_END;
}

static SpaResult
clear_buffers (SpaFakeSrc *this)
{
  if (this->n_buffers > 0) {
    spa_log_info (this->log, "fakesrc %p: clear buffers", this);
    this->n_buffers = 0;
    spa_list_init (&this->empty);
    this->started = false;
    set_timer (this, false);
  }
  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_port_set_format (SpaNode         *node,
                                  SpaDirection     direction,
                                  uint32_t         port_id,
                                  uint32_t         flags,
                                  const SpaFormat *format)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (format == NULL) {
    this->have_format = false;
    clear_buffers (this);
  } else {
    if (SPA_POD_SIZE (format) > sizeof (this->format_buffer))
      return SPA_RESULT_ERROR;
    memcpy (this->format_buffer, format, SPA_POD_SIZE (format));
    this->have_format = true;
  }

  if (this->have_format) {
  }

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_port_get_format (SpaNode          *node,
                                  SpaDirection      direction,
                                  uint32_t          port_id,
                                  const SpaFormat **format)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (format != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  *format = (const SpaFormat *) this->format_buffer;

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_port_get_info (SpaNode            *node,
                                SpaDirection        direction,
                                uint32_t            port_id,
                                const SpaPortInfo **info)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  *info = &this->info;

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_port_enum_params (SpaNode       *node,
                                   SpaDirection   direction,
                                   uint32_t       port_id,
                                   uint32_t       index,
                                   SpaParam     **param)
{
  SpaFakeSrc *this;
  SpaPODBuilder b = { NULL };
  SpaPODFrame f[2];

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (param != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  spa_pod_builder_init (&b, this->params_buffer, sizeof (this->params_buffer));

  switch (index) {
  case 0:
    spa_pod_builder_object (&b, &f[0], 0, this->type.param_alloc_buffers.Buffers,
      PROP      (&f[1], this->type.param_alloc_buffers.size,    SPA_POD_TYPE_INT, 128),
      PROP      (&f[1], this->type.param_alloc_buffers.stride,  SPA_POD_TYPE_INT, 1),
      PROP_U_MM (&f[1], this->type.param_alloc_buffers.buffers, SPA_POD_TYPE_INT, 32, 2, 32),
      PROP      (&f[1], this->type.param_alloc_buffers.align,   SPA_POD_TYPE_INT, 16));
    break;

  case 1:
    spa_pod_builder_object (&b, &f[0], 0, this->type.param_alloc_meta_enable.MetaEnable,
      PROP      (&f[1], this->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID, this->type.meta.Header),
      PROP      (&f[1], this->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT, sizeof (SpaMetaHeader)));
    break;

  default:
    return SPA_RESULT_NOT_IMPLEMENTED;
  }
  *param = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaParam);

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_port_set_param (SpaNode        *node,
                                 SpaDirection    direction,
                                 uint32_t        port_id,
                                 const SpaParam *param)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_fakesrc_node_port_use_buffers (SpaNode         *node,
                                   SpaDirection     direction,
                                   uint32_t         port_id,
                                   SpaBuffer      **buffers,
                                   uint32_t         n_buffers)
{
  SpaFakeSrc *this;
  uint32_t i;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  clear_buffers (this);

  for (i = 0; i < n_buffers; i++) {
    Buffer *b;
    SpaData *d = buffers[i]->datas;

    b = &this->buffers[i];
    b->outbuf = buffers[i];
    b->outstanding = false;
    b->h = spa_buffer_find_meta (buffers[i], this->type.meta.Header);

    if ((d[0].type == this->type.data.MemPtr ||
         d[0].type == this->type.data.MemFd ||
         d[0].type == this->type.data.DmaBuf) &&
        d[0].data == NULL) {
      spa_log_error (this->log, "fakesrc %p: invalid memory on buffer %p", this, buffers[i]);
    }
    spa_list_insert (this->empty.prev, &b->link);
  }
  this->n_buffers = n_buffers;
  this->underrun = false;

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_port_alloc_buffers (SpaNode         *node,
                                     SpaDirection     direction,
                                     uint32_t         port_id,
                                     SpaParam       **params,
                                     uint32_t         n_params,
                                     SpaBuffer      **buffers,
                                     uint32_t        *n_buffers)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_return_val_if_fail (CHECK_PORT (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  if (!this->have_format)
    return SPA_RESULT_NO_FORMAT;

  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_fakesrc_node_port_set_io (SpaNode       *node,
                              SpaDirection   direction,
                              uint32_t       port_id,
                              SpaPortIO     *io)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_return_val_if_fail (CHECK_PORT_NUM (this, direction, port_id), SPA_RESULT_INVALID_PORT);

  this->io = io;

  return SPA_RESULT_OK;
}

static inline void
reuse_buffer (SpaFakeSrc *this, uint32_t id)
{
  Buffer *b = &this->buffers[id];
  spa_return_if_fail (b->outstanding);

  spa_log_trace (this->log, "fakesrc %p: reuse buffer %d", this, id);

  b->outstanding = false;
  spa_list_insert (this->empty.prev, &b->link);

  if (this->underrun) {
    set_timer (this, true);
    this->underrun = false;
  }
}

static SpaResult
spa_fakesrc_node_port_reuse_buffer (SpaNode         *node,
                                    uint32_t         port_id,
                                    uint32_t         buffer_id)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);

  spa_return_val_if_fail (port_id == 0, SPA_RESULT_INVALID_PORT);
  spa_return_val_if_fail (this->n_buffers > 0, SPA_RESULT_NO_BUFFERS);
  spa_return_val_if_fail (buffer_id < this->n_buffers, SPA_RESULT_INVALID_BUFFER_ID);

  reuse_buffer (this, buffer_id);

  return SPA_RESULT_OK;
}

static SpaResult
spa_fakesrc_node_port_send_command (SpaNode        *node,
                                    SpaDirection    direction,
                                    uint32_t        port_id,
                                    SpaCommand     *command)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_fakesrc_node_process_input (SpaNode *node)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_fakesrc_node_process_output (SpaNode *node)
{
  SpaFakeSrc *this;
  SpaPortIO *io;

  spa_return_val_if_fail (node != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = SPA_CONTAINER_OF (node, SpaFakeSrc, node);
  io = this->io;
  spa_return_val_if_fail (io != NULL, SPA_RESULT_WRONG_STATE);

  if (io->status == SPA_RESULT_HAVE_BUFFER)
    return SPA_RESULT_HAVE_BUFFER;

  if (io->buffer_id != SPA_ID_INVALID) {
    reuse_buffer (this, this->io->buffer_id);
    this->io->buffer_id = SPA_ID_INVALID;
  }

  if (this->callbacks.have_output == NULL && (io->status == SPA_RESULT_NEED_BUFFER))
    return fakesrc_make_buffer (this);
  else
    return SPA_RESULT_OK;
}

static const SpaNode fakesrc_node = {
  sizeof (SpaNode),
  NULL,
  spa_fakesrc_node_get_props,
  spa_fakesrc_node_set_props,
  spa_fakesrc_node_send_command,
  spa_fakesrc_node_set_callbacks,
  spa_fakesrc_node_get_n_ports,
  spa_fakesrc_node_get_port_ids,
  spa_fakesrc_node_add_port,
  spa_fakesrc_node_remove_port,
  spa_fakesrc_node_port_enum_formats,
  spa_fakesrc_node_port_set_format,
  spa_fakesrc_node_port_get_format,
  spa_fakesrc_node_port_get_info,
  spa_fakesrc_node_port_enum_params,
  spa_fakesrc_node_port_set_param,
  spa_fakesrc_node_port_use_buffers,
  spa_fakesrc_node_port_alloc_buffers,
  spa_fakesrc_node_port_set_io,
  spa_fakesrc_node_port_reuse_buffer,
  spa_fakesrc_node_port_send_command,
  spa_fakesrc_node_process_input,
  spa_fakesrc_node_process_output,
};

static SpaResult
spa_fakesrc_clock_get_props (SpaClock  *clock,
                             SpaProps **props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_fakesrc_clock_set_props (SpaClock       *clock,
                             const SpaProps *props)
{
  return SPA_RESULT_NOT_IMPLEMENTED;
}

static SpaResult
spa_fakesrc_clock_get_time (SpaClock         *clock,
                            int32_t          *rate,
                            int64_t          *ticks,
                            int64_t          *monotonic_time)
{
  struct timespec now;
  uint64_t tnow;

  spa_return_val_if_fail (clock != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  if (rate)
    *rate = SPA_NSEC_PER_SEC;

  clock_gettime (CLOCK_MONOTONIC, &now);
  tnow = SPA_TIMESPEC_TO_TIME (&now);

  if (ticks)
    *ticks = tnow;
  if (monotonic_time)
    *monotonic_time = tnow;

  return SPA_RESULT_OK;
}

static const SpaClock fakesrc_clock = {
  sizeof (SpaClock),
  NULL,
  SPA_CLOCK_STATE_STOPPED,
  spa_fakesrc_clock_get_props,
  spa_fakesrc_clock_set_props,
  spa_fakesrc_clock_get_time,
};

static SpaResult
spa_fakesrc_get_interface (SpaHandle         *handle,
                           uint32_t           interface_id,
                           void             **interface)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (interface != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaFakeSrc *) handle;

  if (interface_id == this->type.node)
    *interface = &this->node;
  else if (interface_id == this->type.clock)
    *interface = &this->clock;
  else
    return SPA_RESULT_UNKNOWN_INTERFACE;

  return SPA_RESULT_OK;
}

static SpaResult
fakesrc_clear (SpaHandle *handle)
{
  SpaFakeSrc *this;

  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  this = (SpaFakeSrc *) handle;

  if (this->data_loop)
    spa_loop_remove_source (this->data_loop, &this->timer_source);
  close (this->timer_source.fd);

  return SPA_RESULT_OK;
}

static SpaResult
fakesrc_init (const SpaHandleFactory  *factory,
              SpaHandle               *handle,
              const SpaDict           *info,
              const SpaSupport        *support,
              uint32_t                 n_support)
{
  SpaFakeSrc *this;
  uint32_t i;

  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (handle != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  handle->get_interface = spa_fakesrc_get_interface;
  handle->clear = fakesrc_clear;

  this = (SpaFakeSrc *) handle;

  for (i = 0; i < n_support; i++) {
    if (strcmp (support[i].type, SPA_TYPE__TypeMap) == 0)
      this->map = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE__Log) == 0)
      this->log = support[i].data;
    else if (strcmp (support[i].type, SPA_TYPE_LOOP__DataLoop) == 0)
      this->data_loop = support[i].data;
  }
  if (this->map == NULL) {
    spa_log_error (this->log, "a type-map is needed");
    return SPA_RESULT_ERROR;
  }
  init_type (&this->type, this->map);

  this->node = fakesrc_node;
  this->clock = fakesrc_clock;
  reset_fakesrc_props (this, &this->props);

  spa_list_init (&this->empty);

  this->timer_source.func = fakesrc_on_output;
  this->timer_source.data = this;
  this->timer_source.fd = timerfd_create (CLOCK_MONOTONIC, TFD_CLOEXEC);
  this->timer_source.mask = SPA_IO_IN;
  this->timer_source.rmask = 0;
  this->timerspec.it_value.tv_sec = 0;
  this->timerspec.it_value.tv_nsec = 0;
  this->timerspec.it_interval.tv_sec = 0;
  this->timerspec.it_interval.tv_nsec = 0;

  if (this->data_loop)
    spa_loop_add_source (this->data_loop, &this->timer_source);

  this->info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS |
                     SPA_PORT_INFO_FLAG_NO_REF;
  if (this->props.live)
    this->info.flags |= SPA_PORT_INFO_FLAG_LIVE;

  spa_log_info (this->log, "fakesrc %p: initialized", this);

  return SPA_RESULT_OK;
}

static const SpaInterfaceInfo fakesrc_interfaces[] =
{
  { SPA_TYPE__Node, },
  { SPA_TYPE__Clock, },
};

static SpaResult
fakesrc_enum_interface_info (const SpaHandleFactory  *factory,
                             const SpaInterfaceInfo **info,
                             uint32_t                 index)
{
  spa_return_val_if_fail (factory != NULL, SPA_RESULT_INVALID_ARGUMENTS);
  spa_return_val_if_fail (info != NULL, SPA_RESULT_INVALID_ARGUMENTS);

  switch (index) {
    case 0:
      *info = &fakesrc_interfaces[index];
      break;
    default:
      return SPA_RESULT_ENUM_END;
  }
  return SPA_RESULT_OK;
}

const SpaHandleFactory spa_fakesrc_factory =
{ "fakesrc",
  NULL,
  sizeof (SpaFakeSrc),
  fakesrc_init,
  fakesrc_enum_interface_info,
};