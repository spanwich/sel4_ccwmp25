/* SPDX-License-Identifier: BSD-2-Clause */
#include "demo_wire.h"
#include "gateway_abi.h"

#include <microkit.h>

__attribute__((section(".auth_config"))) AuthConfig auth_config;

static const char ADMIN_TOKEN[] = "admin.demo.token";
static const char OPERATOR_TOKEN[] = "operator.demo.token";
static AuthMessage pending_response;
static bool response_pending;
#if D1_FAULT_KIND == 2 || D1_FAULT_KIND == 5 || D1_FAULT_KIND == 6
static uint32_t private_delivery_count;
#endif

static bool contains(const uint8_t *bytes, uint32_t length, const char *needle)
{
    const size_t needle_length = strlen(needle);
    if (needle_length == 0u || needle_length > length) {
        return false;
    }
    for (uint32_t i = 0u; i + needle_length <= length; ++i) {
        if (memcmp(bytes + i, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool token_is(const AuthMessage *message, const char *token)
{
    const size_t length = strlen(token);
    return message->length == length &&
        memcmp(message->payload, token, length) == 0;
}

static void authenticate(AuthMessage *message)
{
    if (message->length > AUTH_PAYLOAD_BYTES) {
        message->valid = 0u;
        return;
    }
    if (message->kind == AUTH_IDENTIFY) {
        const bool admin = token_is(message, ADMIN_TOKEN);
        const bool operator = token_is(message, OPERATOR_TOKEN);
        message->kind = AUTH_IDENTIFY_RESULT;
        message->length = 0u;
        message->valid = admin || operator;
        message->role = admin ? ROLE_ADMIN :
            operator ? ROLE_OPERATOR : ROLE_NONE;
        message->scope = admin ? UINT16_C(0x0007) :
            operator ? UINT16_C(0x0001) : 0u;
        memset(message->payload, 0, sizeof(message->payload));
        return;
    }
    if (message->kind == AUTH_LOGIN) {
        const bool admin =
            contains(message->payload, message->length,
                     "\"username\":\"admin\"") &&
            contains(message->payload, message->length,
                     "\"password\":\"admin456\"");
        const bool operator = !admin &&
            contains(message->payload, message->length,
                     "\"username\":\"operator\"") &&
            contains(message->payload, message->length,
                     "\"password\":\"oper789\"");
        const char *token = admin ? ADMIN_TOKEN :
            operator ? OPERATOR_TOKEN : NULL;
        message->kind = AUTH_LOGIN_RESULT;
        message->valid = token != NULL;
        message->role = ROLE_NONE;
        message->scope = 0u;
        message->length = token == NULL ? 0u : (uint32_t)strlen(token);
        memset(message->payload, 0, sizeof(message->payload));
        if (token != NULL) {
            memcpy(message->payload, token, message->length);
        }
        return;
    }
    message->valid = 0u;
    message->length = 0u;
}

void init(void)
{
    microkit_dbg_puts("HTTP_DEMO_COMPONENT,name=authenticator,mode=private_identity_oracle\n");
}

void notified(microkit_channel channel)
{
#if D1_FAULT_KIND == 4
    microkit_dbg_puts("D1_INJECTION_BEGIN,origin=AUTH,target=self_fault\n");
    __builtin_trap();
#endif
    if (channel != auth_config.channel) {
        return;
    }
#if D1_FAULT_KIND == 2 || D1_FAULT_KIND == 5 || D1_FAULT_KIND == 6
    microkit_dbg_puts("D1_TARGET_COUNTER,pd=AUTH,before=");
    microkit_dbg_put32(private_delivery_count);
    microkit_dbg_puts(",after=");
    microkit_dbg_put32(++private_delivery_count);
    microkit_dbg_putc('\n');
#endif
    AuthQueue *request = (AuthQueue *)(uintptr_t)auth_config.request_vaddr;
    AuthQueue *response = (AuthQueue *)(uintptr_t)auth_config.response_vaddr;
    if (response_pending) {
        if (!auth_queue_send(response, &pending_response)) {
            return;
        }
        response_pending = false;
        microkit_notify(auth_config.channel);
    }
    AuthMessage message;
    if (!auth_queue_receive(request, &message)) {
        return;
    }
    authenticate(&message);
    if (!auth_queue_send(response, &message)) {
        pending_response = message;
        response_pending = true;
        return;
    }
    microkit_notify(auth_config.channel);
}
