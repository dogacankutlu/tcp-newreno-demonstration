#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stddef.h>
#include <netinet/in.h>

#define MAX_NODES      16
#define MAX_NEIGHBORS  8
#define MAX_NAME       8
#define MAX_LINE       1024
#define MAX_PAYLOAD    512
#define INF_COST       1000000000

typedef struct {
    char name[MAX_NAME];
    char host[64];
    int  port;
    int  cost;
} neighbor_t;

typedef struct {
    char name[MAX_NAME];
    int  port;
    neighbor_t neighbors[MAX_NEIGHBORS];
    int  n_neighbors;
} node_config_t;

/* One entry of the link-state database: what neighbors does <origin>
   advertise, and at what sequence number was that advertisement sent. */
typedef struct {
    char name[MAX_NAME];           /* origin */
    int  seq;                      /* highest LSA seq seen for this origin */
    int  in_use;
    struct {
        char name[MAX_NAME];
        int  cost;
    } links[MAX_NEIGHBORS];
    int  n_links;
} lsa_entry_t;

typedef struct {
    char dest[MAX_NAME];
    char next_hop[MAX_NAME];       /* "-" if dest is self */
    int  cost;
    char path[128];                /* "A -> B -> D" */
    int  reachable;
} route_t;

#endif
