/*
 * Copyright 2026, PhD Research Project — multikernel-AMP
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Ported from projects/multikernel_qemu (feature/qemu-aarch64-ikc, ABI v2).
 *
 * The CCMP25 kernels are unicore, so seL4 maps cacheable user pages
 * non-shareable and nothing keeps the two cores' caches coherent. The shared
 * regions are therefore mapped non-cacheable (Device memory), which changes
 * how they may be accessed:
 *   - only naturally aligned 64-bit loads/stores (no memcpy/memset, which may
 *     issue unaligned or vector accesses that fault on Device memory);
 *   - no exclusives or atomic read-modify-writes (counters are owner-written,
 *     so a plain load+store is enough);
 *   - ordering via LDAR/STLR (__ATOMIC_ACQUIRE/RELEASE on plain loads/stores).
 * Each region is written only by its owner; the peer maps it read-only.
 * Every field read from the peer is read once into private memory before use.
 */

#include "mk_channel.h"

#include <string.h>

_Static_assert(MK_CHANNEL_PAYLOAD_SIZE % sizeof(uint64_t) == 0,
               "payload must be a whole number of words");

static uint64_t load_acquire_u64(const uint64_t *value)
{
    return __atomic_load_n(value, __ATOMIC_ACQUIRE);
}

static void store_release_u64(uint64_t *value, uint64_t next)
{
    __atomic_store_n(value, next, __ATOMIC_RELEASE);
}

static void bump_u64(uint64_t *counter)
{
    volatile uint64_t *c = counter;
    *c = *c + 1u;
}

static void shm_zero(void *dst, size_t bytes)
{
    volatile uint64_t *d = dst;
    for (size_t i = 0; i < bytes / sizeof(uint64_t); i++) {
        d[i] = 0;
    }
}

/* Word copies; both sides must be 8-byte aligned, bytes a multiple of 8. */
static void shm_copy(void *dst, const void *src, size_t bytes)
{
    volatile uint64_t *d = dst;
    const volatile uint64_t *s = src;
    for (size_t i = 0; i < bytes / sizeof(uint64_t); i++) {
        d[i] = s[i];
    }
}

void mk_channel_region_prepare(mk_channel_region_t *region, uint8_t owner_id,
                               uint64_t epoch)
{
    if (region == NULL) {
        return;
    }

    /* Clear only what the protocol reads: control block and slot headers.
     * Payload bytes beyond a slot's length are never consumed. */
    shm_zero(&region->control, sizeof(region->control));
    for (uint32_t i = 0; i < MK_CHANNEL_SLOT_COUNT; i++) {
        shm_zero(&region->slots[i], MK_CHANNEL_SLOT_META_SIZE);
    }

    volatile mk_channel_control_t *c = &region->control;
    c->magic = MK_CHANNEL_MAGIC;
    c->abi_version = MK_CHANNEL_ABI_VERSION;
    c->owner_id = owner_id;
    c->slot_count = MK_CHANNEL_SLOT_COUNT;
    c->payload_size = MK_CHANNEL_PAYLOAD_SIZE;
    c->epoch = epoch;
}

void mk_channel_region_mark_ready(mk_channel_region_t *region)
{
    if (region != NULL) {
        __atomic_store_n(&region->control.state, MK_CHANNEL_STATE_READY,
                         __ATOMIC_RELEASE);
    }
}

int mk_channel_region_is_ready(const mk_channel_region_t *region,
                               uint8_t expected_owner)
{
    if (region == NULL ||
        __atomic_load_n(&region->control.state, __ATOMIC_ACQUIRE) !=
        MK_CHANNEL_STATE_READY) {
        return 0;
    }

    const volatile mk_channel_control_t *c = &region->control;
    return c->magic == MK_CHANNEL_MAGIC &&
           c->abi_version == MK_CHANNEL_ABI_VERSION &&
           c->owner_id == expected_owner &&
           c->slot_count == MK_CHANNEL_SLOT_COUNT &&
           c->payload_size == MK_CHANNEL_PAYLOAD_SIZE;
}

mk_channel_status_t mk_channel_attach(mk_channel_t *channel, uint8_t local_id,
                                      mk_channel_region_t *local,
                                      const mk_channel_region_t *peer,
                                      mk_channel_notify_fn notify,
                                      mk_channel_wait_fn wait,
                                      void *callback_cookie)
{
    if (channel == NULL || local == NULL || peer == NULL || local_id > 1u) {
        return MK_CHANNEL_INVALID_ARGUMENT;
    }
    if (!mk_channel_region_is_ready(local, local_id) ||
        !mk_channel_region_is_ready(peer, (uint8_t)(1u - local_id))) {
        return MK_CHANNEL_PROTOCOL_ERROR;
    }

    channel->local = local;
    channel->peer = peer;
    channel->local_id = local_id;
    channel->notify = notify;
    channel->wait = wait;
    channel->callback_cookie = callback_cookie;
    return MK_CHANNEL_OK;
}

mk_channel_status_t mk_channel_try_send(mk_channel_t *channel, uint16_t type,
                                        uint16_t flags, const void *payload,
                                        uint32_t length, uint64_t *sequence)
{
    if (channel == NULL || channel->local == NULL || channel->peer == NULL ||
        (payload == NULL && length != 0u)) {
        return MK_CHANNEL_INVALID_ARGUMENT;
    }
    if (length > MK_CHANNEL_PAYLOAD_SIZE) {
        return MK_CHANNEL_TOO_LARGE;
    }

    uint64_t head = *(volatile uint64_t *)&channel->local->control.tx_head;
    uint64_t tail = load_acquire_u64(&channel->peer->control.rx_tail);
    if (head - tail >= MK_CHANNEL_SLOT_COUNT) {
        bump_u64(&channel->local->control.full);
        return MK_CHANNEL_FULL;
    }

    /* Stage in aligned private memory: the caller's buffer may be unaligned. */
    uint64_t staged[MK_CHANNEL_PAYLOAD_SIZE / sizeof(uint64_t)];
    size_t words = (length + sizeof(uint64_t) - 1u) / sizeof(uint64_t);
    if (length != 0u) {
        staged[words - 1u] = 0;
        memcpy(staged, payload, length);
    }

    mk_channel_slot_t *slot =
        &channel->local->slots[head & (MK_CHANNEL_SLOT_COUNT - 1u)];
    volatile mk_channel_slot_t *vs = slot;
    vs->sequence = head + 1u;
    vs->length = length;
    vs->type = type;
    vs->flags = flags;
    shm_copy(slot->payload, staged, words * sizeof(uint64_t));

    store_release_u64(&channel->local->control.tx_head, head + 1u);
    bump_u64(&channel->local->control.sent);
    if (sequence != NULL) {
        *sequence = head + 1u;
    }
    if (channel->notify != NULL) {
        channel->notify(channel->callback_cookie);
    }
    return MK_CHANNEL_OK;
}

mk_channel_status_t mk_channel_try_recv(mk_channel_t *channel,
                                        mk_channel_message_t *message)
{
    if (channel == NULL || channel->local == NULL || channel->peer == NULL ||
        message == NULL) {
        return MK_CHANNEL_INVALID_ARGUMENT;
    }

    uint64_t tail = *(volatile uint64_t *)&channel->local->control.rx_tail;
    uint64_t head = load_acquire_u64(&channel->peer->control.tx_head);
    if (tail == head) {
        return MK_CHANNEL_EMPTY;
    }

    const volatile mk_channel_slot_t *slot =
        &channel->peer->slots[tail & (MK_CHANNEL_SLOT_COUNT - 1u)];
    /* Single read of each header field: the peer may change them under us. */
    uint64_t seq = slot->sequence;
    uint32_t length = slot->length;
    uint16_t type = slot->type;
    uint16_t flags = slot->flags;
    if (seq != tail + 1u || length > MK_CHANNEL_PAYLOAD_SIZE) {
        bump_u64(&channel->local->control.corrupt);
        return MK_CHANNEL_CORRUPT;
    }

    message->sequence = seq;
    message->length = length;
    message->type = type;
    message->flags = flags;
    size_t words = (length + sizeof(uint64_t) - 1u) / sizeof(uint64_t);
    shm_copy(message->payload, (const void *)slot->payload,
             words * sizeof(uint64_t));

    store_release_u64(&channel->local->control.rx_tail, tail + 1u);
    bump_u64(&channel->local->control.received);
    return MK_CHANNEL_OK;
}

mk_channel_status_t mk_channel_drop_one(mk_channel_t *channel)
{
    if (channel == NULL || channel->local == NULL || channel->peer == NULL) {
        return MK_CHANNEL_INVALID_ARGUMENT;
    }
    uint64_t tail = *(volatile uint64_t *)&channel->local->control.rx_tail;
    uint64_t head = load_acquire_u64(&channel->peer->control.tx_head);
    if (tail == head) {
        return MK_CHANNEL_EMPTY;
    }
    store_release_u64(&channel->local->control.rx_tail, tail + 1u);
    return MK_CHANNEL_OK;
}

mk_channel_status_t mk_channel_recv(mk_channel_t *channel,
                                    mk_channel_message_t *message)
{
    for (;;) {
        mk_channel_status_t status = mk_channel_try_recv(channel, message);
        if (status != MK_CHANNEL_EMPTY) {
            return status;
        }
        if (channel == NULL || channel->wait == NULL ||
            channel->wait(channel->callback_cookie) != 0) {
            return MK_CHANNEL_WAIT_FAILED;
        }
    }
}

mk_channel_status_t mk_channel_recv_until(mk_channel_t *channel,
                                          mk_channel_message_t *message,
                                          uint64_t deadline,
                                          mk_channel_now_fn now,
                                          mk_channel_wait_until_fn wait_until)
{
    if (now == NULL || wait_until == NULL) {
        return MK_CHANNEL_INVALID_ARGUMENT;
    }
    for (;;) {
        mk_channel_status_t status = mk_channel_try_recv(channel, message);
        if (status != MK_CHANNEL_EMPTY) {
            return status;
        }
        if (now(channel->callback_cookie) >= deadline) {
            return MK_CHANNEL_TIMEOUT;
        }
        if (wait_until(channel->callback_cookie, deadline) != 0) {
            return MK_CHANNEL_WAIT_FAILED;
        }
    }
}
