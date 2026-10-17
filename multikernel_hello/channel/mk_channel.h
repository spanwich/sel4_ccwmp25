/*
 * Copyright 2026, PhD Research Project — multikernel-AMP
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#define MK_CHANNEL_MAGIC          0x4d4b4348u /* "MKCH" */
#define MK_CHANNEL_ABI_VERSION    2u
#define MK_CHANNEL_REGION_SIZE    0x00a00000u
#define MK_CHANNEL_SLOT_COUNT     16384u
#define MK_CHANNEL_PAYLOAD_SIZE   512u
#define MK_CHANNEL_SLOT_META_SIZE 64u
#define MK_CHANNEL_STATE_READY    1u

typedef enum mk_channel_status {
    MK_CHANNEL_OK = 0,
    MK_CHANNEL_EMPTY = 1,
    MK_CHANNEL_FULL = 2,
    MK_CHANNEL_TOO_LARGE = 3,
    MK_CHANNEL_PROTOCOL_ERROR = 4,
    MK_CHANNEL_CORRUPT = 5,
    MK_CHANNEL_WAIT_FAILED = 6,
    MK_CHANNEL_INVALID_ARGUMENT = 7,
} mk_channel_status_t;

typedef struct __attribute__((aligned(64))) mk_channel_control {
    uint32_t magic;
    uint16_t abi_version;
    uint8_t owner_id;
    uint8_t state;
    uint32_t slot_count;
    uint32_t payload_size;
    uint64_t epoch;
    uint64_t tx_head;
    uint64_t rx_tail;
    uint64_t sent;
    uint64_t received;
    uint64_t full;
    uint64_t corrupt;
    uint8_t reserved[256 - 80];
} mk_channel_control_t;

typedef struct __attribute__((aligned(64))) mk_channel_slot {
    uint64_t sequence;
    uint32_t length;
    uint16_t type;
    uint16_t flags;
    uint8_t reserved[MK_CHANNEL_SLOT_META_SIZE - 16];
    uint8_t payload[MK_CHANNEL_PAYLOAD_SIZE];
} mk_channel_slot_t;

typedef struct __attribute__((aligned(64))) mk_channel_region {
    mk_channel_control_t control;
    mk_channel_slot_t slots[MK_CHANNEL_SLOT_COUNT];
    uint8_t reserved[MK_CHANNEL_REGION_SIZE - sizeof(mk_channel_control_t) -
                     sizeof(mk_channel_slot_t) * MK_CHANNEL_SLOT_COUNT];
} mk_channel_region_t;

typedef struct mk_channel_message {
    uint64_t sequence;
    uint32_t length;
    uint16_t type;
    uint16_t flags;
    uint8_t payload[MK_CHANNEL_PAYLOAD_SIZE];
} mk_channel_message_t;

typedef void (*mk_channel_notify_fn)(void *cookie);
typedef int (*mk_channel_wait_fn)(void *cookie);

typedef struct mk_channel {
    mk_channel_region_t *local;
    const mk_channel_region_t *peer;
    uint8_t local_id;
    mk_channel_notify_fn notify;
    mk_channel_wait_fn wait;
    void *callback_cookie;
} mk_channel_t;

_Static_assert(sizeof(mk_channel_control_t) == 256,
               "channel control must occupy four cache lines");
_Static_assert(sizeof(mk_channel_slot_t) == 576,
               "channel slot must contain a cache-line header and 512-byte payload");
_Static_assert((MK_CHANNEL_SLOT_COUNT & (MK_CHANNEL_SLOT_COUNT - 1u)) == 0,
               "channel slot count must be a power of two");
_Static_assert(sizeof(mk_channel_region_t) == MK_CHANNEL_REGION_SIZE,
               "channel region ABI must be exactly 10 MiB");

void mk_channel_region_prepare(mk_channel_region_t *region, uint8_t owner_id,
                               uint64_t epoch);
void mk_channel_region_mark_ready(mk_channel_region_t *region);
int mk_channel_region_is_ready(const mk_channel_region_t *region,
                               uint8_t expected_owner);
mk_channel_status_t mk_channel_attach(mk_channel_t *channel, uint8_t local_id,
                                      mk_channel_region_t *local,
                                      const mk_channel_region_t *peer,
                                      mk_channel_notify_fn notify,
                                      mk_channel_wait_fn wait,
                                      void *callback_cookie);
mk_channel_status_t mk_channel_try_send(mk_channel_t *channel, uint16_t type,
                                        uint16_t flags, const void *payload,
                                        uint32_t length, uint64_t *sequence);
mk_channel_status_t mk_channel_try_recv(mk_channel_t *channel,
                                        mk_channel_message_t *message);
mk_channel_status_t mk_channel_recv(mk_channel_t *channel,
                                    mk_channel_message_t *message);

