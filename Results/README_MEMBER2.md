# Member 2 — Protocol Layer Handoff

Branch **`member2-protocol`** · **PR #1**, Wave 1, due **Oct 17** · merges first,
because Members 1, 3, 4 and 5 all call `send_frame()` / `recv_frame()`.

Status: **done, compiles with `-Werror`, 198 assertions passing**, clean under
AddressSanitizer and UndefinedBehaviorSanitizer, warning-free under all four
`-DPART_NUM` / `-DCASE_MODE` combinations.

---

## What goes into PR #1

Exactly two files, copied into four folders:

```
Part1/protocol.h                 Part1/protocol.cpp
Part2_Case1/protocol.h           Part2_Case1/protocol.cpp
Part2_Case2/protocol.h           Part2_Case2/protocol.cpp
Part2_Case2_Enhanced/protocol.h  Part2_Case2_Enhanced/protocol.cpp
```

All four copies are byte-identical. One implementation serves every case —
Part I simply never calls the opcodes above `0x07`.

**`_local_test_NOT_submitted/` is not committed.** It holds the standalone unit
test and a reference `common_types.h` so the test can compile on its own. The
auto-grader checklist forbids loose folders in the submission, and the plan
assigns Member 2 only `protocol.h` and `protocol.cpp`.

## Before you commit

`common_types.h` is **PR #0**, owned by the whole group, not by Member 2. Pull
the real one from `github.com/nisargvekariya01/CN_Project` and drop it into
`_local_test_NOT_submitted/` replacing the reference copy, then re-run the test.
If it still passes, the header contract matches and PR #1 is safe to open.

```bash
cd _local_test_NOT_submitted
make test      # 198 assertions
make asan      # same tests under ASan + UBSan
make modes     # warning-free under all 4 Makefile define combos
make clean
```

The test compiles `../Part1/protocol.cpp` directly, so it always exercises the
exact file being committed rather than a stale copy.

---

## B_total accounting — read this if you touch metrics

Every byte-moving function takes an optional trailing `uint64_t* wire_counter`.
When non-null it accumulates **every byte that crossed the socket**, frame
headers included:

```cpp
uint64_t wire_bytes = 0;                      // B_total for this connection

send_file_data(fd, idx, off, buf, len, &wire_bytes);
recv_frame(fd, &hdr, payload, &wire_bytes);
```

Member 6's `record_transfer_metrics()` needs this:

```
Wire Throughput (MB/s) = B_total / (T_complete × 10^6)
Overhead (%)           = ((B_total − B_useful) / B_useful) × 100
```

Two details that matter:

- **Bytes sent just before a fault still count.** The counter increments inside
  the send/recv loop, not after it, so a chunk that was half-transmitted when
  the server dropped the connection contributes to `B_total`. That is exactly
  what `redundant_bytes` is supposed to measure — if those bytes were dropped
  from the count, Case 1's overhead would look identical to Case 2's and the
  Section 9 comparison would be meaningless.
- **A refused frame costs zero.** `send_frame()` rejects a payload over 1 MB
  before writing anything, so the counter is untouched.

Pass `nullptr` (the default) wherever you do not care.

---

## Sending

```cpp
#include "protocol.h"

send_get(fd, "Cane", &wire);                          // 0x01
send_get_ok(fd, sid, file_count, total_bytes, &wire); // 0x02
send_error(fd, ERR_TOPIC_NOT_FOUND, "TOPIC_NOT_FOUND", &wire);   // 0x03
send_file_begin(fd, idx, offset, bytes_following, name, &wire);  // 0x04
send_file_data(fd, idx, offset, buf, len, &wire);     // 0x05
send_file_end(fd, idx, &wire);                        // 0x06
send_transfer_end(fd, files_sent, useful_bytes, &wire);          // 0x07
send_ckpt_ack(fd, sid, idx, ack_offset, &wire);       // 0x08
send_resume(fd, sid, idx, offset, topic, filename, &wire);       // 0x09
send_resume_ok(fd, idx, accepted, rem_files, rem_bytes, &wire);  // 0x0A
send_restart(fd, ERR_SESSION_EXPIRED, "SESSION_EXPIRED", &wire); // 0x0B
send_get_enhanced(fd, 4, topic, &wire);               // 0x0C
send_join_session(fd, sid, conn_id, bitmap, bmap_len, &wire);    // 0x0D
send_assign_work(fd, conn_id, idx, offset, chunk_len, &wire);    // 0x0E
```

All return `0` on success, `-1` if the socket is gone.

`0x03` and `0x0B` share one payload layout and one serializer
(`serialize_status_msg`), but keep separate send helpers because they mean
different things: `send_error()` for a request that can never succeed,
`send_restart()` for a resume that was rejected.

## Receiving

```cpp
FrameHeader hdr;
std::vector<uint8_t> payload;

int rc = recv_frame(fd, &hdr, payload, &wire_bytes);

if (rc == FRAME_EOF) {
    // peer closed cleanly with no message pending
} else if (rc == FRAME_SOCK_ERR) {
    // dropped mid-stream — THIS is what a fault injection looks like
} else if (rc == FRAME_PROTO_ERR) {
    // garbage on the wire, close the connection
} else {                      // rc == FRAME_OK
    switch (hdr.type) {
    case OP_FILE_DATA: {
        MsgFileData m;
        if (!parse_file_data(payload.data(), hdr.payload_length, &m)) {
            // malformed payload — treat exactly like FRAME_PROTO_ERR
            break;
        }
        // use m.file_index, m.offset, m.data
        break;
    }
    // ... other opcodes
    }
}
```

### The four return codes

| Code | Meaning | What you normally do |
|---|---|---|
| `FRAME_OK` (1) | good frame | parse it |
| `FRAME_EOF` (0) | clean close, nothing pending | transfer finished or peer quit |
| `FRAME_SOCK_ERR` (−1) | dropped mid-stream | **reconnect / retry path** |
| `FRAME_PROTO_ERR` (−2) | bad magic, version, or payload > 1 MB | close, do not retry |

A fault injected mid-chunk surfaces as `FRAME_SOCK_ERR`, not `FRAME_EOF`.
Case 1's bounded retry loop and Case 2's `RESUME` logic both branch on exactly
this distinction, so don't collapse the two.

---

## Notes per member

**Member 1 (`server.cpp`, PR #3)** — `send_all`/`recv_all` retry on `EINTR`, so
your `SIGINT`/`SIGTERM` handler won't corrupt an in-flight frame. `send()` uses
`MSG_NOSIGNAL`, so a dead client gives `EPIPE` rather than killing the process.

**Member 3 (`fault.cpp`, PR #4)** — call `should_inject_fault()` *after*
`send_file_data()` returns 0, as the plan specifies. The client will see
`FRAME_SOCK_ERR`. In `run_client_part1_and_case1_loop()`, the bytes you add to
`redundant_bytes` are already in `*wire_counter` if you passed one — don't
double-count.

**Member 4 (`session.cpp` / `checkpoint.cpp`, PR #5)** — `zero_session_id()`,
`session_id_to_hex()` and `hex_to_session_id()` keep your `.ckpt` file and the
wire format consistent. The 32-char hex form is for disk and logs only; the wire
always carries 16 raw bytes. The terminal state (`file_index == file_count`,
`byte_offset == 0`, `file_name == "EOF"`) is covered by the test suite.

**Member 5 (`enhanced_epoll_server.cpp`, PR #6)** — `send_chunk_zerocopy()`
can't use `send_frame()`, because `sendfile()` never brings the payload into
user space. Use the builder instead, so the wire format stays defined in one
place:

```cpp
uint8_t hdr[FILE_DATA_HEADER_SIZE];           // 24 = 8 frame + 16 prefix
build_file_data_header(hdr, unit.file_index, unit.file_offset, unit.chunk_len);
if (send_all(client_fd, hdr, sizeof(hdr), wire_counter) < 0) return -1;

off_t off = static_cast<off_t>(unit.file_offset);
// sendfile() loop handling EAGAIN/EWOULDBLOCK …
// then add the bytes sendfile() reports to *wire_counter yourself,
// since those bytes never pass through send_all().
```

Test 3 asserts these 24 bytes are byte-for-byte identical to what
`send_frame()` emits for the same chunk, so a client cannot tell the two paths
apart. Also: `send_join_session()` accepts a `NULL` bitmap with `bmap_len = 0`
for the first connect, and keep chunks at 64 KiB — `send_frame()` refuses
anything over 1 MB.

**Member 6 (`metrics_logger.cpp`, PR #2)** — `opcode_name(hdr.type)` gives a
printable name for `[INFO]` / `[FAULT]` log lines. `B_total` comes from the
`wire_counter` described above; `B_useful` is the sum of `MsgFileData::data`
sizes actually written to disk.

---

## What the layer guarantees

- Every integer on the wire is fixed-width big-endian. No struct is ever
  `memcpy`'d onto a socket, so there are no padding surprises between machines.
- Filenames may contain spaces or any byte; strings are length-prefixed, not
  delimiter-separated.
- A short `send()` or `recv()` is looped internally, so a 64 KB chunk arrives
  whole or not at all.
- Parsers reject truncated payloads *and* payloads with trailing bytes, so a
  lying length field can never cause an over-read.
- The zero-copy preamble and the normal path produce identical bytes.
