# CSE 320 — TCP NewReno (Algorithm 2)

A C implementation of the CSE 320 congestion-control assignment.
The instructor's `(student id) mod 3` rule selects algorithm **2 → TCP NewReno**,
so that's what this project implements.

The single binary `node` is launched once per node (A through F). The six
processes form a UDP overlay, learn the topology with link-state
advertisements, build a routing table with Dijkstra, and then run a
NewReno congestion-controlled "transfer" between any two nodes you pick.

## Layout

```
.
├── src/
│   ├── common.h
│   ├── config.{c,h}      parse A.conf .. F.conf
│   ├── net.{c,h}         UDP helpers
│   ├── routing.{c,h}     LSA flooding + Dijkstra
│   ├── tcp_newreno.{c,h} cwnd state machine (sender + receiver)
│   └── node.c            main, CLI, glue
├── configs/
│   ├── A.conf B.conf C.conf D.conf E.conf F.conf
├── Makefile
├── DEMO_GUIDE.txt        narration script for the recorded demo
└── README.md
```

## Build

```
make
```

Requires a C11 compiler and POSIX sockets. Tested on macOS (Darwin)
with `cc` (Apple clang).

## Run

Open six terminals (or six tabs in iTerm/Terminal) and start one node per
terminal:

```
./node configs/A.conf
./node configs/B.conf
./node configs/C.conf
./node configs/D.conf
./node configs/E.conf
./node configs/F.conf
```

Each node listens on its configured port (5001..5006), floods LSAs to its
direct neighbors, runs Dijkstra, and prints its routing table after about
3 seconds. After that you get a prompt:

```
A>
```

### Topology

```
       D ----13---- A ----7---- C
       |          / |           |
       8         5  4          12
       |        /   |           |
       +---- F      B -----3----E
                    |
                    8
                    |
                    D
```

Edges: A-B=4, A-C=7, A-D=13, A-F=5, B-D=8, B-E=3, C-E=12.

A→D **must** route via B (cost 12) instead of using the direct A-D link
(cost 13). The PDF table is reproduced exactly:

```
Routing table for node A
Destination  Next Hop  Cost  Path
A            -         0     A
B            B         4     A -> B
C            C         7     A -> C
D            B         12    A -> B -> D
E            B         7     A -> B -> E
F            F         5     A -> F
```

### Commands

```
send <DEST> <message>            forward a plain message via Dijkstra
tcp  <DEST> <N>                  send N segments with NewReno (no loss)
tcp  <DEST> <N> drop S1 S2 ...   drop those seqs once -> triple-dup-ACK
tcp  <DEST> <N> timeout          drop seq 1 to force an RTO
route                            reprint routing table
help
quit
```

The `tcp` command prints one trace line per state-machine event so the
congestion window evolution is visible step by step.

## What's actually implemented

* **Routing layer** — link-state. Each node periodically floods its own
  LSA to all direct neighbors. Received LSAs are added to a database
  (keyed by origin + sequence number) and re-flooded to every neighbor
  except the one we got it from. Once the LSDB is populated, a small
  Dijkstra in `routing.c` produces next-hop, cost and full path for
  every destination.

* **TCP NewReno sender** (`tcp_newreno.c`):
  * Slow start: `cwnd += 1` per new ACK until `cwnd >= ssthresh`.
  * Congestion avoidance: `cwnd += 1/cwnd` per new ACK.
  * Triple duplicate ACK: `ssthresh = cwnd/2`, `cwnd = ssthresh + 3`,
    fast retransmit, enter Fast Recovery.
  * **Partial ACK in FR (the NewReno point)**: stay in FR, retransmit
    the new `snd_una`, deflate `cwnd` by `(newly_acked - 1)`. Reno
    would have exited FR on this ACK; NewReno doesn't.
  * Full ACK (acks past `recover`): exit FR, `cwnd = ssthresh`.
  * Timeout: `ssthresh = cwnd/2`, `cwnd = 1`, retransmit, slow start.

* **TCP receiver** — cumulative ACKs with an out-of-order bitmap
  buffer; duplicate ACKs are produced naturally when an out-of-order
  packet arrives.

* **Loss injection** — sender-side. The `drop` arguments mark segments
  that should *not* actually be transmitted on their first send;
  retransmissions go through normally.

## Layout note about timeouts vs. FR

`drop 8 9 10` is a fast-retransmit demo: enough later segments arrive at
the receiver that three duplicate ACKs come back, and Fast Recovery
starts before the RTO expires.

`timeout` (which translates to `drop 1`) is a pure-timeout demo: with
`cwnd=1` the very first segment is dropped, no further data is ever in
flight, so no duplicate ACKs can be produced and the only escape is the
RTO firing (configured at 800ms in `tcp_newreno.c`).

## Notes

* All sockets are UDP on `127.0.0.1`. The "TCP" in this project is the
  congestion-control state machine on top — not the OS kernel's TCP.
* Output is line-buffered with explicit `fflush(stdout)` so piping into
  `tee` while recording the demo works without surprises.
* The node ignores commands until link-state convergence finishes
  (~3 s after startup). Wait for the routing table to print before
  typing.
