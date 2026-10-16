/*
 * test_protocol.cpp - Member 2 standalone unit test
 *
 * NOT A SUBMISSION FILE. This lives outside Part1/ .. Part2_Case2_Enhanced/
 * and is never committed in PR #1 - only protocol.h and protocol.cpp are.
 *
 * Proves that protocol.cpp works WITHOUT any other member's code:
 *   - a local socketpair() replaces the real TCP connection
 *   - every opcode 0x01..0x0E is serialized into sv[0] and parsed from sv[1]
 *   - every field is asserted to come back bit-exact
 *   - wire_counter (B_total) is asserted to the exact byte
 *   - the zero-copy header builders are asserted byte-identical to send_frame()
 *   - malformed frames are rejected with FRAME_PROTO_ERR
 *   - a clean close is FRAME_EOF, a mid-frame drop is FRAME_SOCK_ERR
 *
 * Build:  g++ -O2 -Wall -Wextra -Werror -pthread -std=c++17 \
 *             test_protocol.cpp protocol.cpp -o test_protocol
 * Run:    ./test_protocol
 */

#include "protocol.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string>
#include <vector>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool cond, const char *what)
{
    if (cond) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", what);
    }
}

static void check_u64(uint64_t got, uint64_t want, const char *what)
{
    if (got == want) {
        g_pass++;
    } else {
        g_fail++;
        printf("  [FAIL] %s (got %llu, want %llu)\n", what,
               static_cast<unsigned long long>(got),
               static_cast<unsigned long long>(want));
    }
}

static void banner(const char *name)
{
    printf("\n--- %s ---\n", name);
}

/* Make a predictable 16-byte session id so we can assert on it. */
static void make_session_id(uint8_t sid[16], uint8_t seed)
{
    for (int i = 0; i < 16; i++)
        sid[i] = static_cast<uint8_t>(seed + i);
}

/* Read one frame and assert the opcode is what we expected. */
static bool pull(int fd, uint8_t expect_op, FrameHeader *hdr,
                 std::vector<uint8_t> &payload, uint64_t *rx_counter = nullptr)
{
    int rc = recv_frame(fd, hdr, payload, rx_counter);
    if (rc != FRAME_OK) {
        printf("  [FAIL] recv_frame returned %d while waiting for %s\n",
               rc, opcode_name(expect_op));
        g_fail++;
        return false;
    }
    if (hdr->type != expect_op) {
        printf("  [FAIL] expected opcode %s, got %s\n",
               opcode_name(expect_op), opcode_name(hdr->type));
        g_fail++;
        return false;
    }
    check(hdr->magic == PROTOCOL_MAGIC, "magic is 0x4954");
    check(hdr->version == PROTOCOL_VERSION, "version is 0x01");
    check(hdr->payload_length == payload.size(), "payload_length matches body");
    return true;
}

/* ================================================================== */
/* Test 1 : round trip of every opcode, with B_total accounting        */
/* ================================================================== */
static void test_all_opcodes(void)
{
    banner("Test 1: round-trip of all 14 opcodes (+ wire_counter)");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("  [FAIL] socketpair() failed\n");
        g_fail++;
        return;
    }

    FrameHeader hdr;
    std::vector<uint8_t> payload;

    /* Both ends keep their own B_total. They must agree at the end. */
    uint64_t tx = 0;
    uint64_t rx = 0;
    uint64_t expect = 0;          /* 8-byte header + payload, per frame */

    uint8_t sid[16];
    make_session_id(sid, 0xA0);

    /* ---- 0x01 GET ---- */
    check(send_get(sv[0], "Cane", &tx) == 0, "send GET");
    expect += 8 + (2 + 4);
    if (pull(sv[1], OP_GET, &hdr, payload, &rx)) {
        MsgGet m;
        check(parse_get(payload.data(), hdr.payload_length, &m), "parse GET");
        check(m.topic == "Cane", "GET.topic == Cane");
    }
    check_u64(tx, expect, "B_total after GET");

    /* ---- 0x02 GET_OK ---- */
    check(send_get_ok(sv[0], sid, 1234u, 9876543210ULL, &tx) == 0, "send GET_OK");
    expect += 8 + (16 + 4 + 8);
    if (pull(sv[1], OP_GET_OK, &hdr, payload, &rx)) {
        MsgGetOk m;
        check(parse_get_ok(payload.data(), hdr.payload_length, &m), "parse GET_OK");
        check(memcmp(m.session_id, sid, 16) == 0, "GET_OK.session_id round trip");
        check(m.file_count == 1234u, "GET_OK.file_count");
        check(m.total_bytes == 9876543210ULL, "GET_OK.total_bytes (64-bit)");
    }
    check_u64(tx, expect, "B_total after GET_OK");

    /* ---- 0x03 ERROR ---- */
    check(send_error(sv[0], ERR_TOPIC_NOT_FOUND, "TOPIC_NOT_FOUND", &tx) == 0,
          "send ERROR");
    expect += 8 + (2 + 2 + 15);
    if (pull(sv[1], OP_ERROR, &hdr, payload, &rx)) {
        MsgStatus m;
        check(parse_status_msg(payload.data(), hdr.payload_length, &m), "parse ERROR");
        check(m.code == 404, "ERROR.code == 404");
        check(m.reason == "TOPIC_NOT_FOUND", "ERROR.reason");
    }
    check_u64(tx, expect, "B_total after ERROR");

    /* ---- 0x04 FILE_BEGIN (filename with a space, to prove no delimiters) ---- */
    check(send_file_begin(sv[0], 7u, 262144ULL, 1048576ULL, "my photo 01.jpeg", &tx) == 0,
          "send FILE_BEGIN");
    expect += 8 + (4 + 8 + 8 + 2 + 16);
    if (pull(sv[1], OP_FILE_BEGIN, &hdr, payload, &rx)) {
        MsgFileBegin m;
        check(parse_file_begin(payload.data(), hdr.payload_length, &m), "parse FILE_BEGIN");
        check(m.file_index == 7u, "FILE_BEGIN.file_index");
        check(m.byte_offset == 262144ULL, "FILE_BEGIN.byte_offset");
        check(m.bytes_following == 1048576ULL, "FILE_BEGIN.bytes_following");
        check(m.filename == "my photo 01.jpeg", "FILE_BEGIN.filename with space");
    }
    check_u64(tx, expect, "B_total after FILE_BEGIN");

    /* ---- 0x05 FILE_DATA : a full 64 KB chunk of binary bytes ---- */
    std::vector<uint8_t> chunk(CHUNK_SIZE);
    for (size_t i = 0; i < chunk.size(); i++)
        chunk[i] = static_cast<uint8_t>(i * 31 + 7);   /* includes 0x00 and 0xFF */

    check(send_file_data(sv[0], 7u, 131072ULL, chunk.data(),
                         static_cast<uint32_t>(chunk.size()), &tx) == 0,
          "send FILE_DATA 64KB");
    expect += 8 + (16 + CHUNK_SIZE);
    if (pull(sv[1], OP_FILE_DATA, &hdr, payload, &rx)) {
        MsgFileData m;
        check(parse_file_data(payload.data(), hdr.payload_length, &m), "parse FILE_DATA");
        check(m.file_index == 7u, "FILE_DATA.file_index");
        check(m.offset == 131072ULL, "FILE_DATA.offset");
        check(m.data.size() == chunk.size(), "FILE_DATA.data length");
        check(memcmp(m.data.data(), chunk.data(), chunk.size()) == 0,
              "FILE_DATA 64KB binary payload is bit-exact");
    }
    check_u64(tx, expect, "B_total after 64KB FILE_DATA");

    /* ---- 0x05 FILE_DATA : empty chunk edge case ---- */
    check(send_file_data(sv[0], 0u, 0ULL, NULL, 0u, &tx) == 0, "send FILE_DATA empty");
    expect += 8 + 16;
    if (pull(sv[1], OP_FILE_DATA, &hdr, payload, &rx)) {
        MsgFileData m;
        check(parse_file_data(payload.data(), hdr.payload_length, &m),
              "parse FILE_DATA empty");
        check(m.data.empty(), "FILE_DATA empty data");
    }

    /* ---- 0x06 FILE_END ---- */
    check(send_file_end(sv[0], 42u, &tx) == 0, "send FILE_END");
    expect += 8 + 4;
    if (pull(sv[1], OP_FILE_END, &hdr, payload, &rx)) {
        MsgFileEnd m;
        check(parse_file_end(payload.data(), hdr.payload_length, &m), "parse FILE_END");
        check(m.file_index == 42u, "FILE_END.file_index");
    }

    /* ---- 0x07 TRANSFER_END ---- */
    check(send_transfer_end(sv[0], 2623u, 150000000ULL, &tx) == 0, "send TRANSFER_END");
    expect += 8 + (4 + 8);
    if (pull(sv[1], OP_TRANSFER_END, &hdr, payload, &rx)) {
        MsgTransferEnd m;
        check(parse_transfer_end(payload.data(), hdr.payload_length, &m),
              "parse TRANSFER_END");
        check(m.files_sent == 2623u, "TRANSFER_END.files_sent");
        check(m.useful_bytes == 150000000ULL, "TRANSFER_END.useful_bytes");
    }

    /* ---- 0x08 CHECKPOINT_ACK ---- */
    check(send_ckpt_ack(sv[0], sid, 3u, 196608ULL, &tx) == 0, "send CHECKPOINT_ACK");
    expect += 8 + (16 + 4 + 8);
    if (pull(sv[1], OP_CHECKPOINT_ACK, &hdr, payload, &rx)) {
        MsgCkptAck m;
        check(parse_ckpt_ack(payload.data(), hdr.payload_length, &m),
              "parse CHECKPOINT_ACK");
        check(memcmp(m.session_id, sid, 16) == 0, "CHECKPOINT_ACK.session_id");
        check(m.file_index == 3u, "CHECKPOINT_ACK.file_index");
        check(m.ack_offset == 196608ULL, "CHECKPOINT_ACK.ack_offset");
    }

    /* ---- 0x09 RESUME ---- */
    check(send_resume(sv[0], sid, 5u, 262144ULL, "Cavallo", "OIP-xyz.jpeg", &tx) == 0,
          "send RESUME");
    expect += 8 + (16 + 4 + 8 + 2 + 7 + 2 + 12);
    if (pull(sv[1], OP_RESUME, &hdr, payload, &rx)) {
        MsgResume m;
        check(parse_resume(payload.data(), hdr.payload_length, &m), "parse RESUME");
        check(memcmp(m.session_id, sid, 16) == 0, "RESUME.session_id");
        check(m.file_index == 5u, "RESUME.file_index");
        check(m.byte_offset == 262144ULL, "RESUME.byte_offset");
        check(m.topic == "Cavallo", "RESUME.topic");
        check(m.filename == "OIP-xyz.jpeg", "RESUME.filename");
    }
    check_u64(tx, expect, "B_total after RESUME");

    /* ---- 0x09 RESUME : terminal EOF state (file_index == file_count) ---- */
    check(send_resume(sv[0], sid, 2623u, 0ULL, "Cane", "EOF", &tx) == 0,
          "send RESUME EOF");
    expect += 8 + (16 + 4 + 8 + 2 + 4 + 2 + 3);
    if (pull(sv[1], OP_RESUME, &hdr, payload, &rx)) {
        MsgResume m;
        check(parse_resume(payload.data(), hdr.payload_length, &m), "parse RESUME EOF");
        check(m.file_index == 2623u && m.byte_offset == 0ULL && m.filename == "EOF",
              "RESUME terminal EOF state");
    }

    /* ---- 0x0A RESUME_OK ---- */
    check(send_resume_ok(sv[0], 5u, 262144ULL, 11u, 5000000ULL, &tx) == 0,
          "send RESUME_OK");
    expect += 8 + (4 + 8 + 4 + 8);
    if (pull(sv[1], OP_RESUME_OK, &hdr, payload, &rx)) {
        MsgResumeOk m;
        check(parse_resume_ok(payload.data(), hdr.payload_length, &m), "parse RESUME_OK");
        check(m.file_index == 5u, "RESUME_OK.file_index");
        check(m.accepted_offset == 262144ULL, "RESUME_OK.accepted_offset");
        check(m.rem_files == 11u, "RESUME_OK.rem_files");
        check(m.rem_bytes == 5000000ULL, "RESUME_OK.rem_bytes");
    }

    /* ---- 0x0B RESTART (same layout as ERROR, different opcode) ---- */
    check(send_restart(sv[0], ERR_SESSION_EXPIRED, "SESSION_EXPIRED", &tx) == 0,
          "send RESTART");
    expect += 8 + (2 + 2 + 15);
    if (pull(sv[1], OP_RESTART, &hdr, payload, &rx)) {
        MsgStatus m;
        check(parse_status_msg(payload.data(), hdr.payload_length, &m), "parse RESTART");
        check(m.code == 410, "RESTART.code == 410");
        check(m.reason == "SESSION_EXPIRED", "RESTART.reason");
    }

    /* ---- 0x0C GET_ENHANCED ---- */
    check(send_get_enhanced(sv[0], 4u, "Elefante", &tx) == 0, "send GET_ENHANCED");
    expect += 8 + (2 + 2 + 8);
    if (pull(sv[1], OP_GET_ENHANCED, &hdr, payload, &rx)) {
        MsgGetEnhanced m;
        check(parse_get_enhanced(payload.data(), hdr.payload_length, &m),
              "parse GET_ENHANCED");
        check(m.num_streams == 4u, "GET_ENHANCED.num_streams");
        check(m.topic == "Elefante", "GET_ENHANCED.topic");
    }

    /* ---- 0x0D JOIN_SESSION with a chunk bitmap ---- */
    std::vector<uint8_t> bmap(300);
    for (size_t i = 0; i < bmap.size(); i++)
        bmap[i] = static_cast<uint8_t>(i % 251);

    check(send_join_session(sv[0], sid, 2u, bmap.data(),
                            static_cast<uint32_t>(bmap.size()), &tx) == 0,
          "send JOIN_SESSION");
    expect += 8 + (16 + 4 + 4 + 300);
    if (pull(sv[1], OP_JOIN_SESSION, &hdr, payload, &rx)) {
        MsgJoinSession m;
        check(parse_join_session(payload.data(), hdr.payload_length, &m),
              "parse JOIN_SESSION");
        check(memcmp(m.session_id, sid, 16) == 0, "JOIN_SESSION.session_id");
        check(m.conn_id == 2u, "JOIN_SESSION.conn_id");
        check(m.chunk_bitmap.size() == bmap.size(), "JOIN_SESSION.bmap_len");
        check(memcmp(m.chunk_bitmap.data(), bmap.data(), bmap.size()) == 0,
              "JOIN_SESSION.chunk_bitmap bit-exact");
    }

    /* ---- 0x0D JOIN_SESSION with an empty bitmap (first connect) ---- */
    check(send_join_session(sv[0], sid, 0u, NULL, 0u, &tx) == 0,
          "send JOIN_SESSION empty");
    expect += 8 + (16 + 4 + 4);
    if (pull(sv[1], OP_JOIN_SESSION, &hdr, payload, &rx)) {
        MsgJoinSession m;
        check(parse_join_session(payload.data(), hdr.payload_length, &m),
              "parse JOIN_SESSION empty");
        check(m.chunk_bitmap.empty(), "JOIN_SESSION empty bitmap");
    }

    /* ---- 0x0E ASSIGN_WORK ---- */
    check(send_assign_work(sv[0], 3u, 9u, 655360ULL, 65536u, &tx) == 0,
          "send ASSIGN_WORK");
    expect += 8 + (4 + 4 + 8 + 4);
    if (pull(sv[1], OP_ASSIGN_WORK, &hdr, payload, &rx)) {
        MsgAssignWork m;
        check(parse_assign_work(payload.data(), hdr.payload_length, &m),
              "parse ASSIGN_WORK");
        check(m.conn_id == 3u, "ASSIGN_WORK.conn_id");
        check(m.file_index == 9u, "ASSIGN_WORK.file_index");
        check(m.chunk_offset == 655360ULL, "ASSIGN_WORK.chunk_offset");
        check(m.chunk_len == 65536u, "ASSIGN_WORK.chunk_len");
    }

    /* ---- B_total must agree on both ends and match the hand count ---- */
    check_u64(tx, expect, "sender B_total matches hand-computed total");
    check_u64(rx, expect, "receiver B_total matches hand-computed total");
    check_u64(tx, rx, "sender and receiver agree on B_total");

    close(sv[0]);
    close(sv[1]);
}

/* ================================================================== */
/* Test 2 : the wire header really is big-endian                       */
/* ================================================================== */
static void test_header_bytes_on_wire(void)
{
    banner("Test 2: raw header bytes are big-endian");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("  [FAIL] socketpair() failed\n");
        g_fail++;
        return;
    }

    uint8_t body[5] = { 1, 2, 3, 4, 5 };
    uint64_t tx = 0;
    check(send_frame(sv[0], OP_FILE_END, body, 5, &tx) == 0, "send raw frame");
    check_u64(tx, 13, "a 5-byte payload costs exactly 13 wire bytes");

    uint8_t raw[13];
    uint64_t rx = 0;
    check(recv_all(sv[1], raw, sizeof(raw), &rx) == static_cast<ssize_t>(sizeof(raw)),
          "read 13 raw bytes");
    check_u64(rx, 13, "recv_all counted 13 bytes");

    check(raw[0] == 0x49, "byte 0 == 0x49 ('I')");
    check(raw[1] == 0x54, "byte 1 == 0x54 ('T')");
    check(raw[2] == 0x01, "byte 2 == version 0x01");
    check(raw[3] == 0x06, "byte 3 == opcode FILE_END");
    check(raw[4] == 0x00 && raw[5] == 0x00 && raw[6] == 0x00 && raw[7] == 0x05,
          "bytes 4..7 == length 5, big-endian");
    check(raw[8] == 1 && raw[12] == 5, "payload follows header");

    close(sv[0]);
    close(sv[1]);
}

/* ================================================================== */
/* Test 3 : zero-copy header builders agree with send_frame()          */
/* ================================================================== */
static void test_zerocopy_headers(void)
{
    banner("Test 3: build_file_data_header matches send_frame byte-for-byte");

    const uint32_t file_index = 9;
    const uint64_t offset     = 655360ULL;
    const uint32_t data_len   = CHUNK_SIZE;

    /* (a) What send_frame() would put on the wire for this chunk. */
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("  [FAIL] socketpair() failed\n");
        g_fail++;
        return;
    }

    std::vector<uint8_t> chunk(data_len);
    for (size_t i = 0; i < chunk.size(); i++)
        chunk[i] = static_cast<uint8_t>(i * 17 + 3);

    uint64_t tx = 0;
    check(send_file_data(sv[0], file_index, offset, chunk.data(), data_len, &tx) == 0,
          "send FILE_DATA via send_frame");
    check_u64(tx, 8 + 16 + data_len, "normal path wire cost");

    uint8_t from_send_frame[FILE_DATA_HEADER_SIZE];
    check(recv_all(sv[1], from_send_frame, sizeof(from_send_frame))
              == static_cast<ssize_t>(sizeof(from_send_frame)),
          "read the first 24 bytes send_frame produced");

    /* Drain the rest so the socket closes cleanly. */
    std::vector<uint8_t> drain(data_len);
    recv_all(sv[1], drain.data(), data_len);
    check(memcmp(drain.data(), chunk.data(), data_len) == 0,
          "normal path payload intact");

    close(sv[0]);
    close(sv[1]);

    /* (b) What Member 5's zero-copy path builds by hand. */
    uint8_t from_builder[FILE_DATA_HEADER_SIZE];
    build_file_data_header(from_builder, file_index, offset, data_len);

    check(memcmp(from_builder, from_send_frame, FILE_DATA_HEADER_SIZE) == 0,
          "zero-copy 24-byte preamble is identical to send_frame's");

    /* (c) And that preamble decodes back correctly. */
    check(from_builder[0] == 0x49 && from_builder[1] == 0x54,
          "zero-copy header carries the magic");
    check(from_builder[3] == OP_FILE_DATA, "zero-copy header opcode is FILE_DATA");

    uint32_t len_be;
    memcpy(&len_be, from_builder + 4, 4);
    check_u64(ntohl(len_be), 16 + data_len,
              "zero-copy payload_length covers prefix + data");

    /* (d) The 8-byte generic builder must match the first 8 bytes. */
    uint8_t generic[FRAME_HEADER_SIZE];
    build_frame_header(generic, OP_FILE_DATA, FILE_DATA_PREFIX_SIZE + data_len);
    check(memcmp(generic, from_builder, FRAME_HEADER_SIZE) == 0,
          "build_frame_header matches the first 8 bytes");

    /* (e) The 16-byte prefix must parse as a FILE_DATA payload head. */
    uint8_t prefix[FILE_DATA_PREFIX_SIZE];
    build_file_data_prefix(prefix, file_index, offset, data_len);
    check(memcmp(prefix, from_builder + FRAME_HEADER_SIZE, FILE_DATA_PREFIX_SIZE) == 0,
          "build_file_data_prefix matches bytes 8..23");
}

/* Helper: push a hand-made bad header and check recv_frame's verdict. */
static void bad_header_case(const uint8_t hdr[8], int expected_rc, const char *what)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("  [FAIL] socketpair() failed\n");
        g_fail++;
        return;
    }

    send_all(sv[0], hdr, 8);

    FrameHeader fh;
    std::vector<uint8_t> payload;
    int rc = recv_frame(sv[1], &fh, payload);
    check(rc == expected_rc, what);

    close(sv[0]);
    close(sv[1]);
}

/* ================================================================== */
/* Test 4 : malformed frames are rejected                              */
/* ================================================================== */
static void test_bad_frames(void)
{
    banner("Test 4: malformed frames rejected with FRAME_PROTO_ERR");

    /* wrong magic (0xDEAD instead of 0x4954) */
    {
        uint8_t h[8] = { 0xDE, 0xAD, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00 };
        bad_header_case(h, FRAME_PROTO_ERR, "bad magic -> FRAME_PROTO_ERR");
    }
    /* wrong version */
    {
        uint8_t h[8] = { 0x49, 0x54, 0x09, 0x01, 0x00, 0x00, 0x00, 0x00 };
        bad_header_case(h, FRAME_PROTO_ERR, "bad version -> FRAME_PROTO_ERR");
    }
    /* payload_length = 2 MB, above the 1 MB cap */
    {
        uint8_t h[8] = { 0x49, 0x54, 0x01, 0x01, 0x00, 0x20, 0x00, 0x00 };
        bad_header_case(h, FRAME_PROTO_ERR, "oversized length -> FRAME_PROTO_ERR");
    }

    /* send_frame itself must refuse a payload above 1 MB */
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            std::vector<uint8_t> big(MAX_FRAME_PAYLOAD + 1, 0xAB);
            uint64_t tx = 0;
            check(send_frame(sv[0], OP_FILE_DATA, big.data(),
                             static_cast<uint32_t>(big.size()), &tx) == -1,
                  "send_frame refuses payload > 1 MB");
            check_u64(tx, 0, "a refused frame costs zero wire bytes");
            close(sv[0]);
            close(sv[1]);
        }
    }
}

/* ================================================================== */
/* Test 5 : clean EOF and mid-frame drop are distinguished             */
/* ================================================================== */
static void test_eof_and_drop(void)
{
    banner("Test 5: FRAME_EOF vs FRAME_SOCK_ERR");

    /* (a) peer closes with nothing pending -> clean EOF */
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            close(sv[0]);
            FrameHeader fh;
            std::vector<uint8_t> payload;
            uint64_t rx = 0;
            check(recv_frame(sv[1], &fh, payload, &rx) == FRAME_EOF,
                  "clean close -> FRAME_EOF");
            check_u64(rx, 0, "a clean EOF costs zero wire bytes");
            close(sv[1]);
        }
    }

    /* (b) peer sends a valid header, then dies before the body arrives.
     *     This is exactly what the fault injector does mid-chunk. The bytes
     *     that did arrive still count toward B_total (redundant_bytes). */
    {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            uint8_t h[8];
            build_frame_header(h, OP_FILE_DATA, 100u);

            send_all(sv[0], h, 8);
            send_all(sv[0], h, 8);          /* only 8 of the 100 promised bytes */
            close(sv[0]);

            FrameHeader fh;
            std::vector<uint8_t> payload;
            uint64_t rx = 0;
            check(recv_frame(sv[1], &fh, payload, &rx) == FRAME_SOCK_ERR,
                  "truncated body -> FRAME_SOCK_ERR");
            check_u64(rx, 16, "partial bytes before the drop still count in B_total");
            close(sv[1]);
        }
    }
}

/* ================================================================== */
/* Test 6 : truncated / over-long payloads are rejected by parsers     */
/* ================================================================== */
static void test_parser_guards(void)
{
    banner("Test 6: parsers reject malformed payloads");

    /* GET claims a 10-byte topic but only 2 bytes follow */
    {
        uint8_t p[4] = { 0x00, 0x0A, 'C', 'a' };
        MsgGet m;
        check(parse_get(p, 4, &m) == false, "truncated GET rejected");
    }
    /* GET with trailing garbage must also fail (strict full-consume rule) */
    {
        uint8_t p[7] = { 0x00, 0x04, 'C', 'a', 'n', 'e', 0xFF };
        MsgGet m;
        check(parse_get(p, 7, &m) == false, "GET with trailing byte rejected");
    }
    /* FILE_END needs exactly 4 bytes */
    {
        uint8_t p[3] = { 0x00, 0x00, 0x00 };
        MsgFileEnd m;
        check(parse_file_end(p, 3, &m) == false, "short FILE_END rejected");
    }
    /* empty payload where fields are required */
    {
        MsgGetOk m;
        check(parse_get_ok(NULL, 0, &m) == false, "empty GET_OK rejected");
    }
    /* FILE_DATA claiming more data than present */
    {
        uint8_t p[FILE_DATA_PREFIX_SIZE];
        memset(p, 0, sizeof(p));
        p[15] = 0xFF;                        /* data_len = 255, but 0 bytes follow */
        MsgFileData m;
        check(parse_file_data(p, sizeof(p), &m) == false,
              "FILE_DATA with lying data_len rejected");
    }
    /* RESUME truncated in the middle of the filename string */
    {
        uint8_t p[32];
        memset(p, 0, sizeof(p));
        p[30] = 0x00;
        p[31] = 0x40;                        /* topic_len = 64, nothing follows */
        MsgResume m;
        check(parse_resume(p, sizeof(p), &m) == false, "truncated RESUME rejected");
    }
}

/* ================================================================== */
/* Test 7 : session id helpers                                         */
/* ================================================================== */
static void test_session_id_helpers(void)
{
    banner("Test 7: session id zero / hex helpers");

    uint8_t sid[16];
    zero_session_id(sid);
    bool all_zero = true;
    for (int i = 0; i < 16; i++)
        if (sid[i] != 0)
            all_zero = false;
    check(all_zero, "zero_session_id() gives 16 zero bytes (Part I / Case 1)");

    uint8_t original[16];
    make_session_id(original, 0x10);

    char hex[33];
    session_id_to_hex(original, hex);
    check(strlen(hex) == 32, "hex string is 32 chars");
    check(strcmp(hex, "101112131415161718191a1b1c1d1e1f") == 0, "hex value correct");

    uint8_t back[16];
    check(hex_to_session_id(hex, back), "hex parses back");
    check(memcmp(back, original, 16) == 0, "hex round trip is exact");
    check(hex_to_session_id("zzzz", back) == false, "malformed hex rejected");

    /* The all-zero id must survive the hex round trip too, since Case 1
     * checkpoints store it as 32 '0' characters. */
    char zhex[33];
    zero_session_id(sid);
    session_id_to_hex(sid, zhex);
    check(strcmp(zhex, "00000000000000000000000000000000") == 0,
          "zero session id renders as 32 zeros");
}

/* ================================================================== */
/* Test 8 : a realistic Part I transfer, with overhead computed        */
/* ================================================================== */
static void test_full_sequence(void)
{
    banner("Test 8: full Part I message sequence + overhead math");

    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        printf("  [FAIL] socketpair() failed\n");
        g_fail++;
        return;
    }

    uint8_t sid[16];
    zero_session_id(sid);                     /* Part I: all zeros */

    /* Two small "files" of 3 chunks and 1 chunk. */
    const uint32_t kChunk = 4096;
    std::vector<uint8_t> buf(kChunk, 0xC3);

    uint64_t tx = 0;
    uint64_t useful = 0;                      /* B_useful: file bytes only */

    check(send_get(sv[0], "Cane", &tx) == 0, "GET");
    check(send_get_ok(sv[0], sid, 2u, 4 * kChunk, &tx) == 0, "GET_OK");

    /* file 0: three chunks */
    check(send_file_begin(sv[0], 0u, 0ULL, 3ULL * kChunk, "a.jpeg", &tx) == 0,
          "FILE_BEGIN 0");
    for (int i = 0; i < 3; i++) {
        check(send_file_data(sv[0], 0u, static_cast<uint64_t>(i) * kChunk,
                             buf.data(), kChunk, &tx) == 0, "FILE_DATA");
        useful += kChunk;
    }
    check(send_file_end(sv[0], 0u, &tx) == 0, "FILE_END 0");

    /* file 1: one chunk */
    check(send_file_begin(sv[0], 1u, 0ULL, kChunk, "b.jpeg", &tx) == 0,
          "FILE_BEGIN 1");
    check(send_file_data(sv[0], 1u, 0ULL, buf.data(), kChunk, &tx) == 0, "FILE_DATA");
    useful += kChunk;
    check(send_file_end(sv[0], 1u, &tx) == 0, "FILE_END 1");

    check(send_transfer_end(sv[0], 2u, useful, &tx) == 0, "TRANSFER_END");

    /* Now read the whole sequence back in order. */
    FrameHeader hdr;
    std::vector<uint8_t> payload;
    uint64_t rx = 0;
    uint64_t received_useful = 0;
    uint32_t data_frames = 0;

    const uint8_t expected_order[] = {
        OP_GET, OP_GET_OK,
        OP_FILE_BEGIN, OP_FILE_DATA, OP_FILE_DATA, OP_FILE_DATA, OP_FILE_END,
        OP_FILE_BEGIN, OP_FILE_DATA, OP_FILE_END,
        OP_TRANSFER_END
    };
    const size_t n_expected = sizeof(expected_order) / sizeof(expected_order[0]);

    bool order_ok = true;
    for (size_t i = 0; i < n_expected; i++) {
        if (recv_frame(sv[1], &hdr, payload, &rx) != FRAME_OK) {
            order_ok = false;
            break;
        }
        if (hdr.type != expected_order[i]) {
            order_ok = false;
            break;
        }
        if (hdr.type == OP_FILE_DATA) {
            MsgFileData m;
            if (parse_file_data(payload.data(), hdr.payload_length, &m)) {
                received_useful += m.data.size();
                data_frames++;
            }
        }
    }
    check(order_ok, "all 11 frames arrived in the expected order");
    check(data_frames == 4, "four FILE_DATA frames seen");
    check_u64(received_useful, useful, "B_useful matches on both ends");
    check_u64(tx, rx, "B_total matches on both ends");

    /* The sender is now done; a clean close must read as EOF, not an error. */
    close(sv[0]);
    check(recv_frame(sv[1], &hdr, payload, &rx) == FRAME_EOF,
          "after TRANSFER_END, a clean close is FRAME_EOF");
    close(sv[1]);

    /* Overhead sanity: 11 frames * 8 header bytes + 4 * 16 prefix + control
     * payloads, all on top of 4 * 4096 useful bytes. Must be a small, positive
     * percentage - if this ever goes negative or huge, the counter is wrong. */
    check(tx > useful, "B_total exceeds B_useful");
    double overhead = (static_cast<double>(tx - useful)
                       / static_cast<double>(useful)) * 100.0;
    check(overhead > 0.0 && overhead < 5.0,
          "overhead is a small positive percentage");
    printf("  [info] B_useful = %llu, B_total = %llu, overhead = %.3f%%\n",
           static_cast<unsigned long long>(useful),
           static_cast<unsigned long long>(tx), overhead);
}

int main(void)
{
    printf("=========================================\n");
    printf(" IT305 protocol.cpp unit tests (Member 2)\n");
    printf("=========================================\n");

    test_all_opcodes();
    test_header_bytes_on_wire();
    test_zerocopy_headers();
    test_bad_frames();
    test_eof_and_drop();
    test_parser_guards();
    test_session_id_helpers();
    test_full_sequence();

    printf("\n=========================================\n");
    printf(" PASSED: %d   FAILED: %d\n", g_pass, g_fail);
    printf("=========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
