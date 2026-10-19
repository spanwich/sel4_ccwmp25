/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Single-in-flight plaintext HTTP/1.1 bridge for the demonstration. Network
 * frames are owned by the pinned sDDF/lwIP subsystem; only HTTP bytes cross
 * the research-component mailboxes.
 */
#include "demo_wire.h"

#include <microkit.h>
#include <os/sddf.h>
#include <sddf/network/config.h>
#include <sddf/network/lib_sddf_lwip.h>
#include <sddf/network/queue.h>
#include <sddf/timer/client.h>
#include <sddf/timer/config.h>
#include <sddf/util/printf.h>

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define DEMO_HTTP_PORT 8080u
#define LWIP_TICK_MS 100u

__attribute__((section(".timer_client_config")))
timer_client_config_t timer_config;
__attribute__((section(".net_client_config")))
net_client_config_t net_config;
__attribute__((section(".lib_sddf_lwip_config")))
lib_sddf_lwip_config_t lib_sddf_lwip_config;
__attribute__((section(".demo_config"))) DemoConfig demo_config;

static net_queue_handle_t net_rx_handle;
static net_queue_handle_t net_tx_handle;
static struct tcp_pcb *active_pcb;
static uint16_t next_request_epoch = 1u;
static uint16_t active_request_epoch;
static uint8_t request_buffer[DEMO_PAYLOAD_BYTES];
static uint32_t request_length;
static uint32_t response_unacked;
static bool request_ready;
static bool request_submitted;

static uint32_t find_header_end(const uint8_t *bytes, uint32_t length)
{
    for (uint32_t i = 0u; i + 3u < length; ++i) {
        if (bytes[i] == '\r' && bytes[i + 1u] == '\n' &&
            bytes[i + 2u] == '\r' && bytes[i + 3u] == '\n') {
            return i + 4u;
        }
    }
    return 0u;
}

static uint32_t parse_content_length(const uint8_t *bytes, uint32_t header_end)
{
    static const char field[] = "Content-Length:";
    const size_t field_length = sizeof(field) - 1u;
    for (uint32_t i = 0u; i + field_length < header_end; ++i) {
        if (memcmp(bytes + i, field, field_length) != 0) {
            continue;
        }
        uint32_t cursor = i + (uint32_t)field_length;
        while (cursor < header_end &&
               (bytes[cursor] == ' ' || bytes[cursor] == '\t')) {
            ++cursor;
        }
        uint32_t value = 0u;
        while (cursor < header_end && bytes[cursor] >= '0' &&
               bytes[cursor] <= '9') {
            if (value > (DEMO_PAYLOAD_BYTES - 9u) / 10u) {
                return DEMO_PAYLOAD_BYTES + 1u;
            }
            value = value * 10u + (uint32_t)(bytes[cursor] - '0');
            ++cursor;
        }
        return value;
    }
    return 0u;
}

static bool request_complete(void)
{
    const uint32_t header_end = find_header_end(request_buffer, request_length);
    if (header_end == 0u) {
        return false;
    }
    const uint32_t body_length =
        parse_content_length(request_buffer, header_end);
    return body_length <= DEMO_PAYLOAD_BYTES &&
        request_length >= header_end + body_length;
}

static void reset_connection_state(void)
{
    active_pcb = NULL;
    active_request_epoch = 0u;
    request_length = 0u;
    response_unacked = 0u;
    request_ready = false;
    request_submitted = false;
}

static err_t response_sent(void *argument, struct tcp_pcb *pcb, u16_t length)
{
    (void)argument;
    if (pcb != active_pcb) {
        return ERR_OK;
    }
    if (length >= response_unacked) {
        response_unacked = 0u;
        tcp_arg(pcb, NULL);
        tcp_sent(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
        }
        reset_connection_state();
    } else {
        response_unacked -= length;
    }
    return ERR_OK;
}

static void connection_error(void *argument, err_t error)
{
    struct tcp_pcb *failed_pcb = argument;
    (void)error;
    if (failed_pcb == active_pcb) {
        reset_connection_state();
    }
}

static bool submit_ready_request(void)
{
    if (!request_ready || request_submitted) {
        return true;
    }
    DemoMessage message = {
        .kind = DEMO_MESSAGE_RAW_HTTP,
        .length = request_length,
        .request_epoch = active_request_epoch,
    };
    memcpy(message.payload, request_buffer, request_length);
    if (!demo_queue_send(demo_output_queue(&demo_config), &message)) {
        return false;
    }
    request_submitted = true;
    microkit_notify(demo_config.output_channel);
    return true;
}

static err_t receive_http(void *argument, struct tcp_pcb *pcb,
                          struct pbuf *pbuf, err_t error)
{
    (void)argument;
    if (pcb != active_pcb) {
        if (pbuf != NULL) {
            pbuf_free(pbuf);
        }
        return ERR_OK;
    }
    if (pbuf == NULL) {
        tcp_close(pcb);
        reset_connection_state();
        return ERR_OK;
    }
    if (error != ERR_OK) {
        return error;
    }
    if (request_ready || request_submitted ||
        request_length + pbuf->tot_len > sizeof(request_buffer)) {
        return ERR_MEM;
    }

    const u16_t copied = pbuf_copy_partial(
        pbuf, request_buffer + request_length, pbuf->tot_len, 0u);
    request_length += copied;
    tcp_recved(pcb, copied);
    pbuf_free(pbuf);

    if (request_complete()) {
        request_ready = true;
        (void)submit_ready_request();
    }
    return ERR_OK;
}

static err_t accept_http(void *argument, struct tcp_pcb *pcb, err_t error)
{
    (void)argument;
    if (error != ERR_OK || active_pcb != NULL) {
        return ERR_MEM;
    }
    reset_connection_state();
    active_pcb = pcb;
    active_request_epoch = next_request_epoch;
    ++next_request_epoch;
    if (next_request_epoch == 0u) {
        next_request_epoch = 1u;
    }
    tcp_nagle_disable(pcb);
    tcp_arg(pcb, pcb);
    tcp_recv(pcb, receive_http);
    tcp_sent(pcb, response_sent);
    tcp_err(pcb, connection_error);
    return ERR_OK;
}

static int setup_http_listener(void)
{
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
    if (pcb == NULL) {
        return -1;
    }
    err_t error = tcp_bind(pcb, IP_ANY_TYPE, DEMO_HTTP_PORT);
    if (error != ERR_OK) {
        return -1;
    }
    pcb = tcp_listen_with_backlog_and_err(pcb, 1u, &error);
    if (pcb == NULL || error != ERR_OK) {
        return -1;
    }
    tcp_accept(pcb, accept_http);
    return 0;
}

static void network_ready(char *address)
{
    sddf_printf(
        "HTTP_DEMO_READY,address=%s,port=%u,transport=plaintext\n",
        address, DEMO_HTTP_PORT);
}

static void set_timeout(void)
{
    sddf_timer_set_timeout(timer_config.driver_id,
                           LWIP_TICK_MS * NS_IN_MS);
}

static void handle_pipeline_response(void)
{
    DemoMessage message;
    if (!demo_queue_receive(demo_input_queue(&demo_config), &message)) {
        return;
    }
    microkit_notify(demo_config.input_channel);
    if (message.kind != DEMO_MESSAGE_HTTP_RESPONSE ||
        message.length > UINT16_MAX || message.request_epoch == 0u) {
        microkit_dbg_puts("HTTP_DEMO_FAIL,reason=invalid_http_response\n");
        return;
    }
    if (active_pcb == NULL ||
        message.request_epoch != active_request_epoch) {
        microkit_dbg_puts("HTTP_DEMO_STALE_RESPONSE,dropped=1\n");
        return;
    }
    const err_t error = tcp_write(active_pcb, message.payload,
                                  (u16_t)message.length,
                                  TCP_WRITE_FLAG_COPY);
    if (error != ERR_OK) {
        microkit_dbg_puts("HTTP_DEMO_FAIL,reason=tcp_write\n");
        tcp_abort(active_pcb);
        reset_connection_state();
        return;
    }
    response_unacked = message.length;
    tcp_output(active_pcb);
}

void init(void)
{
    net_queue_init(&net_rx_handle, net_config.rx.free_queue.vaddr,
                   net_config.rx.active_queue.vaddr,
                   net_config.rx.num_buffers);
    net_queue_init(&net_tx_handle, net_config.tx.free_queue.vaddr,
                   net_config.tx.active_queue.vaddr,
                   net_config.tx.num_buffers);
    net_buffers_init(&net_tx_handle, 0u);
    sddf_lwip_init(&lib_sddf_lwip_config, &net_config, &timer_config,
                   net_rx_handle, net_tx_handle, NULL, NULL, network_ready,
                   NULL, NULL, NULL);
    set_timeout();
    if (setup_http_listener() != 0) {
        microkit_dbg_puts("HTTP_DEMO_FAIL,reason=http_listener\n");
    } else {
        microkit_dbg_puts("HTTP_DEMO_COMPONENT,name=lwip_proxy,port=8080\n");
    }
    sddf_lwip_maybe_notify();
}

void notified(sddf_channel channel)
{
    if (channel == net_config.rx.id) {
        sddf_lwip_process_rx();
    } else if (channel == timer_config.driver_id) {
        sddf_lwip_process_timeout();
        set_timeout();
    } else if (channel == demo_config.input_channel) {
        handle_pipeline_response();
    } else if (channel == demo_config.output_channel) {
        /* The first stage acknowledged the request mailbox. */
    } else if (channel != net_config.tx.id) {
        microkit_dbg_puts("HTTP_DEMO_FAIL,reason=unexpected_lwip_channel\n");
    }
    (void)submit_ready_request();
    sddf_lwip_maybe_notify();
}
