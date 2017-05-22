/* Simple Plugin API
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

#ifndef __SPA_GRAPH_H__
#define __SPA_GRAPH_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/list.h>

typedef struct SpaGraph SpaGraph;
typedef struct SpaGraphNode SpaGraphNode;
typedef struct SpaGraphPort SpaGraphPort;

struct SpaGraph {
  SpaList  nodes;
  SpaList  ready;
};

typedef SpaResult (*SpaGraphNodeFunc) (SpaGraphNode *node);

struct SpaGraphNode {
  SpaList          link;
  SpaList          ready_link;
  SpaList          ports[2];
#define SPA_GRAPH_NODE_FLAG_ASYNC       (1 << 0)
  uint32_t         flags;
  SpaResult        state;
#define SPA_GRAPH_ACTION_CHECK   0
#define SPA_GRAPH_ACTION_IN      1
#define SPA_GRAPH_ACTION_OUT     2
  uint32_t         action;
  SpaGraphNodeFunc schedule;
  void            *user_data;
  uint32_t         max_in;
  uint32_t         required_in;
  uint32_t         ready_in;
};

struct SpaGraphPort {
  SpaList          link;
  SpaGraphNode    *node;
  SpaDirection     direction;
  uint32_t         port_id;
  uint32_t         flags;
  SpaPortIO       *io;
  SpaGraphPort    *peer;
};

static inline void
spa_graph_init (SpaGraph *graph)
{
  spa_list_init (&graph->nodes);
  spa_list_init (&graph->ready);
}

static inline SpaResult
spa_graph_node_schedule_default (SpaGraphNode *node)
{
  SpaNode *n = node->user_data;

  if (node->action == SPA_GRAPH_ACTION_IN)
    return spa_node_process_input (n);
  else if (node->action == SPA_GRAPH_ACTION_OUT)
    return spa_node_process_output (n);
  else
    return SPA_RESULT_ERROR;
}

static inline void
spa_graph_node_add (SpaGraph *graph, SpaGraphNode *node, SpaGraphNodeFunc schedule, void *user_data)
{
  spa_list_init (&node->ports[SPA_DIRECTION_INPUT]);
  spa_list_init (&node->ports[SPA_DIRECTION_OUTPUT]);
  node->flags = 0;
  node->state = SPA_RESULT_OK;
  node->action = SPA_GRAPH_ACTION_OUT;
  node->schedule = schedule;
  node->user_data = user_data;
  spa_list_insert (graph->nodes.prev, &node->link);
  node->max_in = node->required_in = node->ready_in = 0;
}

static inline void
spa_graph_port_check (SpaGraph     *graph,
                      SpaGraphPort *port)
{
  SpaGraphNode *node = port->node;

  if (port->io->status == SPA_RESULT_HAVE_BUFFER)
    node->ready_in++;

  if (node->required_in > 0 && node->ready_in == node->required_in) {
    node->action = SPA_GRAPH_ACTION_IN;
    if (node->ready_link.next == NULL)
      spa_list_insert (graph->ready.prev, &node->ready_link);
  } else if (node->ready_link.next) {
    spa_list_remove (&node->ready_link);
    node->ready_link.next = NULL;
  }
}

static inline void
spa_graph_port_add (SpaGraph     *graph,
                    SpaGraphNode *node,
                    SpaGraphPort *port,
                    SpaDirection  direction,
                    uint32_t      port_id,
                    uint32_t      flags,
                    SpaPortIO    *io)
{
  port->node = node;
  port->direction = direction;
  port->port_id = port_id;
  port->flags = flags;
  port->io = io;
  port->peer = NULL;
  spa_list_insert (node->ports[port->direction].prev, &port->link);
  node->max_in++;
  if (!(port->flags & SPA_PORT_INFO_FLAG_OPTIONAL) && direction == SPA_DIRECTION_INPUT)
    node->required_in++;
  spa_graph_port_check (graph, port);
}

static inline void
spa_graph_node_remove (SpaGraph *graph, SpaGraphNode *node)
{
  spa_list_remove (&node->link);
}

static inline void
spa_graph_port_remove (SpaGraph *graph, SpaGraphPort *port)
{
  spa_list_remove (&port->link);
}

static inline void
spa_graph_port_link (SpaGraph *graph, SpaGraphPort *out, SpaGraphPort *in)
{
  out->peer = in;
  in->peer = out;
}

static inline void
spa_graph_port_unlink (SpaGraph *graph, SpaGraphPort *out, SpaGraphPort *in)
{
  out->peer = NULL;
  in->peer = NULL;
}

static inline void
spa_graph_node_schedule (SpaGraph *graph, SpaGraphNode *node)
{
  SpaGraphPort *p;

  if (node->ready_link.next == NULL)
    spa_list_insert (graph->ready.prev, &node->ready_link);

  while (!spa_list_is_empty (&graph->ready)) {
    SpaGraphNode *n = spa_list_first (&graph->ready, SpaGraphNode, ready_link);

    spa_list_remove (&n->ready_link);
    n->ready_link.next = NULL;

    switch (n->action) {
      case SPA_GRAPH_ACTION_IN:
      case SPA_GRAPH_ACTION_OUT:
        n->state = n->schedule (n);
        if (n->action == SPA_GRAPH_ACTION_IN && n == node)
          continue;
        n->action = SPA_GRAPH_ACTION_CHECK;
        spa_list_insert (graph->ready.prev, &n->ready_link);
        break;

      case SPA_GRAPH_ACTION_CHECK:
        if (n->state == SPA_RESULT_NEED_BUFFER) {
          n->ready_in = 0;
          spa_list_for_each (p, &n->ports[SPA_DIRECTION_INPUT], link) {
            SpaGraphNode *pn = p->peer->node;
            if (p->io->status == SPA_RESULT_NEED_BUFFER) {
              if (pn != node || pn->flags & SPA_GRAPH_NODE_FLAG_ASYNC) {
                pn->action = SPA_GRAPH_ACTION_OUT;
                spa_list_insert (graph->ready.prev, &pn->ready_link);
              }
            }
            else if (p->io->status == SPA_RESULT_OK)
              n->ready_in++;
          }
        }
        else if (n->state == SPA_RESULT_HAVE_BUFFER) {
          spa_list_for_each (p, &n->ports[SPA_DIRECTION_OUTPUT], link)
            spa_graph_port_check (graph, p->peer);
        }
        break;

      default:
        break;
    }
  }
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_GRAPH_H__ */