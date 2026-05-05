#include "routing.h"
#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct routing_state {
    node_config_t cfg;
    int           own_seq;

    lsa_entry_t   lsdb[MAX_NODES];
    int           n_lsdb;

    route_t       routes[MAX_NODES];
    int           n_routes;
};

/* ---------- LSDB helpers ---------- */

static lsa_entry_t *lsdb_find(routing_state_t *r, const char *name) {
    for (int i = 0; i < r->n_lsdb; i++) {
        if (r->lsdb[i].in_use && strcmp(r->lsdb[i].name, name) == 0) {
            return &r->lsdb[i];
        }
    }
    return NULL;
}

static lsa_entry_t *lsdb_get_or_make(routing_state_t *r, const char *name) {
    lsa_entry_t *e = lsdb_find(r, name);
    if (e) return e;
    if (r->n_lsdb >= MAX_NODES) return NULL;
    e = &r->lsdb[r->n_lsdb++];
    memset(e, 0, sizeof(*e));
    e->in_use = 1;
    snprintf(e->name, sizeof(e->name), "%s", name);
    e->seq = -1;
    return e;
}

/* ---------- LSA serialization ---------- */

static void build_self_lsa(const routing_state_t *r, char *out, size_t outlen) {
    /* LSA|<origin>|<seq>|<n1>:<c1>,<n2>:<c2>,... */
    int off = snprintf(out, outlen, "LSA|%s|%d|", r->cfg.name, r->own_seq);
    for (int i = 0; i < r->cfg.n_neighbors && off < (int)outlen; i++) {
        const neighbor_t *n = &r->cfg.neighbors[i];
        off += snprintf(out + off, outlen - off,
                        "%s%s:%d", (i ? "," : ""), n->name, n->cost);
    }
}

void routing_advertise(routing_state_t *r, int sock) {
    r->own_seq++;

    /* Install own entry locally so Dijkstra sees it without waiting for echo. */
    lsa_entry_t *self = lsdb_get_or_make(r, r->cfg.name);
    self->seq = r->own_seq;
    self->n_links = r->cfg.n_neighbors;
    for (int i = 0; i < r->cfg.n_neighbors; i++) {
        snprintf(self->links[i].name, sizeof(self->links[i].name),
                 "%s", r->cfg.neighbors[i].name);
        self->links[i].cost = r->cfg.neighbors[i].cost;
    }

    char msg[MAX_LINE];
    build_self_lsa(r, msg, sizeof(msg));

    for (int i = 0; i < r->cfg.n_neighbors; i++) {
        const neighbor_t *n = &r->cfg.neighbors[i];
        net_send(sock, n->host, n->port, msg);
    }
}

int routing_handle_lsa(routing_state_t *r, const char *msg) {
    /* Expect: LSA|origin|seq|name:cost,name:cost,... (links may be empty). */
    if (strncmp(msg, "LSA|", 4) != 0) return 0;

    char buf[MAX_LINE];
    snprintf(buf, sizeof(buf), "%s", msg);

    char *p = buf + 4;
    char *bar1 = strchr(p, '|');
    if (!bar1) return 0;
    *bar1 = '\0';
    char *origin = p;

    char *seq_str = bar1 + 1;
    char *bar2 = strchr(seq_str, '|');
    if (!bar2) return 0;
    *bar2 = '\0';
    int seq = atoi(seq_str);

    char *links = bar2 + 1;

    /* Ignore stale or duplicate advertisements. */
    lsa_entry_t *e = lsdb_get_or_make(r, origin);
    if (!e) return 0;
    if (seq <= e->seq) return 0;

    e->seq = seq;
    e->n_links = 0;

    char *save = NULL;
    for (char *tok = strtok_r(links, ",", &save);
         tok && e->n_links < MAX_NEIGHBORS;
         tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon) continue;
        *colon = '\0';
        snprintf(e->links[e->n_links].name,
                 sizeof(e->links[e->n_links].name), "%s", tok);
        e->links[e->n_links].cost = atoi(colon + 1);
        e->n_links++;
    }
    return 1;
}

void routing_forward_lsa(routing_state_t *r, int sock,
                         const char *msg,
                         const char *from_host, int from_port) {
    for (int i = 0; i < r->cfg.n_neighbors; i++) {
        const neighbor_t *n = &r->cfg.neighbors[i];
        if (strcmp(n->host, from_host) == 0 && n->port == from_port) continue;
        net_send(sock, n->host, n->port, msg);
    }
}

/* ---------- Dijkstra ---------- */

static int name_to_idx(routing_state_t *r, const char *name) {
    for (int i = 0; i < r->n_lsdb; i++) {
        if (r->lsdb[i].in_use && strcmp(r->lsdb[i].name, name) == 0) return i;
    }
    return -1;
}

void routing_recompute(routing_state_t *r) {
    int n = r->n_lsdb;
    int src = name_to_idx(r, r->cfg.name);
    if (src < 0) {
        r->n_routes = 0;
        return;
    }

    long dist[MAX_NODES];
    int  prev[MAX_NODES];
    int  done[MAX_NODES];

    for (int i = 0; i < n; i++) {
        dist[i] = INF_COST;
        prev[i] = -1;
        done[i] = 0;
    }
    dist[src] = 0;

    for (int iter = 0; iter < n; iter++) {
        int u = -1;
        long best = INF_COST;
        for (int i = 0; i < n; i++) {
            if (!done[i] && dist[i] < best) {
                best = dist[i];
                u = i;
            }
        }
        if (u < 0) break;
        done[u] = 1;

        lsa_entry_t *eu = &r->lsdb[u];
        for (int k = 0; k < eu->n_links; k++) {
            int v = name_to_idx(r, eu->links[k].name);
            if (v < 0) continue;
            long alt = dist[u] + eu->links[k].cost;
            if (alt < dist[v]) {
                dist[v] = alt;
                prev[v] = u;
            }
        }
    }

    /* Materialize routes (sorted by name for stable output). */
    int order[MAX_NODES];
    for (int i = 0; i < n; i++) order[i] = i;
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (strcmp(r->lsdb[order[i]].name, r->lsdb[order[j]].name) > 0) {
                int t = order[i]; order[i] = order[j]; order[j] = t;
            }
        }
    }

    r->n_routes = 0;
    for (int oi = 0; oi < n; oi++) {
        int v = order[oi];
        route_t *rt = &r->routes[r->n_routes++];
        snprintf(rt->dest, sizeof(rt->dest), "%s", r->lsdb[v].name);

        if (v == src) {
            snprintf(rt->next_hop, sizeof(rt->next_hop), "-");
            rt->cost = 0;
            snprintf(rt->path, sizeof(rt->path), "%s", r->cfg.name);
            rt->reachable = 1;
            continue;
        }

        if (dist[v] >= INF_COST) {
            snprintf(rt->next_hop, sizeof(rt->next_hop), "?");
            rt->cost = -1;
            snprintf(rt->path, sizeof(rt->path), "(unreachable)");
            rt->reachable = 0;
            continue;
        }

        /* Walk prev[] back to src to find first-hop and full path. */
        int chain[MAX_NODES];
        int len = 0;
        for (int x = v; x != -1 && len < MAX_NODES; x = prev[x]) {
            chain[len++] = x;
        }
        /* chain is dest..src; first hop is chain[len-2]. */
        int first_hop = chain[len - 2];
        snprintf(rt->next_hop, sizeof(rt->next_hop),
                 "%s", r->lsdb[first_hop].name);
        rt->cost = (int)dist[v];

        char *p   = rt->path;
        char *end = rt->path + sizeof(rt->path);
        for (int k = len - 1; k >= 0; k--) {
            int n_written = snprintf(p, end - p, "%s%s",
                                     (k == len - 1) ? "" : " -> ",
                                     r->lsdb[chain[k]].name);
            if (n_written < 0 || n_written >= end - p) break;
            p += n_written;
        }
        rt->reachable = 1;
    }
}

void routing_print_table(const routing_state_t *r) {
    printf("\nRouting table for node %s\n", r->cfg.name);
    printf("Destination  Next Hop  Cost  Path\n");
    for (int i = 0; i < r->n_routes; i++) {
        const route_t *rt = &r->routes[i];
        if (rt->reachable) {
            printf("%-11s  %-8s  %-4d  %s\n",
                   rt->dest, rt->next_hop, rt->cost, rt->path);
        } else {
            printf("%-11s  %-8s  %-4s  %s\n",
                   rt->dest, "?", "-", rt->path);
        }
    }
    printf("\n");
    fflush(stdout);
}

const route_t *routing_lookup(const routing_state_t *r, const char *dest) {
    for (int i = 0; i < r->n_routes; i++) {
        if (strcmp(r->routes[i].dest, dest) == 0 && r->routes[i].reachable) {
            return &r->routes[i];
        }
    }
    return NULL;
}

const neighbor_t *routing_neighbor(const routing_state_t *r, const char *name) {
    for (int i = 0; i < r->cfg.n_neighbors; i++) {
        if (strcmp(r->cfg.neighbors[i].name, name) == 0) {
            return &r->cfg.neighbors[i];
        }
    }
    return NULL;
}

const char *routing_self_name(const routing_state_t *r) {
    return r->cfg.name;
}

routing_state_t *routing_create(const node_config_t *cfg) {
    routing_state_t *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->cfg = *cfg;
    r->own_seq = 0;

    /* Pre-seed LSDB with self. */
    lsa_entry_t *self = lsdb_get_or_make(r, cfg->name);
    self->n_links = cfg->n_neighbors;
    for (int i = 0; i < cfg->n_neighbors; i++) {
        snprintf(self->links[i].name, sizeof(self->links[i].name),
                 "%s", cfg->neighbors[i].name);
        self->links[i].cost = cfg->neighbors[i].cost;
    }
    return r;
}

void routing_destroy(routing_state_t *r) {
    free(r);
}
