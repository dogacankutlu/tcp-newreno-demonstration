#ifndef ROUTING_H
#define ROUTING_H

#include "common.h"

typedef struct routing_state routing_state_t;

routing_state_t *routing_create(const node_config_t *cfg);
void             routing_destroy(routing_state_t *r);

/* Build/refresh our own LSA and flood it to all neighbors. */
void routing_advertise(routing_state_t *r, int sock);

/* Feed a received LSA datagram. Returns 1 if the LSDB changed
   (in which case the caller should re-run Dijkstra). */
int  routing_handle_lsa(routing_state_t *r, const char *msg);

/* Forward an LSA to every neighbor except the one whose host:port
   we received it from (split-horizon-ish flooding). */
void routing_forward_lsa(routing_state_t *r, int sock,
                         const char *msg,
                         const char *from_host, int from_port);

void routing_recompute(routing_state_t *r);
void routing_print_table(const routing_state_t *r);

/* Returns the route entry for dest, or NULL if unknown / unreachable. */
const route_t   *routing_lookup(const routing_state_t *r, const char *dest);

/* Resolve a neighbor name to host:port (used to actually send to next hop). */
const neighbor_t *routing_neighbor(const routing_state_t *r, const char *name);

const char       *routing_self_name(const routing_state_t *r);

#endif
