/* BASELINE SENDER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47010  <- harness source delivers frame i here at t0 + i*20ms
 *                  (format: 4-byte big-endian seq + 160-byte payload)
 *   send 47001  -> relay uplink toward the receiver (YOUR wire format)
 *   bind 47004  <- feedback from your receiver, via the relay (optional)
 *
 * This baseline forwards each frame once, unchanged, and ignores feedback.
 * No redundancy, no retransmission. It cannot pass. That is the point.
 *
 * Env vars available if you want them: T0 (epoch seconds, float),
 * DURATION_S, DELAY_MS. The harness kills this process when the run ends,
 * so a forever-loop is fine.
 *
 * build: make        run: python3 run.py --delay_ms 60
 */
#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* Harness-facing packets are: 4-byte sequence number + 160-byte payload. */
enum {
    HARNESS_SEQ_BYTES = 4,
    PAYLOAD_BYTES = 160,
    HARNESS_PACKET_BYTES = HARNESS_SEQ_BYTES + PAYLOAD_BYTES,

    /* Sender-to-receiver wire header: 4 + 1 + 1 + 1 + 1 bytes. */
    WIRE_BLOCK_START_OFFSET = 0,
    WIRE_SHARD_INDEX_OFFSET = 4,
    WIRE_DATA_SHARDS_OFFSET = 5,
    WIRE_PARITY_SHARDS_OFFSET = 6,
    WIRE_FLAGS_OFFSET = 7,
    WIRE_HEADER_BYTES = 8,
    WIRE_PAYLOAD_OFFSET = WIRE_HEADER_BYTES,
    WIRE_PACKET_BYTES = WIRE_HEADER_BYTES + PAYLOAD_BYTES,

    /* Four source shards plus three parity shards per systematic FEC block. */
    DATA_SHARDS_PER_BLOCK = 4,
    PARITY_SHARDS_PER_BLOCK = 3,
    WIRE_FLAGS_MEDIA_V1 = 1
};

/*
 * Host-order representation of the 168-byte wire packet.
 * It is serialized field-by-field below; this C struct is never sent directly,
 * so compiler padding cannot affect the on-wire format.
 */
struct wire_packet {
    uint32_t block_start_seq; /* First harness frame sequence in this block. */
    uint8_t shard_index;      /* 0..5 for a source-data shard; 6..8 for parity. */
    uint8_t data_shards;      /* Number of source-data shards in this block. */
    uint8_t parity_shards;    /* Number of parity shards in this block. */
    uint8_t flags;            /* Packet kind/protocol version: MEDIA_V1 for now. */
    unsigned char payload[PAYLOAD_BYTES]; /* One original payload or FEC shard. */
};

static void serialize_wire_packet(unsigned char out[WIRE_PACKET_BYTES],
                                  const struct wire_packet *packet) {
    uint32_t network_block_start = htonl(packet->block_start_seq);

    memcpy(out + WIRE_BLOCK_START_OFFSET, &network_block_start,
           sizeof network_block_start);
    out[WIRE_SHARD_INDEX_OFFSET] = packet->shard_index;
    out[WIRE_DATA_SHARDS_OFFSET] = packet->data_shards;
    out[WIRE_PARITY_SHARDS_OFFSET] = packet->parity_shards;
    out[WIRE_FLAGS_OFFSET] = packet->flags;
    memcpy(out + WIRE_PAYLOAD_OFFSET, packet->payload, PAYLOAD_BYTES);
}

/* GF(2^8) multiplication using the x^8+x^4+x^3+x^2+1 field. */
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    while (b != 0) {
        if (b & 1) result ^= a;
        a = (a & 0x80) ? (uint8_t)((a << 1) ^ 0x1d) : (uint8_t)(a << 1);
        b >>= 1;
    }
    return result;
}

static uint8_t gf_pow(uint8_t value, unsigned int power) {
    uint8_t result = 1;
    while (power != 0) {
        if (power & 1) result = gf_mul(result, value);
        value = gf_mul(value, value);
        power >>= 1;
    }
    return result;
}

/* Vandermonde parity coefficients: parity 0 is XOR; later rows are independent. */
static uint8_t parity_coefficient(unsigned int parity_index,
                                  unsigned int data_index) {
    return gf_pow((uint8_t)(data_index + 1), parity_index);
}

static void send_shard(int out_fd, const struct sockaddr_in *relay,
                       uint32_t block_start_seq, uint8_t shard_index,
                       const unsigned char payload[PAYLOAD_BYTES],
                       unsigned char wire_bytes[WIRE_PACKET_BYTES]) {
    struct wire_packet packet = {
        .block_start_seq = block_start_seq,
        .shard_index = shard_index,
        .data_shards = DATA_SHARDS_PER_BLOCK,
        .parity_shards = PARITY_SHARDS_PER_BLOCK,
        .flags = WIRE_FLAGS_MEDIA_V1,
    };
    memcpy(packet.payload, payload, PAYLOAD_BYTES);
    serialize_wire_packet(wire_bytes, &packet);
    sendto(out_fd, wire_bytes, WIRE_PACKET_BYTES, 0,
           (const struct sockaddr *)relay, sizeof *relay);
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47010);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47010");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in relay = {0};
    relay.sin_family = AF_INET;
    relay.sin_port = htons(47001);
    relay.sin_addr.s_addr = inet_addr("127.0.0.1");

    unsigned char harness_packet[HARNESS_PACKET_BYTES];
    unsigned char wire_bytes[WIRE_PACKET_BYTES];
    unsigned char data_shards[DATA_SHARDS_PER_BLOCK][PAYLOAD_BYTES];
    for (;;) {
        ssize_t n = recvfrom(in_fd, harness_packet, sizeof harness_packet, 0,
                             NULL, NULL);
        if (n <= 0) continue;
        if (n != HARNESS_PACKET_BYTES) continue;

        uint32_t network_seq;
        memcpy(&network_seq, harness_packet, sizeof network_seq);
        uint32_t frame_seq = ntohl(network_seq);

        uint32_t block_start_seq = frame_seq -
                                   (frame_seq % DATA_SHARDS_PER_BLOCK);
        uint8_t shard_index = (uint8_t)(frame_seq % DATA_SHARDS_PER_BLOCK);
        memcpy(data_shards[shard_index], harness_packet + HARNESS_SEQ_BYTES,
               PAYLOAD_BYTES);

        /* Systematic data is sent immediately, retaining baseline latency. */
        send_shard(out_fd, &relay, block_start_seq, shard_index,
                   data_shards[shard_index], wire_bytes);

        if (shard_index == DATA_SHARDS_PER_BLOCK - 1) {
            for (unsigned int parity_index = 0;
                 parity_index < PARITY_SHARDS_PER_BLOCK; parity_index++) {
                unsigned char parity[PAYLOAD_BYTES] = {0};
                for (unsigned int data_index = 0;
                     data_index < DATA_SHARDS_PER_BLOCK; data_index++) {
                    uint8_t coefficient =
                        parity_coefficient(parity_index, data_index);
                    for (unsigned int byte = 0; byte < PAYLOAD_BYTES; byte++) {
                        parity[byte] ^= gf_mul(coefficient,
                                               data_shards[data_index][byte]);
                    }
                }
                send_shard(out_fd, &relay, block_start_seq,
                           (uint8_t)(DATA_SHARDS_PER_BLOCK + parity_index),
                           parity, wire_bytes);
            }
        }
    }
    return 0;
}
