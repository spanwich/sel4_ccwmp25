/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Bounded, architecture-neutral mailbox ABI for the HTTP demonstration.
 */
#ifndef POLICY_GATEWAY_DEMO_WIRE_H
#define POLICY_GATEWAY_DEMO_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define DEMO_QUEUE_BYTES UINT64_C(0x2000)
#define DEMO_PAYLOAD_BYTES 4096u
#define AUTH_PAYLOAD_BYTES 1536u
#define AUTH_QUEUE_BYTES UINT64_C(0x1000)

typedef enum {
    DEMO_MESSAGE_RAW_HTTP = 1,
    DEMO_MESSAGE_SECURITY_PARAMS = 2,
    DEMO_MESSAGE_BODY_RESPONSE = 3,
    DEMO_MESSAGE_HTTP_RESPONSE = 4
} DemoMessageKind;

typedef struct {
    uint32_t kind;
    uint32_t length;
    uint16_t http_status;
    uint16_t request_epoch;
    uint8_t payload[DEMO_PAYLOAD_BYTES];
} DemoMessage;

typedef struct {
    volatile uint32_t ready;
    uint32_t reserved;
    DemoMessage message;
} DemoQueue;

typedef struct {
    uint64_t input_vaddr;
    uint64_t output_vaddr;
    uint8_t input_channel;
    uint8_t output_channel;
    uint8_t reserved[6];
} DemoConfig;

typedef enum {
    AUTH_IDENTIFY = 1,
    AUTH_LOGIN = 2,
    AUTH_IDENTIFY_RESULT = 3,
    AUTH_LOGIN_RESULT = 4
} AuthMessageKind;

typedef struct {
    uint32_t kind;
    uint32_t length;
    uint8_t valid;
    uint8_t role;
    uint16_t scope;
    uint8_t payload[AUTH_PAYLOAD_BYTES];
} AuthMessage;

typedef struct {
    volatile uint32_t ready;
    uint32_t reserved;
    AuthMessage message;
} AuthQueue;

typedef struct {
    uint64_t request_vaddr;
    uint64_t response_vaddr;
    uint8_t channel;
    uint8_t reserved[7];
} AuthConfig;

_Static_assert(sizeof(DemoQueue) <= DEMO_QUEUE_BYTES,
               "demo queue exceeds its shared region");
_Static_assert(sizeof(DemoMessage) == 4108u,
               "demo message ABI changed");
_Static_assert(sizeof(DemoConfig) == 24u, "demo config ABI changed");
_Static_assert(sizeof(AuthQueue) <= AUTH_QUEUE_BYTES,
               "auth queue exceeds its private region");
_Static_assert(sizeof(AuthConfig) == 24u, "auth config ABI changed");

static inline DemoQueue *demo_input_queue(const DemoConfig *config)
{
    return (DemoQueue *)(uintptr_t)config->input_vaddr;
}

static inline DemoQueue *demo_output_queue(const DemoConfig *config)
{
    return (DemoQueue *)(uintptr_t)config->output_vaddr;
}

static inline bool demo_queue_receive(DemoQueue *queue, DemoMessage *message)
{
    if (__atomic_load_n(&queue->ready, __ATOMIC_ACQUIRE) == 0u) {
        return false;
    }
    memcpy(message, &queue->message, sizeof(*message));
    __atomic_store_n(&queue->ready, 0u, __ATOMIC_RELEASE);
    return true;
}

static inline bool demo_queue_send(DemoQueue *queue,
                                   const DemoMessage *message)
{
    if (__atomic_load_n(&queue->ready, __ATOMIC_ACQUIRE) != 0u) {
        return false;
    }
    memcpy(&queue->message, message, sizeof(*message));
    __atomic_store_n(&queue->ready, 1u, __ATOMIC_RELEASE);
    return true;
}

static inline bool auth_queue_receive(AuthQueue *queue, AuthMessage *message)
{
    if (__atomic_load_n(&queue->ready, __ATOMIC_ACQUIRE) == 0u) {
        return false;
    }
    memcpy(message, &queue->message, sizeof(*message));
    __atomic_store_n(&queue->ready, 0u, __ATOMIC_RELEASE);
    return true;
}

static inline bool auth_queue_send(AuthQueue *queue,
                                   const AuthMessage *message)
{
    if (__atomic_load_n(&queue->ready, __ATOMIC_ACQUIRE) != 0u) {
        return false;
    }
    memcpy(&queue->message, message, sizeof(*message));
    __atomic_store_n(&queue->ready, 1u, __ATOMIC_RELEASE);
    return true;
}

static inline void demo_set_body_response(DemoMessage *message,
                                          uint16_t http_status,
                                          const char *body)
{
    size_t length = strlen(body);
    if (length > DEMO_PAYLOAD_BYTES) {
        length = DEMO_PAYLOAD_BYTES;
    }
    message->kind = DEMO_MESSAGE_BODY_RESPONSE;
    message->http_status = http_status;
    message->length = (uint32_t)length;
    memcpy(message->payload, body, length);
}

#endif
