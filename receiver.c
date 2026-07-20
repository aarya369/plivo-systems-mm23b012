/* BASELINE RECEIVER (C) — naive on purpose. Rewrite it (C, C++, Go, or Rust).
 *
 * Ports (all 127.0.0.1):
 *   bind 47002  <- media from your sender, via the hostile relay
 *   send 47020  -> harness player. MUST be: 4-byte big-endian seq +
 *                  160-byte payload. Frame i counts only if it arrives
 *                  BEFORE its deadline t0 + DELAY_MS + i*20ms.
 *   send 47003  -> feedback to your sender, via the relay (optional)
 *
 * This baseline forwards whatever arrives straight to the player: lost
 * frames stay lost, late frames stay late, duplicates are re-sent
 * harmlessly. All yours to fix — jitter buffer, reordering, recovery.
 *
 * Env vars available: T0, DURATION_S, DELAY_MS. Harness kills the process
 * at run end; a forever-loop is fine.
 */
#include <arpa/inet.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

enum {
    HARNESS_SEQ_BYTES = 4,
    PAYLOAD_BYTES = 160,
    HARNESS_PACKET_BYTES = HARNESS_SEQ_BYTES + PAYLOAD_BYTES,

    WIRE_BLOCK_START_OFFSET = 0,
    WIRE_SHARD_INDEX_OFFSET = 4,
    WIRE_DATA_SHARDS_OFFSET = 5,
    WIRE_PARITY_SHARDS_OFFSET = 6,
    WIRE_FLAGS_OFFSET = 7,
    WIRE_HEADER_BYTES = 8,
    WIRE_PAYLOAD_OFFSET = WIRE_HEADER_BYTES,
    WIRE_PACKET_BYTES = WIRE_HEADER_BYTES + PAYLOAD_BYTES,

    DATA_SHARDS_PER_BLOCK = 4,
    PARITY_SHARDS_PER_BLOCK = 3,
    WIRE_FLAGS_MEDIA_V1 = 1,

    FRAME_INTERVAL_MS = 20,
    /* Fixed window: 512 frames is about 10.24 seconds of media. */
    PLAYOUT_BUFFER_SLOTS = 512,
    FEC_BLOCK_SLOTS = PLAYOUT_BUFFER_SLOTS / DATA_SHARDS_PER_BLOCK
};

/*
 * The player timestamps a UDP datagram after it is delivered.  Leave a fixed
 * local handoff margin so a receiver send is not judged late at the deadline.
 */
static const double PLAYOUT_GUARD_SECONDS = 0.010;

struct playout_slot {
    uint32_t seq;
    int present;
    unsigned char payload[PAYLOAD_BYTES];
};

/* One in-flight 4-data/3-parity systematic FEC block. */
struct fec_block {
    uint32_t block_start_seq;
    int valid;
    int decoded;
    uint8_t received[DATA_SHARDS_PER_BLOCK + PARITY_SHARDS_PER_BLOCK];
    unsigned char shards[DATA_SHARDS_PER_BLOCK + PARITY_SHARDS_PER_BLOCK]
                        [PAYLOAD_BYTES];
};

/*
 * Host-order representation of the sender-to-receiver packet.  It is parsed
 * explicitly rather than cast from received bytes, avoiding alignment and
 * padding assumptions.
 */
struct wire_packet {
    uint32_t block_start_seq; /* First harness frame sequence in this block. */
    uint8_t shard_index;      /* Data index 0..3, or parity index 4..6. */
    uint8_t data_shards;      /* Source-data shard count. */
    uint8_t parity_shards;    /* Parity shard count. */
    uint8_t flags;            /* Packet kind/protocol version. */
    unsigned char payload[PAYLOAD_BYTES]; /* Original payload or FEC shard. */
};

static void deserialize_wire_packet(struct wire_packet *packet,
                                    const unsigned char in[WIRE_PACKET_BYTES]) {
    uint32_t network_block_start;

    memcpy(&network_block_start, in + WIRE_BLOCK_START_OFFSET,
           sizeof network_block_start);
    packet->block_start_seq = ntohl(network_block_start);
    packet->shard_index = in[WIRE_SHARD_INDEX_OFFSET];
    packet->data_shards = in[WIRE_DATA_SHARDS_OFFSET];
    packet->parity_shards = in[WIRE_PARITY_SHARDS_OFFSET];
    packet->flags = in[WIRE_FLAGS_OFFSET];
    memcpy(packet->payload, in + WIRE_PAYLOAD_OFFSET, PAYLOAD_BYTES);
}

/* GF(2^8) arithmetic for the small systematic Reed-Solomon-style code. */
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

static uint8_t coding_coefficient(unsigned int shard_index,
                                  unsigned int data_index) {
    if (shard_index < DATA_SHARDS_PER_BLOCK)
        return shard_index == data_index ? 1 : 0;
    return gf_pow((uint8_t)(data_index + 1),
                  shard_index - DATA_SHARDS_PER_BLOCK);
}

/* Invert a 4x4 matrix over GF(2^8) with Gauss-Jordan elimination. */
static int invert_matrix(uint8_t input[DATA_SHARDS_PER_BLOCK]
                                       [DATA_SHARDS_PER_BLOCK],
                         uint8_t output[DATA_SHARDS_PER_BLOCK]
                                        [DATA_SHARDS_PER_BLOCK]) {
    uint8_t augmented[DATA_SHARDS_PER_BLOCK]
                     [DATA_SHARDS_PER_BLOCK * 2];
    for (unsigned int row = 0; row < DATA_SHARDS_PER_BLOCK; row++) {
        for (unsigned int col = 0; col < DATA_SHARDS_PER_BLOCK; col++) {
            augmented[row][col] = input[row][col];
            augmented[row][DATA_SHARDS_PER_BLOCK + col] = row == col ? 1 : 0;
        }
    }

    for (unsigned int col = 0; col < DATA_SHARDS_PER_BLOCK; col++) {
        unsigned int pivot = col;
        while (pivot < DATA_SHARDS_PER_BLOCK && augmented[pivot][col] == 0)
            pivot++;
        if (pivot == DATA_SHARDS_PER_BLOCK) return 0;
        if (pivot != col) {
            for (unsigned int j = 0; j < DATA_SHARDS_PER_BLOCK * 2; j++) {
                uint8_t saved = augmented[col][j];
                augmented[col][j] = augmented[pivot][j];
                augmented[pivot][j] = saved;
            }
        }

        uint8_t inverse = gf_pow(augmented[col][col], 254);
        for (unsigned int j = 0; j < DATA_SHARDS_PER_BLOCK * 2; j++)
            augmented[col][j] = gf_mul(augmented[col][j], inverse);
        for (unsigned int row = 0; row < DATA_SHARDS_PER_BLOCK; row++) {
            if (row == col) continue;
            uint8_t factor = augmented[row][col];
            if (factor == 0) continue;
            for (unsigned int j = 0; j < DATA_SHARDS_PER_BLOCK * 2; j++)
                augmented[row][j] ^= gf_mul(factor, augmented[col][j]);
        }
    }

    for (unsigned int row = 0; row < DATA_SHARDS_PER_BLOCK; row++)
        for (unsigned int col = 0; col < DATA_SHARDS_PER_BLOCK; col++)
            output[row][col] = augmented[row][DATA_SHARDS_PER_BLOCK + col];
    return 1;
}

/* Insert one original frame, retaining the first arrival for duplicate safety. */
static void insert_playout_frame(
    struct playout_slot slots[PLAYOUT_BUFFER_SLOTS], uint32_t next_play_seq,
    uint32_t frame_seq, const unsigned char payload[PAYLOAD_BYTES]) {
    uint32_t distance = frame_seq - next_play_seq;
    if (distance >= PLAYOUT_BUFFER_SLOTS) return;

    size_t slot_index = frame_seq % PLAYOUT_BUFFER_SLOTS;
    struct playout_slot *slot = &slots[slot_index];
    if (slot->present) return; /* Matching seq is duplicate; any other is stale. */
    slot->seq = frame_seq;
    memcpy(slot->payload, payload, PAYLOAD_BYTES);
    slot->present = 1;
}

/* Recover all four original payloads after any four distinct shards arrive. */
static void try_decode_block(struct fec_block *block,
                             struct playout_slot slots[PLAYOUT_BUFFER_SLOTS],
                             uint32_t next_play_seq) {
    if (block->decoded) return;
    unsigned int selected[DATA_SHARDS_PER_BLOCK];
    unsigned int count = 0;
    for (unsigned int shard = 0;
         shard < DATA_SHARDS_PER_BLOCK + PARITY_SHARDS_PER_BLOCK; shard++) {
        if (block->received[shard]) selected[count++] = shard;
        if (count == DATA_SHARDS_PER_BLOCK) break;
    }
    if (count != DATA_SHARDS_PER_BLOCK) return;

    uint8_t matrix[DATA_SHARDS_PER_BLOCK][DATA_SHARDS_PER_BLOCK];
    uint8_t inverse[DATA_SHARDS_PER_BLOCK][DATA_SHARDS_PER_BLOCK];
    for (unsigned int row = 0; row < DATA_SHARDS_PER_BLOCK; row++)
        for (unsigned int col = 0; col < DATA_SHARDS_PER_BLOCK; col++)
            matrix[row][col] = coding_coefficient(selected[row], col);
    if (!invert_matrix(matrix, inverse)) return;

    for (unsigned int data_index = 0; data_index < DATA_SHARDS_PER_BLOCK;
         data_index++) {
        unsigned char recovered[PAYLOAD_BYTES] = {0};
        for (unsigned int row = 0; row < DATA_SHARDS_PER_BLOCK; row++) {
            uint8_t coefficient = inverse[data_index][row];
            for (unsigned int byte = 0; byte < PAYLOAD_BYTES; byte++)
                recovered[byte] ^= gf_mul(coefficient,
                                          block->shards[selected[row]][byte]);
        }
        insert_playout_frame(slots, next_play_seq,
                             block->block_start_seq + data_index, recovered);
    }
    block->decoded = 1;
}

static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
}

static double playout_time(double t0, double delay_seconds, uint32_t seq) {
    return t0 + delay_seconds +
           (double)seq * FRAME_INTERVAL_MS / 1000.0 - PLAYOUT_GUARD_SECONDS;
}

/*
 * Deliver every frame whose playout time has passed.  A missing slot is
 * counted and skipped, so one lost frame can never stall later playback.
 */
static void play_due_frames(int out_fd, const struct sockaddr_in *player,
                            struct playout_slot slots[PLAYOUT_BUFFER_SLOTS],
                            uint32_t *next_play_seq, uint64_t *missed_frames,
                            double t0, double delay_seconds) {
    while (now_seconds() >= playout_time(t0, delay_seconds, *next_play_seq)) {
        size_t slot_index = *next_play_seq % PLAYOUT_BUFFER_SLOTS;
        struct playout_slot *slot = &slots[slot_index];

        if (slot->present && slot->seq == *next_play_seq) {
            unsigned char harness_packet[HARNESS_PACKET_BYTES];
            uint32_t network_seq = htonl(*next_play_seq);
            memcpy(harness_packet, &network_seq, sizeof network_seq);
            memcpy(harness_packet + HARNESS_SEQ_BYTES, slot->payload,
                   PAYLOAD_BYTES);
            sendto(out_fd, harness_packet, sizeof harness_packet, 0,
                   (const struct sockaddr *)player, sizeof *player);
            slot->present = 0;
        } else {
            /* The deadline passed without this sequence: record and skip it. */
            (*missed_frames)++;
        }

        /* Always advance, preserving strictly increasing player output. */
        (*next_play_seq)++;
    }
}

int main(void) {
    int in_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in in_addr = {0};
    in_addr.sin_family = AF_INET;
    in_addr.sin_port = htons(47002);
    in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (bind(in_fd, (struct sockaddr *)&in_addr, sizeof in_addr) < 0) {
        perror("bind 47002");
        return 1;
    }

    int out_fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in player = {0};
    player.sin_family = AF_INET;
    player.sin_port = htons(47020);
    player.sin_addr.s_addr = inet_addr("127.0.0.1");

    const char *t0_env = getenv("T0");
    const char *delay_env = getenv("DELAY_MS");
    char *parse_end;
    if (t0_env == NULL || delay_env == NULL) {
        fputs("T0 and DELAY_MS must be set\n", stderr);
        return 1;
    }
    double t0 = strtod(t0_env, &parse_end);
    if (*parse_end != '\0') {
        fputs("invalid T0\n", stderr);
        return 1;
    }
    double delay_seconds = strtod(delay_env, &parse_end) / 1000.0;
    if (*parse_end != '\0' || delay_seconds < 0.0) {
        fputs("invalid DELAY_MS\n", stderr);
        return 1;
    }

    unsigned char wire_bytes[WIRE_PACKET_BYTES];
    struct playout_slot slots[PLAYOUT_BUFFER_SLOTS] = {0};
    struct fec_block fec_blocks[FEC_BLOCK_SLOTS] = {0};
    uint32_t next_play_seq = 0;
    uint64_t missed_frames = 0;
    for (;;) {
        /* First advance past any expired deadlines before accepting new data. */
        play_due_frames(out_fd, &player, slots, &next_play_seq, &missed_frames,
                        t0, delay_seconds);

        double wait_seconds =
            playout_time(t0, delay_seconds, next_play_seq) - now_seconds();
        if (wait_seconds < 0.0) wait_seconds = 0.0;
        struct timeval timeout = {
            .tv_sec = (time_t)wait_seconds,
            .tv_usec = (suseconds_t)((wait_seconds - (double)(time_t)wait_seconds) *
                                     1000000.0)
        };
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(in_fd, &read_fds);
        int ready = select(in_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready <= 0) continue;

        ssize_t n = recvfrom(in_fd, wire_bytes, sizeof wire_bytes, 0,
                             NULL, NULL);
        if (n <= 0 || n != WIRE_PACKET_BYTES) continue;

        struct wire_packet packet;
        deserialize_wire_packet(&packet, wire_bytes);

        /* Validate the fixed initial protocol before using any parsed fields. */
        if (packet.flags != WIRE_FLAGS_MEDIA_V1 ||
            packet.data_shards != DATA_SHARDS_PER_BLOCK ||
            packet.parity_shards != PARITY_SHARDS_PER_BLOCK ||
            packet.shard_index >= packet.data_shards + packet.parity_shards) {
            continue;
        }

        size_t block_index = (packet.block_start_seq / DATA_SHARDS_PER_BLOCK) %
                             FEC_BLOCK_SLOTS;
        struct fec_block *block = &fec_blocks[block_index];
        if (!block->valid || block->block_start_seq != packet.block_start_seq) {
            memset(block, 0, sizeof *block);
            block->valid = 1;
            block->block_start_seq = packet.block_start_seq;
        }

        /* A repeated shard is a duplicate and cannot alter the stored block. */
        if (block->received[packet.shard_index]) continue;
        block->received[packet.shard_index] = 1;
        memcpy(block->shards[packet.shard_index], packet.payload, PAYLOAD_BYTES);

        /* Systematic data is usable immediately; parity is retained for repair. */
        if (packet.shard_index < DATA_SHARDS_PER_BLOCK)
            insert_playout_frame(slots, next_play_seq,
                                 packet.block_start_seq + packet.shard_index,
                                 packet.payload);
        try_decode_block(block, slots, next_play_seq);
    }
    return 0;
}
