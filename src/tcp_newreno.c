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

static void log_line(const tcp_sender_t *s, const char *event) {
    printf("  cwnd=%-5.2f ssthresh=%-3.0f state=%s snd_una=%-3d snd_nxt=%-3d "
           "dup_acks=%d  | %s\n",
           s->cwnd, s->ssthresh, tcp_state_name(s->state),
           s->snd_una, s->snd_nxt, s->dup_acks, event);
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

    s->snd_una        = 1;
    s->snd_nxt        = 1;
    s->cwnd           = 1.0;
    s->init_ssthresh  = 16;
    s->ssthresh       = s->init_ssthresh;
    s->state          = TCP_SS;
    s->dup_acks       = 0;
    s->recover        = 0;
    s->rto_ms         = 800;
    s->acked_pkts     = 0;

    for (int i = 0; i < n_drop && i < TCP_MAX_DROP; i++) {
        s->drop_seqs[s->n_drop++] = drop_seqs[i];
    }

    char ev[64];
    snprintf(ev, sizeof(ev),
             "START total=%d ssthresh=%d", total, s->init_ssthresh);
    log_line(s, ev);
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
        snprintf(ev, sizeof(ev),
                 dropped ? "TX  seq=%d (DROPPED for demo)" : "TX  seq=%d",
                 seq);
        log_line(s, ev);
    }
}

int tcp_sender_check_timeout(tcp_sender_t *s, long now_ms) {
    if (!s->active) return 0;
    if (s->snd_una > s->total_segs) return 0;
    if (in_flight(s) == 0) return 0;

    long age = now_ms - s->send_time_ms[s->snd_una];
    if (age < s->rto_ms) return 0;

    /* Timeout: NewReno still collapses cwnd to 1 and re-enters slow start. */
    s->ssthresh  = (s->cwnd / 2.0 < 2.0) ? 2.0 : (s->cwnd / 2.0);
    s->cwnd      = 1.0;
    s->state     = TCP_SS;
    s->dup_acks  = 0;
    s->snd_nxt   = s->snd_una;       /* abandon anything past the timer */

    char ev[80];
    snprintf(ev, sizeof(ev),
             "TIMEOUT  -> cwnd=1 ssthresh=%.0f, retransmit %d",
             s->ssthresh, s->snd_una);
    log_line(s, ev);
    return 1;
}

int tcp_sender_on_ack(tcp_sender_t *s, int ack, int *retx_seq_out) {
    if (!s->active) return 0;
    if (retx_seq_out) *retx_seq_out = -1;

    if (ack <= s->snd_una) {
        /* Duplicate (cumulative) ACK. */
        s->dup_acks++;

        if (s->state != TCP_FR && s->dup_acks == 3) {
            /* Fast retransmit + enter fast recovery. */
            s->ssthresh = (s->cwnd / 2.0 < 2.0) ? 2.0 : (s->cwnd / 2.0);
            s->cwnd     = s->ssthresh + 3.0;
            s->recover  = s->snd_nxt - 1;
            s->state    = TCP_FR;

            char ev[96];
            snprintf(ev, sizeof(ev),
                     "DUPACK x3 ack=%d -> FR ssthresh=%.0f cwnd=%.2f "
                     "recover=%d, retx %d",
                     ack, s->ssthresh, s->cwnd, s->recover, s->snd_una);
            log_line(s, ev);

            if (retx_seq_out) *retx_seq_out = s->snd_una;
            s->send_time_ms[s->snd_una] = s->send_time_ms[s->snd_una];
            return 1;
        }

        if (s->state == TCP_FR) {
            /* Window inflation: each extra dupACK lets one more new seg out. */
            s->cwnd += 1.0;
            char ev[64];
            snprintf(ev, sizeof(ev), "DUPACK ack=%d (FR inflate)", ack);
            log_line(s, ev);
        } else {
            char ev[64];
            snprintf(ev, sizeof(ev),
                     "DUPACK ack=%d (#%d)", ack, s->dup_acks);
            log_line(s, ev);
        }
        return 0;
    }

    /* New cumulative ACK. */
    int newly_acked = ack - s->snd_una;
    s->snd_una      = ack;
    s->acked_pkts  += newly_acked;
    s->dup_acks     = 0;

    if (s->state == TCP_FR) {
        if (ack > s->recover) {
            /* Full ACK: leave fast recovery. */
            s->cwnd  = s->ssthresh;
            s->state = TCP_CA;
            char ev[80];
            snprintf(ev, sizeof(ev),
                     "ACK ack=%d FULL -> exit FR, cwnd=%.2f", ack, s->cwnd);
            log_line(s, ev);
        } else {
            /* Partial ACK: retransmit the new snd_una, deflate, stay in FR.
               This is the NewReno-specific behavior. */
            double deflate = (double)newly_acked - 1.0;
            if (deflate < 0) deflate = 0;
            s->cwnd -= deflate;
            if (s->cwnd < 1.0) s->cwnd = 1.0;

            char ev[96];
            snprintf(ev, sizeof(ev),
                     "ACK ack=%d PARTIAL (recover=%d) -> retx %d, cwnd=%.2f",
                     ack, s->recover, ack, s->cwnd);
            log_line(s, ev);

            if (retx_seq_out) *retx_seq_out = ack;
            return 1;
        }
    } else if (s->state == TCP_SS) {
        s->cwnd += (double)newly_acked;
        if (s->cwnd >= s->ssthresh) {
            s->state = TCP_CA;
            char ev[80];
            snprintf(ev, sizeof(ev),
                     "ACK ack=%d -> SS->CA, cwnd=%.2f ssthresh=%.0f",
                     ack, s->cwnd, s->ssthresh);
            log_line(s, ev);
        } else {
            char ev[64];
            snprintf(ev, sizeof(ev),
                     "ACK ack=%d (SS) cwnd=%.2f", ack, s->cwnd);
            log_line(s, ev);
        }
    } else { /* TCP_CA */
        s->cwnd += (double)newly_acked / s->cwnd;
        char ev[64];
        snprintf(ev, sizeof(ev),
                 "ACK ack=%d (CA) cwnd=%.2f", ack, s->cwnd);
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
    if (!r->active) {
        r->active = 1;
        snprintf(r->src, sizeof(r->src), "%s", src);
        r->rcv_next = 1;
    }

    if (seq < 1 || seq >= TCP_MAX_SEGS) {
        return r->rcv_next;
    }

    if (seq == r->rcv_next) {
        r->rcv_next++;
        while (r->rcv_next < TCP_MAX_SEGS && r->buf[r->rcv_next]) {
            r->buf[r->rcv_next] = 0;
            r->rcv_next++;
        }
    } else if (seq > r->rcv_next) {
        r->buf[seq] = 1;
    }
    /* If seq < rcv_next we've already delivered it; just re-ack. */
    return r->rcv_next;
}
