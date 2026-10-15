#ifndef PROTOCOL_H
#define PROTOCOL_H

/*
 * protocol.h - Member 2 (branch member2-protocol, PR #1, Wave 1)
 *
 * Wire framing layer for the Fault-Tolerant Topic-Based File Distribution
 * Service (IT305 Lab, Group 103 & 106).
 *
 * This module is the ONLY place in the project that touches raw bytes on the
 * socket. Everyone else (server loops, fault injector, session manager,
 * enhanced parallel client) calls the functions below instead of calling
 * send()/recv() directly.
 *
 * Three layers live here:
 *   Layer 1 : send_all() / recv_all()      - loop until exactly N bytes moved
 *   Layer 2 : send_frame() / recv_frame()  - 8-byte big-endian length-prefixed header
 *   Layer 3 : serialize_* / parse_*        - exact payload layouts for opcodes 0x01-0x0E
 *
 * Wire rules (Design Doc Section 2.2):
 *   - every integer is fixed width and BIG-ENDIAN (network byte order)
 *   - no C structs are ever memcpy'd onto the wire (compiler padding is unsafe)
 *   - every variable-length string is [uint16_t length][raw bytes], NO null terminator
 *   - session_id is always 16 raw bytes on the wire (all zeros in Part I / Case 1)
 *
 * B_total ACCOUNTING
 * ------------------
 * Every function that moves bytes takes an optional trailing
 * `uint64_t* wire_counter`. When non-null, EVERY byte that actually crossed
 * the socket - frame headers included, and including bytes that crossed just
 * before a fault killed the connection - is added to *wire_counter. That
 * running total is B_total, which Member 6's record_transfer_metrics() needs:
 *
 *     Wire Throughput (MB/s) = B_total / (T_complete * 10^6)
 *     Overhead (%)           = ((B_total - B_useful) / B_useful) * 100
 *
 * Pass nullptr (the default) when you do not care.
 */

#include "common_types.h"

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <string>
#include <vector>

/* ------------------------------------------------------------------ */
/* Return codes for recv_frame()                                       */
/* ------------------------------------------------------------------ */
#define FRAME_OK          1    /* a complete, valid frame was received      */
#define FRAME_EOF         0    /* peer closed the socket cleanly (no frame) */
#define FRAME_SOCK_ERR   (-1)  /* socket error / connection reset           */
#define FRAME_PROTO_ERR  (-2)  /* bad magic, bad version, or payload too big*/

/* ------------------------------------------------------------------ */
/* Error codes carried inside ERROR (0x03) and RESTART (0x0B)          */
/* ------------------------------------------------------------------ */
#define ERR_BAD_REQUEST        400
#define ERR_TOPIC_NOT_FOUND    404
#define ERR_IO_ERROR           500
#define ERR_TOPIC_MISMATCH     409   /* also used for FILENAME_MISMATCH    */
#define ERR_FILENAME_MISMATCH  409
#define ERR_SESSION_EXPIRED    410
#define ERR_INVALID_OFFSET     416   /* also used for HIGHWATER_EXCEEDED   */
#define ERR_HIGHWATER_EXCEEDED 416

/* Size of the fixed frame header, in bytes. */
#define FRAME_HEADER_SIZE 8

/* Size of the fixed metadata prefix inside a FILE_DATA (0x05) payload:
 * [file_index:u32][offset:u64][data_len:u32] = 16 bytes, before the raw data. */
#define FILE_DATA_PREFIX_SIZE 16

/* Total bytes Member 5's zero-copy path must push before calling sendfile():
 * the 8-byte frame header plus the 16-byte FILE_DATA metadata prefix. */
#define FILE_DATA_HEADER_SIZE (FRAME_HEADER_SIZE + FILE_DATA_PREFIX_SIZE)  /* 24 */

/* ================================================================== */
/* LAYER 1 - loop-complete socket helpers                              */
/* ================================================================== */

/*
 * Send exactly len bytes. A single send() may move fewer bytes than asked,
 * so we loop. MSG_NOSIGNAL is used so a dead peer gives us EPIPE instead of
 * killing the whole process with SIGPIPE.
 *
 * wire_counter : if non-null, incremented by every byte actually sent,
 *                including the partial bytes sent before a failure.
 *
 * Returns: len on success, -1 on socket error.
 */
ssize_t send_all(int sockfd, const void *buf, size_t len,
                 uint64_t *wire_counter = nullptr);

/*
 * Receive exactly len bytes.
 *
 * wire_counter : if non-null, incremented by every byte actually received,
 *                including bytes received before a mid-read failure.
 *
 * Returns: len on success,
 *          0  if the peer closed the connection cleanly before any byte of
 *             this request arrived (clean EOF),
 *          -1 on socket error (ECONNRESET etc.) or on a short read caused by
 *             the peer dying halfway through.
 */
ssize_t recv_all(int sockfd, void *buf, size_t len,
                 uint64_t *wire_counter = nullptr);

/* ================================================================== */
/* LAYER 2 - 8-byte length-prefixed framing                            */
/* ================================================================== */

/*
 * Frame layout on the wire (big-endian):
 *   byte 0..1 : magic          = 0x4954 ("IT")
 *   byte 2    : version        = 0x01
 *   byte 3    : opcode         = 0x01 .. 0x0E
 *   byte 4..7 : payload_length (uint32_t)
 *   byte 8..  : payload, exactly payload_length bytes
 *
 * payload may be NULL when payload_len == 0.
 * wire_counter, if non-null, accumulates 8 + payload_len on success.
 * Returns 0 on success, -1 on failure.
 */
int send_frame(int sockfd, uint8_t opcode, const uint8_t *payload,
               uint32_t payload_len, uint64_t *wire_counter = nullptr);

/* Convenience wrapper: build the frame straight from a serialized vector. */
int send_frame_vec(int sockfd, uint8_t opcode, const std::vector<uint8_t> &payload,
                   uint64_t *wire_counter = nullptr);

/*
 * Read one full frame. out_payload is resized to the exact payload length.
 * wire_counter, if non-null, accumulates 8 + payload_length.
 * Returns FRAME_OK / FRAME_EOF / FRAME_SOCK_ERR / FRAME_PROTO_ERR.
 */
int recv_frame(int sockfd, FrameHeader *out_hdr, std::vector<uint8_t> &out_payload,
               uint64_t *wire_counter = nullptr);

/* ------------------------------------------------------------------ */
/* Header builders for the zero-copy path (Member 5)                   */
/* ------------------------------------------------------------------ */
/*
 * send_frame() cannot be used by send_chunk_zerocopy(), because there the
 * payload never passes through user space - sendfile() moves it straight from
 * the page cache to the NIC. These builders hand back the exact header bytes
 * so the wire format stays defined in precisely one place.
 */

/* Writes the 8-byte frame header into out. */
void build_frame_header(uint8_t out[FRAME_HEADER_SIZE], uint8_t opcode,
                        uint32_t payload_len);

/* Writes the 16-byte FILE_DATA metadata prefix into out. */
void build_file_data_prefix(uint8_t out[FILE_DATA_PREFIX_SIZE], uint32_t file_index,
                            uint64_t offset, uint32_t data_len);

/*
 * Writes the complete 24-byte preamble for a zero-copy FILE_DATA chunk:
 * the frame header (payload_length = 16 + data_len) followed by the metadata
 * prefix. Typical use:
 *
 *     uint8_t hdr[FILE_DATA_HEADER_SIZE];
 *     build_file_data_header(hdr, unit.file_index, unit.file_offset, unit.chunk_len);
 *     if (send_all(client_fd, hdr, sizeof(hdr), wire_counter) < 0) return -1;
 *     // then sendfile(client_fd, file_fd, &off, unit.chunk_len)
 *     // and add the bytes sendfile() reports to *wire_counter yourself
 *
 * The bytes produced are byte-for-byte identical to what send_frame() would
 * have emitted for the same chunk; the unit test asserts this.
 */
void build_file_data_header(uint8_t out[FILE_DATA_HEADER_SIZE], uint32_t file_index,
                            uint64_t offset, uint32_t data_len);

/* Human readable opcode name, for [INFO]/[FAULT] logging. Never returns NULL. */
const char *opcode_name(uint8_t opcode);

/* ================================================================== */
/* LAYER 3 - message structs (parser output)                           */
/* ================================================================== */

struct MsgGet {                       /* 0x01 */
    std::string topic;
};

struct MsgGetOk {                     /* 0x02 */
    uint8_t  session_id[16];
    uint32_t file_count;
    uint64_t total_bytes;
};

struct MsgStatus {                    /* 0x03 ERROR and 0x0B RESTART */
    uint16_t    code;
    std::string reason;
};

struct MsgFileBegin {                 /* 0x04 */
    uint32_t    file_index;
    uint64_t    byte_offset;
    uint64_t    bytes_following;
    std::string filename;
};

struct MsgFileData {                  /* 0x05 */
    uint32_t             file_index;
    uint64_t             offset;
    std::vector<uint8_t> data;
};

struct MsgFileEnd {                   /* 0x06 */
    uint32_t file_index;
};

struct MsgTransferEnd {               /* 0x07 */
    uint32_t files_sent;
    uint64_t useful_bytes;
};

struct MsgCkptAck {                   /* 0x08 */
    uint8_t  session_id[16];
    uint32_t file_index;
    uint64_t ack_offset;
};

struct MsgResume {                    /* 0x09 */
    uint8_t     session_id[16];
    uint32_t    file_index;
    uint64_t    byte_offset;
    std::string topic;
    std::string filename;
};

struct MsgResumeOk {                  /* 0x0A */
    uint32_t file_index;
    uint64_t accepted_offset;
    uint32_t rem_files;
    uint64_t rem_bytes;
};

struct MsgGetEnhanced {               /* 0x0C */
    uint16_t    num_streams;
    std::string topic;
};

struct MsgJoinSession {               /* 0x0D */
    uint8_t              session_id[16];
    uint32_t             conn_id;
    std::vector<uint8_t> chunk_bitmap;
};

struct MsgAssignWork {                /* 0x0E */
    uint32_t conn_id;
    uint32_t file_index;
    uint64_t chunk_offset;
    uint32_t chunk_len;
};

/* ================================================================== */
/* LAYER 3 - serializers (build payload)                               */
/* ================================================================== */
/* Each one CLEARS out and refills it with the exact bytes of Table 2. */

/* 0x01 : [topic_len:u16][topic] */
void serialize_get(const std::string &topic, std::vector<uint8_t> &out);

/* 0x02 : [session_id:16B][file_count:u32][total_bytes:u64] */
void serialize_get_ok(const uint8_t session_id[16], uint32_t file_count,
                      uint64_t total_bytes, std::vector<uint8_t> &out);

/* 0x03 OP_ERROR and 0x0B OP_RESTART : [code:u16][msg_len:u16][reason]
 * One layout, one function - the opcode passed to send_frame() is what
 * distinguishes a hard error from a resume rejection. */
void serialize_status_msg(uint16_t code, const std::string &reason,
                          std::vector<uint8_t> &out);

/* 0x04 : [file_index:u32][byte_offset:u64][bytes_following:u64][name_len:u16][filename] */
void serialize_file_begin(uint32_t file_index, uint64_t byte_offset,
                          uint64_t bytes_following, const std::string &filename,
                          std::vector<uint8_t> &out);

/* 0x05 : [file_index:u32][offset:u64][data_len:u32][data] */
void serialize_file_data(uint32_t file_index, uint64_t offset,
                         const uint8_t *data, uint32_t data_len,
                         std::vector<uint8_t> &out);

/* 0x06 : [file_index:u32] */
void serialize_file_end(uint32_t file_index, std::vector<uint8_t> &out);

/* 0x07 : [files_sent:u32][useful_bytes:u64] */
void serialize_transfer_end(uint32_t files_sent, uint64_t useful_bytes,
                            std::vector<uint8_t> &out);

/* 0x08 : [session_id:16B][file_index:u32][ack_offset:u64] */
void serialize_ckpt_ack(const uint8_t session_id[16], uint32_t file_index,
                        uint64_t ack_offset, std::vector<uint8_t> &out);

/* 0x09 : [session_id:16B][file_index:u32][byte_offset:u64]
 *        [topic_len:u16][topic][name_len:u16][filename] */
void serialize_resume(const uint8_t session_id[16], uint32_t file_index,
                      uint64_t byte_offset, const std::string &topic,
                      const std::string &filename, std::vector<uint8_t> &out);

/* 0x0A : [file_index:u32][accepted_offset:u64][rem_files:u32][rem_bytes:u64] */
void serialize_resume_ok(uint32_t file_index, uint64_t accepted_offset,
                         uint32_t rem_files, uint64_t rem_bytes,
                         std::vector<uint8_t> &out);

/* 0x0C : [num_streams:u16][topic_len:u16][topic] */
void serialize_get_enhanced(uint16_t num_streams, const std::string &topic,
                            std::vector<uint8_t> &out);

/* 0x0D : [session_id:16B][conn_id:u32][bmap_len:u32][chunk_bitmap] */
void serialize_join_session(const uint8_t session_id[16], uint32_t conn_id,
                            const uint8_t *chunk_bitmap, uint32_t bmap_len,
                            std::vector<uint8_t> &out);

/* 0x0E : [conn_id:u32][file_index:u32][chunk_offset:u64][chunk_len:u32] */
void serialize_assign_work(uint32_t conn_id, uint32_t file_index,
                           uint64_t chunk_offset, uint32_t chunk_len,
                           std::vector<uint8_t> &out);

/* ================================================================== */
/* LAYER 3 - parsers (read payload)                                    */
/* ================================================================== */
/*
 * Every parser returns true only if the payload decoded cleanly AND every
 * single byte was consumed. A malformed or truncated payload returns false,
 * and the caller must treat that as a PROTOCOL_ERROR.
 */

bool parse_get(const uint8_t *payload, uint32_t len, MsgGet *out);
bool parse_get_ok(const uint8_t *payload, uint32_t len, MsgGetOk *out);
bool parse_status_msg(const uint8_t *payload, uint32_t len, MsgStatus *out);
bool parse_file_begin(const uint8_t *payload, uint32_t len, MsgFileBegin *out);
bool parse_file_data(const uint8_t *payload, uint32_t len, MsgFileData *out);
bool parse_file_end(const uint8_t *payload, uint32_t len, MsgFileEnd *out);
bool parse_transfer_end(const uint8_t *payload, uint32_t len, MsgTransferEnd *out);
bool parse_ckpt_ack(const uint8_t *payload, uint32_t len, MsgCkptAck *out);
bool parse_resume(const uint8_t *payload, uint32_t len, MsgResume *out);
bool parse_resume_ok(const uint8_t *payload, uint32_t len, MsgResumeOk *out);
bool parse_get_enhanced(const uint8_t *payload, uint32_t len, MsgGetEnhanced *out);
bool parse_join_session(const uint8_t *payload, uint32_t len, MsgJoinSession *out);
bool parse_assign_work(const uint8_t *payload, uint32_t len, MsgAssignWork *out);

/* ================================================================== */
/* SEND HELPERS - serialize + send_frame in one call                   */
/* ================================================================== */
/* These are what the rest of the team should normally call.           */
/* All of them return 0 on success and -1 on socket failure, and all   */
/* accept an optional wire_counter for B_total accounting.             */

int send_get(int fd, const std::string &topic,
             uint64_t *wire_counter = nullptr);

int send_get_ok(int fd, const uint8_t session_id[16], uint32_t file_count,
                uint64_t total_bytes, uint64_t *wire_counter = nullptr);

/* 0x03 - a request that can never succeed. */
int send_error(int fd, uint16_t code, const std::string &reason,
               uint64_t *wire_counter = nullptr);

/* 0x0B - a RESUME that was rejected; client must fall back to a fresh GET. */
int send_restart(int fd, uint16_t code, const std::string &reason,
                 uint64_t *wire_counter = nullptr);

int send_file_begin(int fd, uint32_t file_index, uint64_t byte_offset,
                    uint64_t bytes_following, const std::string &filename,
                    uint64_t *wire_counter = nullptr);

int send_file_data(int fd, uint32_t file_index, uint64_t offset,
                   const uint8_t *data, uint32_t data_len,
                   uint64_t *wire_counter = nullptr);

int send_file_end(int fd, uint32_t file_index,
                  uint64_t *wire_counter = nullptr);

int send_transfer_end(int fd, uint32_t files_sent, uint64_t useful_bytes,
                      uint64_t *wire_counter = nullptr);

int send_ckpt_ack(int fd, const uint8_t session_id[16], uint32_t file_index,
                  uint64_t ack_offset, uint64_t *wire_counter = nullptr);

int send_resume(int fd, const uint8_t session_id[16], uint32_t file_index,
                uint64_t byte_offset, const std::string &topic,
                const std::string &filename, uint64_t *wire_counter = nullptr);

int send_resume_ok(int fd, uint32_t file_index, uint64_t accepted_offset,
                   uint32_t rem_files, uint64_t rem_bytes,
                   uint64_t *wire_counter = nullptr);

int send_get_enhanced(int fd, uint16_t num_streams, const std::string &topic,
                      uint64_t *wire_counter = nullptr);

int send_join_session(int fd, const uint8_t session_id[16], uint32_t conn_id,
                      const uint8_t *chunk_bitmap, uint32_t bmap_len,
                      uint64_t *wire_counter = nullptr);

int send_assign_work(int fd, uint32_t conn_id, uint32_t file_index,
                     uint64_t chunk_offset, uint32_t chunk_len,
                     uint64_t *wire_counter = nullptr);

/* ------------------------------------------------------------------ */
/* Session id helpers                                                  */
/* ------------------------------------------------------------------ */

/* Part I and Case 1 send an all-zero session id. */
void zero_session_id(uint8_t session_id[16]);

/* Render a 16-byte session id as a 32-char lowercase hex string.
 * out_hex must have room for 33 bytes (32 chars + '\0'). Used only for
 * checkpoint files and log lines, never on the wire. */
void session_id_to_hex(const uint8_t session_id[16], char out_hex[33]);

/* Inverse of session_id_to_hex(). Returns false if hex is malformed. */
bool hex_to_session_id(const char *hex, uint8_t out_session_id[16]);

#endif /* PROTOCOL_H */
