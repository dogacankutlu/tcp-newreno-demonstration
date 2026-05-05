#include "tcp_newreno.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

const char *tcp_state_name(tcp_state_t st) {
    switch (st) {
        case TCP_SS: return "SS";
        case TCP_CA: return "CA";
        case TCP_FR: return "FR";
    }
    return "?";
}

static void banner(const char *msg) {
    printf("\n  +----------------------------------------------------+\n");
    printf("  |  %-50s|\n", msg);
    printf("  +----------------------------------------------------+\n");
    fflush(stdout);
}

static void log_line(const tcp_sender_t *s, const char *event) {
    printf("  cwnd=%-5.2f ssthresh=%-2.0f state=%s snd_una=%-3d snd_nxt=%-3d"
           "  | %s\n",
           s->cwnd, s->ssthresh, tcp_state_name(s->state),
           s->snd_una, s->snd_nxt, event);
    fflush(stdout);
}

/* ---------- sender ---------- */

void tcp_sender_reset(tcp_sender_t *s) {
    memset(s, 0, sizeof(*s));
}

void tcp_sender_start(tcp_sender_t *s,
                      const char *dest,
                      int total,
                      const int *drop_seqs, int n_drop) {
    tcp_sender_reset(s);
    s->active = 1;
    snprintf(s->dest, sizeof(s->dest), "%s", dest);
    if (total < 1) total = 1;
    if (total > TCP_MAX_SEGS - 1) total = TCP_MAX_SEGS - 1;
    s->total_segs = total;

    s->snd_una       = 1;
    s->snd_nxt       = 1;
    s->cwnd          = 1.0;
    s->init_ssthresh = 16;
    s->ssthresh      = s->init_ssthresh;
    s->state         = TCP_SS;
    s->dup_acks      = 0;
    s->recover       = 0;
    s->rto_ms        = 800;
    s->acked_pkts    = 0;

    for (int i = 0; i < n_drop && i < TCP_MAX_DROP; i++)
        s->drop_seqs[s->n_drop++] = drop_seqs[i];

    printf("\n  Sending %d segments to %s  (ssthresh=%d, cwnd starts at 1)\n",
           total, dest, s->init_ssthresh);
    printf("  States:  SS=Slow Start  CA=Congestion Avoidance  "
           "FR=Fast Recovery\n\n");
    fflush(stdout);
}

static int should_drop(tcp_sender_t *s, int seq) {
    for (int i = 0; i < s->n_drop; i++) {
        if (s->drop_seqs[i] == seq && !s->drop_done[i]) {
            s->drop_done[i] = 1;
            return 1;
        }
    }
    return 0;
}

static int in_flight(const tcp_sender_t *s) {
    return s->snd_nxt - s->snd_una;
}

void tcp_sender_pump(tcp_sender_t *s, long now_ms,
                     void (*send_segment)(int seq, int dropped, void *user),
                     void *user) {
    if (!s->active) return;

    int win = (int)s->cwnd;
    if (win < 1) win = 1;

    while (s->snd_nxt <= s->total_segs && in_flight(s) < win) {
        int seq     = s->snd_nxt++;
        int dropped = should_drop(s, seq);
        s->send_time_ms[seq] = now_ms;
        send_segment(seq, dropped, user);

        char ev[64];
        if (dropped)
            snprintf(ev, sizeof(ev), "TX  seg=%-3d  << LOST (simulated) >>", seq);
        else
            snprintf(ev, sizeof(ev), "TX  seg=%-3d", seq);
        log_line(s, ev);
    }
}

int tcp_sender_check_timeout(tcp_sender_t *s, long now_ms) {
    if (!s->active) return 0;
    if (s->snd_una > s->total_segs) return 0;
    if (in_flight(s) == 0) return 0;

    long age = now_ms - s->send_time_ms[s->snd_una];
    if (age < s->rto_ms) return 0;

    s->ssthresh = (s->cwnd / 2.0 < 2.0) ? 2.0 : (s->cwnd / 2.0);
    s->cwnd     = 1.0;
    s->state    = TCP_SS;
    s->dup_acks = 0;
    s->snd_nxt  = s->snd_una;

    banner("TIMEOUT  ->  cwnd=1,  ssthresh halved,  Slow Start restart");
    char ev[80];
    snprintf(ev, sizeof(ev),
             "ssthresh=%.0f  cwnd=1  retransmit seg=%d",
             s->ssthresh, s->snd_una);
    log_line(s, ev);
    return 1;
}

int tcp_sender_on_ack(tcp_sender_t *s, int ack, int *retx_seq_out) {
    if (!s->active) return 0;
    if (retx_seq_out) *retx_seq_out = -1;

    if (ack <= s->snd_una) {
        s->dup_acks++;

        if (s->state != TCP_FR && s->dup_acks == 3) {
            s->ssthresh = (s->cwnd / 2.0 < 2.0) ? 2.0 : (s->cwnd / 2.0);
            s->cwnd     = s->ssthresh + 3.0;
            s->recover  = s->snd_nxt - 1;
            s->state    = TCP_FR;

            banner("3 DUP ACKs  ->  FAST RETRANSMIT + enter FAST RECOVERY");
            char ev[96];
            snprintf(ev, sizeof(ev),
                     "ssthresh=%.0f  cwnd=%.0f  recover=%d  retransmit seg=%d",
                     s->ssthresh, s->cwnd, s->recover, s->snd_una);
            log_line(s, ev);

            if (retx_seq_out) *retx_seq_out = s->snd_una;
            return 1;
        }

        if (s->state == TCP_FR) {
            s->cwnd += 1.0;
            char ev[64];
            snprintf(ev, sizeof(ev),
                     "dup ACK ack=%d  (FR window inflate)  cwnd=%.0f",
                     ack, s->cwnd);
            log_line(s, ev);
        } else {
            char ev[64];
            snprintf(ev, sizeof(ev),
                     "dup ACK ack=%d  (%d of 3 needed)", ack, s->dup_acks);
            log_line(s, ev);
        }
        return 0;
    }

    /* New cumulative ACK. */
    int newly_acked = ack - s->snd_una;
    s->snd_una     = ack;
    s->acked_pkts += newly_acked;
    s->dup_acks    = 0;

    if (s->state == TCP_FR) {
        if (ack > s->recover) {
            s->cwnd  = s->ssthresh;
            s->state = TCP_CA;
            banner("FULL ACK  ->  Exit Fast Recovery,  enter Congestion Avoidance");
            char ev[80];
            snprintf(ev, sizeof(ev),
                     "ack=%d > recover=%d  cwnd=ssthresh=%.0f",
                     ack, s->recover, s->cwnd);
            log_line(s, ev);
        } else {
            /* Partial ACK: NewReno-specific — stay in FR, retransmit next gap. */
            double deflate = (double)newly_acked - 1.0;
            if (deflate < 0) deflate = 0;
            s->cwnd -= deflate;
            if (s->cwnd < 1.0) s->cwnd = 1.0;

            banner("PARTIAL ACK (NewReno)  ->  stay in FR, retransmit next gap");
            char ev[96];
            snprintf(ev, sizeof(ev),
                     "ack=%d still < recover=%d  retransmit seg=%d  cwnd=%.0f",
                     ack, s->recover, ack, s->cwnd);
            log_line(s, ev);

            if (retx_seq_out) *retx_seq_out = ack;
            return 1;
        }
    } else if (s->state == TCP_SS) {
        s->cwnd += (double)newly_acked;
        if (s->cwnd >= s->ssthresh) {
            s->state = TCP_CA;
            banner("SLOW START -> CONGESTION AVOIDANCE  (cwnd hit ssthresh)");
            char ev[80];
            snprintf(ev, sizeof(ev),
                     "ack=%d  cwnd=%.2f  ssthresh=%.0f  now growing linearly",
                     ack, s->cwnd, s->ssthresh);
            log_line(s, ev);
        } else {
            char ev[48];
            snprintf(ev, sizeof(ev),
                     "ACK ack=%d  [SS]  cwnd=%.2f", ack, s->cwnd);
            log_line(s, ev);
        }
    } else { /* TCP_CA */
        s->cwnd += (double)newly_acked / s->cwnd;
        char ev[48];
        snprintf(ev, sizeof(ev),
                 "ACK ack=%d  [CA]  cwnd=%.2f", ack, s->cwnd);
        log_line(s, ev);
    }
    return 0;
}

int tcp_sender_done(const tcp_sender_t *s) {
    return s->active && s->snd_una > s->total_segs;
}

/* ---------- receiver ---------- */

void tcp_receiver_reset(tcp_receiver_t *r) {
    memset(r, 0, sizeof(*r));
    r->rcv_next = 1;
}

int tcp_receiver_on_data(tcp_receiver_t *r, const char *src, int seq) {
    /* A fresh transfer always starts at seq=1. If we see seq=1 from a sender
       whose previous transfer already finished, wipe the buffer so we don't
       carry rcv_next over from the last run. */
    if (!r->active || seq == 1) {
        memset(r->buf, 0, sizeof(r->buf));
        r->active = 1;
        snprintf(r->src, sizeof(r->src), "%s", src);
        r->rcv_next = 1;
    }

    if (seq < 1 || seq >= TCP_MAX_SEGS)
        return r->rcv_next;

    if (seq == r->rcv_next) {
        r->rcv_next++;
        while (r->rcv_next < TCP_MAX_SEGS && r->buf[r->rcv_next]) {
            r->buf[r->rcv_next] = 0;
            r->rcv_next++;
        }
    } else if (seq > r->rcv_next) {
        r->buf[seq] = 1;
    }
    return r->rcv_next;
}
