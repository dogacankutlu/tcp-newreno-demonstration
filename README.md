# TCP NewReno — CSE 320 Assignment

Implementation of TCP NewReno congestion control over a simulated six-node UDP network.

Six nodes (A–F) run as separate processes on localhost ports 5001–5006. They exchange link-state advertisements, each builds a routing table with Dijkstra, and the TCP layer on top simulates NewReno congestion control with slow start, congestion avoidance, fast retransmit, fast recovery with partial ACK handling, and timeout.

Topology edges (undirected): A-B=4, A-C=7, A-D=13, A-F=5, B-D=8, B-E=3, C-E=12. A to D routes via B at cost 12, not the direct link at cost 13.

## Build

Requires a C11 compiler and POSIX sockets (macOS or Linux).

    make

## Run

Start one node per terminal:

    ./node configs/A.conf
    ./node configs/B.conf
    ./node configs/C.conf
    ./node configs/D.conf
    ./node configs/E.conf
    ./node configs/F.conf

Wait about 5 seconds for routing to converge. Each node prints its routing table and then shows a prompt.

## Commands

    send <DEST> <message>               forward a plain text message
    tcp  <DEST> <N>                     send N segments, no loss
    tcp  <DEST> <N> drop S1 S2 ...     simulate loss on those seqs (triple dup ACK demo)
    tcp  <DEST> <N> timeout             drop seq 1 to trigger RTO
    route                               reprint routing table
    quit

The tcp command prints a trace line for every congestion-control event so you can watch cwnd evolve.

## What is implemented

Routing: link-state flooding with LSA sequence numbers to prevent loops, followed by Dijkstra on the accumulated LSDB.

TCP NewReno sender:
- Slow start: cwnd += 1 per new ACK until cwnd reaches ssthresh
- Congestion avoidance: cwnd += 1/cwnd per new ACK
- Three duplicate ACKs: fast retransmit, enter fast recovery (ssthresh = cwnd/2, cwnd = ssthresh + 3)
- Partial ACK in fast recovery (NewReno specific): stay in FR, retransmit next gap, deflate cwnd. Reno would exit FR here; NewReno does not.
- Full ACK past recover: exit FR, cwnd = ssthresh
- Timeout: ssthresh = cwnd/2, cwnd = 1, slow start restart

Loss injection is sender-side only. Dropped segments are never transmitted on the first attempt; retransmissions go through normally.
