/* node.c
 *
 * Six-node UDP overlay (A-F) with link-state routing and a TCP NewReno
 * sender on top. Run one process per node:
 *
 *     ./node configs/A.conf
 *
 * After a short LSA convergence phase the node prints its routing table
 * and drops to a prompt. Type 'help' for commands.
 */

#include "common.h"
#include "config.h"
#include "net.h"
#include "routing.h"
#include "tcp_newreno.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/time.h>

static long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ---------------- forwarding helpers ---------------- */

static void forward_or_deliver_data(routing_state_t *r, int sock,
                                    const char *src, const char *dst,
                                    const char *payload) {
    if (strcmp(dst, routing_self_name(r)) == 0) {
        printf("[%s] Received message from %s: %s\n",
               routing_self_name(r), src, payload);
        fflush(stdout);
        return;
    }

    const route_t *rt = routing_lookup(r, dst);
    if (!rt) {
        printf("[%s] No route to %s, dropping\n",
               routing_self_name(r), dst);
        return;
    }
    const neighbor_t *nh = routing_neighbor(r, rt->next_hop);
    if (!nh) {
        printf("[%s] Next hop %s not a direct neighbor (?)\n",
               routing_self_name(r), rt->next_hop);
        return;
    }

    if (strcmp(src, routing_self_name(r)) == 0) {
        printf("[%s] Destination %s, next hop %s\n",
               routing_self_name(r), dst, rt->next_hop);
    } else {
        printf("[%s] Forwarding message from %s to %s, next hop %s\n",
               routing_self_name(r), src, dst, rt->next_hop);
    }
    fflush(stdout);

    char wire[MAX_LINE];
    snprintf(wire, sizeof(wire), "DATA|%s|%s|%s", src, dst, payload);
    net_send(sock, nh->host, nh->port, wire);
}

/* TCP segments take the same routing path but we keep their forwarding silent
   so the cwnd trace stays readable. */
static void forward_tcp(routing_state_t *r, int sock,
                        const char *src, const char *dst,
                        const char *kind, int seq) {
    char wire[MAX_LINE];
    snprintf(wire, sizeof(wire), "TCP|%s|%s|%s|%d", src, dst, kind, seq);

    if (strcmp(dst, routing_self_name(r)) == 0) {
        /* shouldn't happen — caller checked. */
        return;
    }
    const route_t *rt = routing_lookup(r, dst);
    if (!rt) return;
    const neighbor_t *nh = routing_neighbor(r, rt->next_hop);
    if (!nh) return;

    net_send(sock, nh->host, nh->port, wire);
}

/* Closure passed to tcp_sender_pump so it can hand each segment to the
   routing layer. */
typedef struct {
    routing_state_t *r;
    int              sock;
} send_ctx_t;

/* The pump callback doesn't carry the destination, so we stash it in a
   small process-global slot just before calling pump. Single-threaded, so
   this is fine. */
static const char *g_tcp_dest = NULL;

static void send_segment_real(int seq, int dropped, void *user) {
    if (dropped) return;
    send_ctx_t *ctx = (send_ctx_t *)user;
    forward_tcp(ctx->r, ctx->sock,
                routing_self_name(ctx->r),
                g_tcp_dest, "DATA", seq);
}

/* ---------------- CLI parsing ---------------- */

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void prompt(const char *name) {
    printf("%s> ", name);
    fflush(stdout);
}

static void usage(void) {
    printf(
"Commands:\n"
"  send <DEST> <message>\n"
"  tcp <DEST> <num_segments> [drop <seq> ...] [timeout]\n"
"      drop: simulate triple-dup-ACK loss on those seqs (one tx each)\n"
"      timeout: drop seq 1 to force an RTO\n"
"  route          show routing table\n"
"  help\n"
"  quit\n");
    fflush(stdout);
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <config-file>\n", argv[0]);
        return 1;
    }

    node_config_t cfg;
    if (config_load(argv[1], &cfg) < 0) return 1;

    int sock = net_open(cfg.port);
    if (sock < 0) return 1;

    routing_state_t *router = routing_create(&cfg);

    tcp_sender_t   tcp_s;  tcp_sender_reset(&tcp_s);
    tcp_receiver_t tcp_r;  tcp_receiver_reset(&tcp_r);

    long start_ms       = now_ms();
    long last_lsa_ms    = 0;
    long lsa_period_ms  = 300;        /* aggressive flood so simultaneous starts all converge */
    int  converged      = 0;
    int  stdin_open     = 1;

    printf("[%s] listening on port %d, %d neighbor(s)\n",
           cfg.name, cfg.port, cfg.n_neighbors);
    fflush(stdout);

    /* Kick off the first LSA right away so neighbors learn us quickly. */
    routing_advertise(router, sock);
    last_lsa_ms = now_ms();

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (stdin_open) FD_SET(0, &rfds);
        FD_SET(sock, &rfds);
        int maxfd = sock;

        struct timeval tv = {0, 100 * 1000};   /* 100ms tick */
        int rv = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }

        long t = now_ms();

        /* ---- timed work: LSAs + TCP pump + RTO ---- */

        if (t - last_lsa_ms >= lsa_period_ms) {
            routing_advertise(router, sock);
            last_lsa_ms = t;
        }

        if (!converged && t - start_ms >= 5000) {
            routing_recompute(router);
            routing_print_table(router);
            converged = 1;
            usage();
            prompt(cfg.name);
        }

        if (tcp_s.active && converged) {
            tcp_sender_check_timeout(&tcp_s, t);

            send_ctx_t ctx = { router, sock };
            g_tcp_dest = tcp_s.dest;
            tcp_sender_pump(&tcp_s, t, send_segment_real, &ctx);
            g_tcp_dest = NULL;

            if (tcp_sender_done(&tcp_s)) {
                printf("  [tcp] transfer complete: %d/%d segments delivered\n",
                       tcp_s.acked_pkts, tcp_s.total_segs);
                fflush(stdout);
                tcp_sender_reset(&tcp_s);
                prompt(cfg.name);
            }
        }

        /* ---- incoming UDP ---- */

        if (FD_ISSET(sock, &rfds)) {
            char buf[MAX_LINE];
            char from_host[64];
            int  from_port = 0;
            int  n = net_recv(sock, buf, sizeof(buf), from_host, &from_port);
            if (n > 0) {
                if (strncmp(buf, "LSA|", 4) == 0) {
                    int changed = routing_handle_lsa(router, buf);
                    if (changed) {
                        routing_forward_lsa(router, sock, buf,
                                            from_host, from_port);
                        if (converged) routing_recompute(router);
                    }
                } else if (strncmp(buf, "DATA|", 5) == 0) {
                    /* DATA|src|dst|payload */
                    char *p = buf + 5;
                    char *b1 = strchr(p, '|');
                    if (!b1) goto cli;
                    *b1 = '\0';
                    char *src = p;
                    char *b2 = strchr(b1 + 1, '|');
                    if (!b2) goto cli;
                    *b2 = '\0';
                    char *dst = b1 + 1;
                    char *payload = b2 + 1;
                    forward_or_deliver_data(router, sock, src, dst, payload);
                } else if (strncmp(buf, "TCP|", 4) == 0) {
                    /* TCP|src|dst|kind|seq */
                    char *p = buf + 4;
                    char *b1 = strchr(p, '|');     if (!b1) goto cli; *b1 = '\0';
                    char *src = p;
                    char *b2 = strchr(b1 + 1, '|'); if (!b2) goto cli; *b2 = '\0';
                    char *dst = b1 + 1;
                    char *b3 = strchr(b2 + 1, '|'); if (!b3) goto cli; *b3 = '\0';
                    char *kind = b2 + 1;
                    int   seq  = atoi(b3 + 1);

                    if (strcmp(dst, routing_self_name(router)) != 0) {
                        /* forward */
                        const route_t *rt = routing_lookup(router, dst);
                        if (rt) {
                            const neighbor_t *nh =
                                routing_neighbor(router, rt->next_hop);
                            if (nh) {
                                char wire[MAX_LINE];
                                snprintf(wire, sizeof(wire),
                                         "TCP|%s|%s|%s|%d",
                                         src, dst, kind, seq);
                                net_send(sock, nh->host, nh->port, wire);
                            }
                        }
                    } else if (strcmp(kind, "DATA") == 0) {
                        int ack = tcp_receiver_on_data(&tcp_r, src, seq);
                        /* Send ACK back to src. */
                        const route_t *rt = routing_lookup(router, src);
                        if (rt) {
                            const neighbor_t *nh =
                                routing_neighbor(router, rt->next_hop);
                            if (nh) {
                                char wire[MAX_LINE];
                                snprintf(wire, sizeof(wire),
                                         "TCP|%s|%s|ACK|%d",
                                         routing_self_name(router), src, ack);
                                net_send(sock, nh->host, nh->port, wire);
                            }
                        }
                    } else if (strcmp(kind, "ACK") == 0) {
                        int retx = -1;
                        if (tcp_sender_on_ack(&tcp_s, seq, &retx)) {
                            if (retx > 0) {
                                send_ctx_t ctx = { router, sock };
                                g_tcp_dest = tcp_s.dest;
                                /* Mark retx send time and transmit. */
                                tcp_s.send_time_ms[retx] = t;
                                send_segment_real(retx, 0, &ctx);
                                g_tcp_dest = NULL;
                            }
                        }
                        if (tcp_sender_done(&tcp_s)) {
                            printf("  [tcp] transfer complete: "
                                   "%d/%d segments delivered\n",
                                   tcp_s.acked_pkts, tcp_s.total_segs);
                            fflush(stdout);
                            tcp_sender_reset(&tcp_s);
                            prompt(cfg.name);
                        }
                    }
                }
            }
        }

cli:
        /* ---- incoming stdin ---- */
        if (stdin_open && FD_ISSET(0, &rfds)) {
            char line[MAX_LINE];
            if (!fgets(line, sizeof(line), stdin)) {
                stdin_open = 0;
                continue;
            }
            char *cmd = trim(line);
            if (*cmd == '\0') {
                if (converged) prompt(cfg.name);
                continue;
            }

            if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0) break;

            if (!converged) {
                printf("(node not ready yet — wait for routing to converge)\n");
                continue;
            }

            if (strcmp(cmd, "help") == 0) {
                usage();
                prompt(cfg.name);
                continue;
            }

            if (strcmp(cmd, "route") == 0) {
                routing_recompute(router);
                routing_print_table(router);
                prompt(cfg.name);
                continue;
            }

            if (strncmp(cmd, "send ", 5) == 0) {
                char dest[MAX_NAME];
                char payload[MAX_PAYLOAD];
                if (sscanf(cmd + 5, "%7s %511[^\n]", dest, payload) == 2) {
                    forward_or_deliver_data(router, sock,
                                            routing_self_name(router),
                                            dest, payload);
                } else {
                    printf("usage: send <DEST> <message>\n");
                }
                prompt(cfg.name);
                continue;
            }

            if (strncmp(cmd, "tcp ", 4) == 0) {
                char dest[MAX_NAME];
                int  total = 0;
                int  consumed = 0;
                if (sscanf(cmd + 4, "%7s %d %n",
                           dest, &total, &consumed) < 2 || total < 1) {
                    printf("usage: tcp <DEST> <N> [drop S1 S2 ...] [timeout]\n");
                    prompt(cfg.name);
                    continue;
                }

                int drops[TCP_MAX_DROP];
                int n_drop = 0;

                char *rest = cmd + 4 + consumed;
                rest = trim(rest);

                while (*rest) {
                    if (strncmp(rest, "drop", 4) == 0) {
                        rest += 4;
                        while (*rest) {
                            while (*rest == ' ' || *rest == '\t') rest++;
                            if (!isdigit((unsigned char)*rest)) break;
                            int v = atoi(rest);
                            if (n_drop < TCP_MAX_DROP) drops[n_drop++] = v;
                            while (isdigit((unsigned char)*rest)) rest++;
                        }
                    } else if (strncmp(rest, "timeout", 7) == 0) {
                        if (n_drop < TCP_MAX_DROP) drops[n_drop++] = 1;
                        rest += 7;
                    } else {
                        rest++;
                    }
                }

                /* Reset receiver so a fresh demo starts clean. */
                tcp_receiver_reset(&tcp_r);
                tcp_sender_start(&tcp_s, dest, total, drops, n_drop);
                /* No prompt yet — it'll come back when the transfer ends. */
                continue;
            }

            printf("unknown command: %s\n", cmd);
            prompt(cfg.name);
        }
    }

    routing_destroy(router);
    close(sock);
    return 0;
}
