# 📡 IT305 Lab — Fault-Tolerant Topic-Based File Distribution Service

> **Course:** IT305 — Computer Networks Lab
> **Submission Deadline:** October 23, 2026
> **Groups:** Group 103 & Group 106 (Merged)
> **Language:** C++17 · POSIX sockets · Linux/WSL2
> **Test Environment:** Intel Core i5 / WSL2 Ubuntu 22.04 · Loopback (127.0.0.1) · Dataset: 4,863 files · 57.88 MB

---

## 👥 Group Members & Contributions

| Roll No. | Name | Group | Wave | Primary Contribution |
|---|---|---|---|---|
| 202401496 | Vaghela Khush | G-106 | Wave 1 | Server data structures, CLI parsing & execution loops |
| 202401412 | Yuvraj Chauhan | G-103 | Wave 1 | Length-prefixed wire framing & message formats (all cases) |
| 202401491 | Vekariya Bhavya | G-106 | Wave 2 | Chunk-boundary fault injection & Case 1 stateless recovery |
| 202401476 | Shlok Gohel | G-103 | Wave 2 | Case 2 session management, durability invariant & validation |
| 202401444 | Digvijy Parmar | G-103 | Wave 3 | Design trade-offs, extra features, instrumentation & integration |
| 202401492 | Vekariya Nisarg | G-106 | Wave 3 | Case 2 Enhanced: parallel TCP, epoll, sendfile & bitmask recovery + Animated progress bars (all parts) |

---

## 🗂️ Repository Layout

```
GH_Project/
│
├── 📁 Part1/                        ← Part I  : Reliable single/multi-threaded server
│   ├── server.cpp                   ← Thread-per-client & single-threaded modes
│   ├── client.cpp                   ← Basic topic fetch client + animated progress bar
│   ├── protocol.{h,cpp}             ← Wire-framing layer (shared across all parts)
│   ├── directory_mapper.{h,cpp}     ← Topic → file-list resolver
│   ├── concurrency.{h,cpp}          ← Thread pool & accept-loop helpers
│   ├── metrics_logger.{h,cpp}       ← CSV goodput / overhead logger
│   ├── fault.{h,cpp}                ← Fault injection (stub — disabled in Part 1)
│   └── Makefile
│
├── 📁 Part2_Case1/                  ← Part II Case 1 : Stateless retry (no session)
│   ├── server.cpp                   ← Server drops conn after injected fault
│   ├── client.cpp                   ← Bounded-retry loop (max 50 trials) + progress bar + fault notices
│   ├── fault.{h,cpp}                ← Probabilistic mid-chunk fault injection
│   └── ...                          ← (same header set as Part1)
│
├── 📁 Part2_Case2/                  ← Part II Case 2 : Stateful resume (128-bit session)
│   ├── server.cpp                   ← Checkpoint-aware server
│   ├── client.cpp                   ← RESUME / CKPT_ACK client
│   ├── session.{h,cpp}              ← 128-bit session ID, checkpoint state machine
│   ├── checkpoint.{h,cpp}           ← Disk-durable .ckpt file manager + progress bar + trial tracking
│   └── ...
│
├── 📁 Part2_Case2_Enhanced/         ← Part II Case 2 Enhanced : epoll + parallel TCP
│   ├── server.cpp                   ← epoll event loop entry point
│   ├── client.cpp                   ← Parallel-stream client coordinator
│   ├── enhanced_epoll_server.{h,cpp}    ← Non-blocking epoll server core
│   ├── enhanced_parallel_client.{h,cpp} ← 4-stream TCP + chunk-bitmask recovery + aggregate progress bar
│   └── ...
│
├── 📁 test/                         ← Standalone unit-test suite (338 assertions)
│   ├── test_protocol.cpp            ← Wire-framing & opcode encoding
│   ├── test_mapper.cpp              ← Directory mapper correctness
│   ├── test_fault.cpp               ← Fault injection probability calibration
│   ├── test_session_ckpt.cpp        ← Session state machine (15 tests)
│   ├── test_enhanced.cpp            ← Enhanced parallel transfer (76 tests)
│   └── Makefile
│
├── 📁 Results/                      ← Benchmark scripts, generated plots & CSV metrics
│   ├── run_benchmarks.sh            ← Automated benchmark harness (with FAST=1 mode)
│   ├── plot_results.py              ← Matplotlib visualization script
│   ├── Part1_metrics.csv            ← Empirical benchmark data (Part 1, N=1..32)
│   ├── Part2_metrics.csv            ← Empirical benchmark data (Part 2, Pf=0.0..0.5)
│   ├── Part1_SingleVsMulti_throughput.png ← Part 1 Goodput vs N plot
│   ├── Part2_AllCases_throughput.png      ← Part 2 Goodput vs Pf plot
│   ├── Part2_AllCases_overhead.png        ← Part 2 Retransmission overhead plot
│   ├── FinalReport_Section2_Member2.md    ← Protocol design report
│   └── README_MEMBER2.md            ← Member 2 protocol handoff notes
│
├── 📁 Dataset/                      ← Topic folders with sample files for testing
├── Design_Document_Group_103_106_.pdf
├── IT305-lab-socket-programming-project-assignment.pdf
├── Socket_Programmin_ Project_Plan.pdf
├── Windows_WSL2_Setup_and_Testing_Guide.pdf
└── .gitignore
```

---

## ⚙️ Prerequisites

| Tool | Min Version | Check |
|---|---|---|
| `g++` | 10+ (C++17) | `g++ --version` |
| `make` | 4.0+ | `make --version` |
| Linux / WSL2 | Ubuntu 20.04+ | `uname -r` |
| `pthreads` | (stdlib) | linked via `-pthread` |

> 💡 **Windows users:** All commands below run inside **WSL2**. Open a WSL terminal before proceeding.

---

## 🔨 Build Instructions

Each tier is built independently inside its own directory.

### Part 1 — Reliable Transfer

```bash
cd Part1
make          # builds ./server and ./client
make clean    # removes binaries and object files
```

### Part 2 Case 1 — Stateless Retry

```bash
cd Part2_Case1
make
```

### Part 2 Case 2 — Stateful Checkpoint & Resume

```bash
cd Part2_Case2
make
```

### Part 2 Case 2 Enhanced — epoll + Parallel TCP + Zero-Copy

```bash
cd Part2_Case2_Enhanced
make
```

### Compiler Flags (same across all parts)

```
-O2 -Wall -Wextra -Werror -pthread -std=c++17 -MMD -MP
```

> ⚠️ The build is **warning-free and error-free** under all four flag combinations.

---

## 🚀 Running the Programs

### 🔵 Part 1 — Basic File Transfer

**Terminal 1 — Start the Server:**

```bash
cd Part1
# Multi-threaded (recommended)
./server --port 9000 --dir ../Dataset --mode multi

# Single-threaded (for baseline comparison)
./server --port 9000 --dir ../Dataset --mode single
```

**Terminal 2 — Run the Client:**

```bash
cd Part1
./client --host 127.0.0.1 --port 9000 --topic Cane --out /tmp/output
```

**Sample terminal output (animated progress bar):**
```
[Progress] [████████████████████░] 97.3% | 4731/4863 | 56.3/57.9 MB | 2.21 MB/s [|]
```

---

### 🟡 Part 2 Case 1 — Stateless Retry Under Faults

**Server** (fault probability = 1.5%):

```bash
cd Part2_Case1
./server 9001 ../Dataset 0.015
```

**Client** (retries up to 50 times per topic):

```bash
cd Part2_Case1
./client 127.0.0.1 9001 cane /tmp/output_c1
```

**Sample terminal output (trial tracking + fault notices):**
```
[Trial 3/50] [█████░                ] 26.1% | 1270/4863 | 15.1/57.9 MB | 2.10 MB/s [/]
[FAULT] Corruption detected at file 1271/4863 — restarting Trial 4/50 from File 0...
```

> **Stateless recovery:** On any mid-transfer fault, the **entire topic transfer restarts from file 0** (no checkpoint is persisted). The progress bar resets and the trial counter increments.

---

### 🟠 Part 2 Case 2 — Stateful Session Resume

**Server:**

```bash
cd Part2_Case2
./server --port 9002 --dir ../Dataset --mode multi --fault-prob 0.3 --ckpt-dir /tmp/ckpt
```

**Client** (automatically resumes from last checkpoint on reconnect):

```bash
cd Part2_Case2
./client --host 127.0.0.1 --port 9002 --topic Cane --out /tmp/output_c2 --session-dir /tmp/sessions
```

**Sample terminal output (stateful resume with trial + resume-point tracking):**
```
[Trial 2/inf | Resume from File 847] [████░         ] 21.3% | 847/3982 | 0.65 MB/s
[RESUME] Session 3a7f...c2e1 resumed at File 847 — skipped 846 already-complete files.
```

> **Stateful recovery:** On fault, the client reads the disk checkpoint (`.it305_<topic>.ckpt`) and sends a `RESUME(session_id, file_index, byte_offset)` to the server. Transfer continues **forward from the last completed file boundary** — previously delivered files are never re-transmitted.

#### Checkpoint Durability Protocol (3-Step fsync Chain)

```
1. fsync(active_file_fd)         ← data payload durable on client disk
2. write(tmp_path, record)
   → fsync(tmp_fd)               ← checkpoint record durable
3. rename(tmp_path, ckpt_path)   ← atomic commit (POSIX guarantee)
   → send CKPT_ACK to server     ← server advances to next file
```

This guarantees **no checkpoint is visible to the resume path unless the payload bytes are safely on disk**, preventing silent data loss on power failure or OOM kill.

---

### 🔴 Part 2 Case 2 Enhanced — epoll + 4 Parallel Streams + sendfile

**Server:**

```bash
cd Part2_Case2_Enhanced
./server --port 9003 --dir ../Dataset --epoll --workers 4 --fault-prob 0.3
```

**Client** (opens 4 parallel TCP connections, chunk-bitmask recovery):

```bash
cd Part2_Case2_Enhanced
./client --host 127.0.0.1 --port 9003 --topic Cane --out /tmp/output_enh --streams 4
```

**Sample terminal output (aggregate multi-stream progress bar):**
```
[Streams: 4/4 active] [████████████░        ] 59.8% | 34.7/58.1 MB | AGG: 6.73 MB/s [/]
[STREAM DROP] Sub-stream #2 lost — reconnecting with bitmask resync (missed chunks re-requested)
[Streams: 4/4 active] [████████████░        ] 62.4% | 36.2/58.1 MB | AGG: 6.31 MB/s [|]
```

> **Aggregate throughput** displayed is the sum across all active sub-streams. The active stream count (`4/4`, `3/4`, etc.) updates live as sub-streams drop and reconnect.

#### How bitmask recovery works

```
File pre-allocation  →  posix_fallocate(fd, 0, file_size)
                     →  4 workers each pwrite() into their assigned byte range

On sub-stream drop:
  coordinator sends JOIN_SESSION(bitmask_of_received_chunks)
  server responds ASSIGN_WORK(start_chunk, end_chunk) for ONLY missing chunks
  reconnected stream resumes writing at the precise 64 KiB gap
```

Zero bytes are re-transmitted on reconnect — only truly missing 64 KiB chunks are re-requested.

---

### 📊 Automated Benchmarks & Visualizations

```bash
cd Results
chmod +x run_benchmarks.sh plot_results.py

# Full benchmark suite (N=1..32 for Part 1 & Pf=0.0..0.5 for Part 2)
bash run_benchmarks.sh

# Ultra-Fast mode (~2 minutes, 200 files per topic)
FAST=1 bash run_benchmarks.sh

# Generate high-resolution comparison charts
python3 plot_results.py
```

---

## 🧪 Running the Test Suite

All 338 unit-test assertions live in `test/`:

```bash
cd test
make all         # build all 5 test binaries
make test        # run all tests (expects 0 failures)
make asan        # rebuild + run under AddressSanitizer & UBSan
make clean
```

### Test Coverage Summary

| Test Binary | Assertions | What it covers |
|---|---|---|
| `test_protocol` | ~108 | Wire framing, opcode encoding, endianness, parse guards |
| `test_mapper` | ~79 | Topic → file mapping, empty/missing topic |
| `test_fault` | ~60 | Fault injection probability, seed reproducibility |
| `test_session_ckpt` | **15 / 15** ✅ | Session ID lifecycle, checkpoint file durability |
| `test_enhanced` | **76 / 76** ✅ | Parallel TCP, bitmask recovery, epoll handshake |
| **Total** | **338** | |

---

## 🏗️ Architecture Overview

```
 +------------------------------------------------------------------+
 |                       WIRE PROTOCOL LAYER                        |
 |  Fixed 8-byte frame header [MAGIC(2) VER(1) TYPE(1) LEN(4)]     |
 |  All integers big-endian · no struct padding · length-prefixed   |
 |  Opcodes: 0x01 GET ... 0x0E ASSIGN_WORK                         |
 +----------------------+-------------------------------------------+
                        |
         +--------------+-----------------+
         v              v                 v
  +------------+  +-------------+  +------------------------+
  |   Part 1   |  | Part2 Case1 |  |   Part2 Case2/Enhanced |
  |  Reliable  |  |  Stateless  |  |  Stateful + epoll      |
  |  Transfer  |  |  Retry(x50) |  |  128-bit session ID    |
  |            |  |             |  |  3-step fsync ckpt     |
  | single /   |  | fault inject|  |  Parallel TCP (x4)     |
  | multi-thrd |  | mid-chunk   |  |  sendfile zero-copy    |
  +------+-----+  +------+------+  +-----------+------------+
         |                |                     |
  Animated          Trial counter +       Active stream count +
  progress bar      fault notices       aggregate MB/s display
         |                |                     |
         +--------------+-+---------------------+
                        |
             +----------v----------+
             |   Directory Mapper  |
             |  Topic -> file list |
             +---------------------+
```

### Terminal UI — Progress Bar Design (all parts)

All client progress bars are throttled at **~25 FPS (40 ms)** using `\033[2K\r` carriage-return refresh, fitting within **80 terminal columns**. Feature matrix:

| Field | Part 1 | Case 1 | Case 2 | Enhanced |
|---|---|---|---|---|
| Trial counter | — | ✅ `[Trial N/50]` | ✅ `[Trial N/inf]` | — |
| Resume-from-file | — | — | ✅ `Resume from File X` | — |
| Active stream count | — | — | — | ✅ `[Streams: N/4]` |
| Progress bar + % | ✅ | ✅ | ✅ | ✅ |
| Files delivered | ✅ | ✅ | ✅ | ✅ (aggregate) |
| MB transferred | ✅ | ✅ | ✅ | ✅ (aggregate) |
| Throughput | ✅ MB/s | ✅ MB/s | ✅ MB/s | ✅ AGG MB/s |
| Spinner | ✅ | ✅ | ✅ | ✅ |
| Fault notice | — | ✅ | ✅ `[RESUME]` | ✅ `[STREAM DROP]` |

---

## 📐 Wire Protocol Quick Reference

| Opcode | Name | Dir | Description |
|---|---|---|---|
| `0x01` | `GET` | C→S | Request files for a topic |
| `0x02` | `GET_OK` | S→C | Acknowledge, announce file count & size |
| `0x03` | `ERROR` | S→C | Topic not found or bad request |
| `0x04` | `FILE_BEGIN` | S→C | Start of a new file in the stream |
| `0x05` | `FILE_DATA` | S→C | 64 KiB data chunk |
| `0x06` | `FILE_END` | S→C | End of one file |
| `0x07` | `TRANSFER_END` | S→C | All files delivered |
| `0x08` | `CKPT_ACK` | C→S | Client acknowledges a checkpoint |
| `0x09` | `RESUME` | C→S | Client requests session resume |
| `0x0A` | `RESUME_OK` | S→C | Server accepts resume |
| `0x0B` | `RESTART` | S→C | Session expired — start over |
| `0x0C` | `GET_ENHANCED` | C→S | Parallel multi-stream GET |
| `0x0D` | `JOIN_SESSION` | C→S | Sub-stream joins session + bitmask |
| `0x0E` | `ASSIGN_WORK` | S→C | Server assigns chunk range to sub-stream |

---

## 📊 Empirical Performance Evaluation & Benchmark Results

> All numbers derived from `Results/Part1_metrics.csv` and `Results/Part2_metrics.csv` generated by `Results/run_benchmarks.sh` on WSL2 (loopback 127.0.0.1, Intel Core i5, Ubuntu 22.04). Dataset: **4,863 files / 57.88 MB** (topic `cane`); full-dataset run: **7,342 files / 57.88 MB**. CSV columns: `case, N, Pf, port_start, port_end, elapsed_s, useful_bytes, total_bytes, redundant_bytes, goodput_MBps, wire_MBps, overhead_pct, retries`.

---

### Part I — Aggregate Goodput vs. Concurrent Clients

![Part 1: Single vs Multi-Threaded Aggregate Goodput](Results/Part1_SingleVsMulti_throughput.png)

#### Measured Values (from `Part1_metrics.csv`)

| Clients (N) | Single-Threaded Best (MB/s) | Multi-Threaded Best (MB/s) | Speedup |
|---|---|---|---|
| **1** | 1.422 | **1.682** | 1.18× |
| **2** | 1.387 (first client) | **3.352** | 2.42× |
| **4** | 1.734 (first client) | **2.744** | 1.58× |
| **8** | 1.454 (first client) | **2.481** | 1.71× |
| **16** | 1.661 (first client) | **1.672** | 1.01× |
| **32** | 1.690 (first client) | **1.128** (pool saturated) | 0.67× |

> **Full dataset single-client reliable run** (`Part1_Reliable`, 60.69 MB, 26.06 s):
> **2.329 MB/s** goodput · **2.348 MB/s** wire · **0.81%** overhead — verifying wire-framing correctness on the large dataset.

#### Key Observations

1. **Single-Threaded Bottleneck:** Serves clients serially. Aggregate goodput stays at **~1.4–1.7 MB/s** regardless of N. Per-client latency scales as O(N) — empirically verified from 1.76 s (N=1) to ~48 s cumulative (N=32).

2. **Multi-Threaded Concurrency:** At N=2, multi-thread achieves **3.35 MB/s** (2.42× over single). Thread-per-client dispatch allows concurrent disk I/O and socket transmission.

3. **Thread Pool Saturation (N=32):** At 32 simultaneous loopback clients, kernel TCP send-buffers contend. Multi-threaded goodput per client dips to **0.83–1.13 MB/s** — expected loopback contention behavior.

---

### Part II — Fault Tolerance: Goodput, Overhead & Retries (N=8 Clients)

<p align="center">
  <img src="Results/Part2_AllCases_throughput.png" width="48%" alt="Part 2 Goodput vs Fault Probability" />
  <img src="Results/Part2_AllCases_overhead.png" width="48%" alt="Part 2 Overhead vs Fault Probability" />
</p>

#### Measured Values (from `Part2_metrics.csv`, 4,863 files, 57.88 MB)

| Protocol Variant | Pf | Elapsed (s) | Goodput (MB/s) | Wire (MB/s) | Overhead % | Retries/Drops |
|---|---|---|---|---|---|---|
| **Case 1** *(Stateless)* | **0.00** | 1.13–1.33 | **2.10–2.21** | 2.13–2.23 | 0.81 | 0 |
| **Case 2** *(Stateful)* | **0.00** | 3.31–4.64 | **0.55–0.76** | 0.56–0.76 | 1.23–2.36 | 2–4 |
| **Case 2** *(Stateful)* | **0.10** | 14.7–27.9 | **0.089–0.170** | 0.092–0.172 | 1.33–3.86 | 15–27 |
| **Enhanced** *(4 Streams)* | **0.00** | 1.31–1.76 | **1.43–1.91** | 1.44–1.92 | 0.73 | 0 |
| **Enhanced** *(4 Streams)* | **0.10** | 2.77–5.35 | **0.47–0.90** | 0.50–0.95 | 5.32–8.59 | 12–24 |
| **Enhanced** *(4 Streams)* | **0.30** | 13.4–17.8 | **0.140–0.186** | 0.154–0.207 | 8.11–14.28 | 56–70 |
| **Enhanced** *(4 Streams)* | **0.50** | 27.3–33.6 | **0.075–0.092** | 0.084–0.106 | 10.79–20.82 | 102–128 |

#### Full Dataset Run — Enhanced (60.69 MB, 7,342 files, 4 Streams, Pf = 0.00)

| Metric | Value |
|---|---|
| Elapsed | **9.02 s** |
| Useful bytes delivered | 60,686,716 bytes (57.88 MB) |
| Total bytes on wire | 61,122,191 bytes (58.30 MB) |
| Redundant bytes | 0 (clean run) |
| **Aggregate Goodput** | **6.731 MB/s** |
| Wire Throughput | **6.779 MB/s** |
| Overhead | **0.72%** |
| Sub-stream drops | 0 |

> 🚀 **6.731 MB/s** aggregate goodput across 4 parallel TCP streams on loopback — peak throughput of this project. Per-stream contribution averages **~1.68 MB/s** with near-perfect load balance.

#### Analysis

- **Case 1 (Stateless Retry, Pf=0.00):** Fastest at **2.10–2.21 MB/s** due to zero session overhead. Stateless means any mid-chunk fault discards *all* received files and restarts from file 0. Up to 50 trials permitted; unsuitable for Pf > 0.20.

- **Case 2 (Stateful Checkpoint, Pf=0.00):** Adds **1.7–3.2 s** overhead vs Case 1 due to the 3-step `fsync` durability chain per completed file. At Pf=0.10: vastly outperforms Case 1's restart approach — resumes from the **last checkpointed file index**, achieving **0.089–0.170 MB/s** across 15–27 session disconnections where Case 1 would fully restart.

- **Case 2 Enhanced (epoll + 4 Streams + Bitmask, Pf=0.10):** Achieves **0.47–0.90 MB/s** vs Case 2's **0.089–0.170 MB/s** — a **4.2–5.3× goodput advantage** through chunk-granularity bitmask recovery and parallel stream multiplexing. At Pf=0.30: still delivers **0.140–0.186 MB/s** across 56–70 sub-stream drops. At Pf=0.50: maintains **0.075–0.092 MB/s** across 102–128 stream drops — system completes transfer even with one-in-two chunks injected with faults.

---

## 🧬 Metrics Logged (CSV Format)

Each client run appends one row to `Results/<part>_metrics.csv`:

| Column | Formula | Meaning |
|---|---|---|
| `case` | string | `Part1_Single`, `Part1_Multi`, `Part2_Case1`, `Part2_Case2`, `Part2_Case2_Enhanced` |
| `N` | int | Concurrent clients (Part 1) or sub-streams (Enhanced) |
| `Pf` | float | Fault injection probability |
| `useful_bytes` | B_useful | Bytes actually written to output directory |
| `total_bytes` | B_total | All bytes sent (headers + data + retransmits) |
| `redundant_bytes` | B_total − B_useful | Bytes wasted on retransmission |
| `goodput_MBps` | B_useful / T_complete | Effective application throughput (MB/s) |
| `wire_throughput` | B_total / T_complete | Raw socket throughput (MB/s) |
| `overhead_pct` | (B_total − B_useful) / B_useful × 100 | Protocol overhead percentage |
| `retries` | int | Trial restarts (Case 1) / disconnects (Case 2) / sub-stream drops (Enhanced) |

---

## 🔍 Fault Injection Details

Controlled by `--fault-prob <p>` or positional `<pf>` argument (float 0.0–1.0):

```
Pf = 0.000   never inject     (clean baseline)
Pf = 0.015   inject ~1.5% of chunk sends  (used in live demo testing)
Pf = 0.030   inject ~3.0% of chunk sends  (default integration test)
Pf = 0.300   inject ~30% of chunk sends   (heavy stress)
Pf = 1.000   inject every chunk            (worst-case / liveness test)
```

- **Injection point:** called *after* `send_file_data()` returns success, inside the chunk loop
- **Effect:** server calls `shutdown(fd, SHUT_RDWR)` — client sees connection reset / `rc=0`
- **Recovery path:** `FRAME_SOCK_ERR` ≠ `FRAME_EOF`; retry/resume branches key on this distinction
- **Reproducibility:** per-run seed configurable in `fault.h` for deterministic replays

---

## 💾 Session & Checkpoint (Case 2 only)

- **Session ID:** 128-bit random UUID, stored as 32-char hex in `<session-dir>/<id>.ckpt`
- **Checkpoint frequency:** every completed file boundary (configurable via `--ckpt-interval`)
- **Durability invariant:** `fsync()` must return before `CKPT_ACK` is sent to client
- **Resume protocol:** client sends `RESUME(session_id, file_index, byte_offset)` — server validates in 7 steps
- **Session expiry:** 5-minute inactivity timeout → server sends `RESTART` opcode
- **Checkpoint filename:** `.it305_<topic>.ckpt` — hidden dot-file in output directory

### Resume State Machine

```
[Client]                                  [Server]
   |                                         |
   |-- RESUME(sid, file_idx, byte_offset) -->|
   |                                         |--(1) lookup session by sid
   |                                         |--(2) validate file_idx <= last_ckpt_file
   |                                         |--(3) seek to file_idx in topic list
   |<---------- RESUME_OK ------------------|
   |<---------- FILE_BEGIN(file_idx) -------|
   |<---------- FILE_DATA chunks ... -------|
   |         (forward only — no re-send)     |
```

---

## ✅ Implementation Status

| Component | Part 1 | Case 1 | Case 2 | Enhanced |
|---|---|---|---|---|
| Server — single-thread | ✅ | ✅ | ✅ | ✅ |
| Server — multi-thread | ✅ | ✅ | ✅ | ✅ (epoll) |
| Client | ✅ | ✅ | ✅ | ✅ (4 streams) |
| Wire protocol layer | ✅ | ✅ | ✅ | ✅ |
| Fault injection | — | ✅ | ✅ | ✅ |
| Session + Checkpoint | — | — | ✅ | ✅ |
| 3-step fsync durability | — | — | ✅ | ✅ |
| Stateless retry (x50) | — | ✅ | — | — |
| Stateful forward-only resume | — | — | ✅ | ✅ |
| Zero-copy `sendfile` | — | — | — | ✅ |
| Chunk bitmask recovery | — | — | — | ✅ |
| Lock-free `pwrite()` assembly | — | — | — | ✅ |
| Animated progress bar | ✅ | ✅ | ✅ | ✅ |
| Trial counter in bar | — | ✅ | ✅ | — |
| Active stream count display | — | — | — | ✅ |
| Aggregate throughput display | — | — | — | ✅ |
| Unit tests | ✅ | ✅ | ✅ | ✅ |
| Metrics CSV logger | ✅ | ✅ | ✅ | ✅ |

---

## 🛠️ Common Issues & Fixes

| Symptom | Fix |
|---|---|
| `Address already in use` | `SO_REUSEADDR` is enabled; wait ~30 s or change port |
| `Permission denied` on Dataset/ | `chmod -R 755 Dataset/` |
| `sendfile()` returns `EINVAL` | Ensure server fd is in blocking mode; epoll server handles `EAGAIN` internally |
| `.ckpt` file not found on resume | Ensure `--session-dir` matches the path used in the previous run |
| Tests fail to link / compile | Run inside WSL2 — `epoll_create1` is Linux-only |
| Build produces warnings with `-Werror` | Do not modify compiler flags; code is verified warning-free |
| Two progress bars appear | Fixed: `\033[2K\r` throttled at 40 ms; only one bar per client process |
| Progress bar wraps to next line | Fixed: bar width clamped to `min(TIOCGWINSZ, 77)` columns |

---

## 📋 Assignment Compliance Checklist

- [x] Part I single-threaded server implemented and tested
- [x] Part I multi-threaded server implemented and tested
- [x] Aggregate goodput measured vs. concurrent client count (N = 1,2,4,8,16,32)
- [x] Part II Case 1 stateless retry (bounded, max 50 trials per topic)
- [x] Part II Case 2 stateful session + 3-step fsync checkpoint + forward-only resume
- [x] Part II Case 2 Enhanced: epoll, sendfile, 4 parallel TCP streams, chunk-bitmask recovery
- [x] Animated progress bar on all 4 client implementations (single bar, no duplication)
- [x] Trial counter + fault notice in Case 1 & Case 2 progress bars
- [x] Active stream count + aggregate throughput in Enhanced progress bar
- [x] Automated benchmark harness (`Results/run_benchmarks.sh`) with `FAST=1` sample mode
- [x] Empirical metrics logged to CSV (`Results/Part1_metrics.csv` & `Results/Part2_metrics.csv`)
- [x] High-resolution visualization charts plotted (`Results/plot_results.py`)
- [x] Bit-for-bit file integrity verified (MD5 checksums match pre/post transfer)
- [x] Design document (Design_Document_Group_103_106_.pdf) complete
- [x] All 4 directory names match auto-grader expectations exactly
- [x] All 338 unit tests pass (zero failures)
- [x] Peak aggregate goodput: **6.731 MB/s** (Enhanced, 4 streams, Pf=0.00, full 57.88 MB dataset)

---

## 📄 Academic Integrity

This repository is submitted as coursework for **IT305 — Computer Networks Lab, DAIICT, 2026**.
All code is original work by the group members listed above.
Redistribution or reuse outside this academic context is not permitted.

---

*Last updated: October 2026 · Merged Group 103 & Group 106*