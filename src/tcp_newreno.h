#ifndef TCP_NEWRENO_H
#define TCP_NEWRENO_H

#include "common.h"
#include "routing.h"

#define TCP_MAX_SEGS  256
#define TCP_MAX_DROP  32

typedef enum {
    TCP_SS = 0,   /* slow start          */
    TCP_CA,       /* congestion avoidance */
    TCP_FR        /* fast recovery       */
} tcp_state_t;

typedef struct {
    int  active;
    char dest[MAX_NAME];

    int  total_segs;
    int  snd_una;
    int  snd_nxt;

    double cwnd;
    double ssthresh;
    int    init_ssthresh;
    tcp_state_t state;

    int  dup_acks;
    int  recover;          /* highest seq sent at FR entry */

    long send_time_ms[TCP_MAX_SEGS];   /* most recent (re)tx time per seq */
    int  acked_pkts;

    /* Loss injection. */
    int  drop_seqs[TCP_MAX_DROP];
    int  drop_done[TCP_MAX_DROP];
    int  n_drop;

    /* RTO management. */
    long rto_ms;
} tcp_sender_t;

typedef struct {
    int  active;
    char src[MAX_NAME];
    int  rcv_next;
    int  buf[TCP_MAX_SEGS];   /* 1 if received out-of-order */
} tcp_receiver_t;

/* Sender API. ------------------------------------------------------------- */

void tcp_sender_reset(tcp_sender_t *s);

/* total: number of segments to deliver (1..TCP_MAX_SEGS).
   drop_seqs/n_drop: optional list of seqs the sender should drop on first tx. */
void tcp_sender_start(tcp_sender_t *s,
                      const char *dest,
                      int total,
                      const int *drop_seqs, int n_drop);

/* Pump as many segments as the window allows. Each transmission goes
   through send_segment(seq, user) — that's how node.c plugs in routing. */
void tcp_sender_pump(tcp_sender_t *s, long now_ms,
                     void (*send_segment)(int seq, int dropped, void *user),
                     void *user);

/* Drive the RTO. Call periodically; if the oldest unacked has aged past
   rto_ms, the timeout transition fires (cwnd -> 1, slow start). Returns 1
   if a retransmit was scheduled. */
int  tcp_sender_check_timeout(tcp_sender_t *s, long now_ms);

/* Process a cumulative ACK from the receiver (ack = next-seq-expected).
   Returns 1 if a fast retransmit / partial-ACK retransmit is required and
   stores its seq in *retx_seq_out. */
int  tcp_sender_on_ack(tcp_sender_t *s, int ack, int *retx_seq_out);

int  tcp_sender_done(const tcp_sender_t *s);

const char *tcp_state_name(tcp_state_t st);

/* Receiver API. ----------------------------------------------------------- */

void tcp_receiver_reset(tcp_receiver_t *r);

/* Returns the cumulative ACK (rcv_next) the receiver should send back. */
int  tcp_receiver_on_data(tcp_receiver_t *r, const char *src, int seq);

#endif
