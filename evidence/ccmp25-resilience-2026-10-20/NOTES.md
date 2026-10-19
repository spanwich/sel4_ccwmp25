# CCMP25 overload resilience — PARKED (2026-10-20)

Spec: `papers/drafts/access-control-as-constraints-s-and-p/studies/ccmp25-board/RESILIENCE_TEST.md`.

**Status: PARKED.** The board-side 1 s counters and the open-loop load
generator are built and validated at low rate over the shared lab switch.
The actual resilience sweep (500 req/s … up to the host ceiling, 60 s + 30 s
recovery per step, 10 min sustained) is **not** run: it needs a direct cable
J22↔eno1 and a point-to-point subnet, so a flood never touches the shared
lab LAN. Waiting for the cable or for confirmation the lab switch has no
uplink.

## What is in place
- **Board counters** (`rootserver/`, no change to the gate pipeline): K0
  emits a 1 s stats frame (`GW_MSG_K0_STATS`): busy ppm, watchdog pets,
  served, rejected frames, no-response, K1→K0 queue depth, send failures.
  K1 keeps the last 256 K0 and K1 samples and serves them on control port
  `:81` via the `counters` command (alongside `health`, `stats`, `probe`).
- **Load generator** (`multikernel_hello/exp/load/loadgen.c`): open-loop,
  fixed per-thread send schedule independent of replies, one connection per
  request, reads each reply to EOF so the board closes first (no host
  TIME_WAIT). Checks every reply's status against `--expect` and counts any
  2xx on a deny flood as a wrong-allow. A legitimate admin client runs
  throughout (`--legit-req`, every 100 ms). Safety: refuses > 1000 req/s
  without `--direct-link`, refuses above `--max-mbps`, and stops the run
  within `--dead-ms` (default 1000) if the board goes silent.
- **Orchestrator** (`exp/resilience.py`): fresh boot per image, logs in for
  real operator/admin tokens, fills the R-A/R-B/R-C and legit templates,
  sweeps rates, polls `:81` counters each second. It too refuses rates above
  `--max-rate` (1000) without `--direct-link`.

## Dry run (validation only, NOT a resilience result)
`dryrun-ra-300rps.json`: R-A anonymous, 300 req/s offered, 10 s, 8 threads,
over the lab switch.
- attempted 3008, completed 406, the rest host-side connect/io errors.
- This is the **generator's own ceiling over the shared switch**: host
  ephemeral ports 32768–60999 with `tcp_tw_reuse=2` (loopback only) cap
  one-connection-per-request near ~470 conn/s. Not a board limit.
- `wrong_allow_2xx = 0`; every completed reply was the expected 401.
- The legitimate client got 0/101 because the generator exhausted the host's
  ephemeral ports, so its own connections failed too — again a host-side
  artefact at this rate over the shared link, not board collapse. The board
  stayed up and kept serving (see `board-after-dryrun.txt`).

The real sweep must run over the direct cable, where the host NIC and a
clean subnet remove this ceiling and the numbers reflect the board.
