/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_mailbox.c
 *
 * See drivers/tep/tep_mailbox.h. The serial port behind it is
 * drivers/tep/tep_uart.h (PL011 on arm64, 16550 COM2 on i386), polled: one
 * request is in flight at a time and the monitor paces the traffic, so there
 * is nothing an interrupt would buy yet.
 */

#include <drivers/tep/tep_mailbox.h>
#include <drivers/tep/tep_uart.h>

#include <kern/console/console.h>
#include <kern/machine/cpu.h>
#include <kern/machine/timer.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEP_MAILBOX_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

#define TEP_RESPONSE_TIMEOUT_US 500000ULL
#define TEP_MONITOR_PERIOD_US 1000000ULL
#define TEP_MONITOR_FAILURES_MAX 3U

static volatile tep_link_state_t g_link_state = TEP_LINK_ABSENT;
static volatile uint8_t g_health = TEP_MB_HEALTH_STARTING;
static volatile uint16_t g_features;
static volatile uint32_t g_exchange_busy;
static uint32_t g_next_request_id = 1U;
static bool g_started;
static bool g_uart_ready;

/* ---- waiting ------------------------------------------------------------ */

/*
 * tep_mailbox_wait_us:
 *
 * Let other threads run for about `microseconds`. Sleeps once the periodic
 * timer runs; before that (boot-time tests) the scheduler is cooperative and
 * a sleep would never be woken, so it yields instead.
 */
void tep_mailbox_wait_us(uint64_t microseconds)
{
	uint64_t deadline = timer_get_microseconds() + microseconds;

	while (timer_get_microseconds() < deadline) {
		if (timer_get_interrupt_count() != 0ULL && sched_sleep_us(deadline - timer_get_microseconds())) continue;
		if (!sched_yield()) cpu_relax();
	}
}

/* ---- frames ------------------------------------------------------------- */

typedef struct {
	uint8_t buffer[TEP_MB_MAX_FRAME];
	uint32_t have;
} tep_frame_reader_t;

typedef struct {
	uint8_t version;
	uint8_t type;
	uint16_t command;
	uint16_t status;
	uint32_t request_id;
	uint16_t payload_len;
	const uint8_t *payload;
} tep_frame_t;

static uint32_t tep_crc32(uint32_t crc, const uint8_t *bytes, uint32_t length)
{
	for (uint32_t index = 0U; index < length; index++) {
		crc ^= bytes[index];
		for (uint32_t bit = 0U; bit < 8U; bit++) crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
	}
	return crc;
}

static uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (uint16_t)p[1] << 8U);
}

static uint32_t get32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8U | (uint32_t)p[2] << 16U | (uint32_t)p[3] << 24U;
}

static void put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8U);
}

static void put32(uint8_t *p, uint32_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8U);
	p[2] = (uint8_t)(value >> 16U);
	p[3] = (uint8_t)(value >> 24U);
}

/* Is the buffered prefix still a possible frame? */
static bool frame_prefix_ok(const tep_frame_reader_t *reader)
{
	if (reader->have >= 1U && reader->buffer[0] != (TEP_MB_MAGIC & 0xffU)) return false;
	if (reader->have >= 2U && reader->buffer[1] != (TEP_MB_MAGIC >> 8U)) return false;
	if (reader->have >= TEP_MB_HEADER_LEN && get16(&reader->buffer[12]) > TEP_MB_MAX_PAYLOAD) return false;
	return true;
}

/* Drop the first buffered byte and restart at the next possible magic. */
static void frame_resync(tep_frame_reader_t *reader)
{
	uint32_t start = 1U;

	while (start < reader->have && reader->buffer[start] != (TEP_MB_MAGIC & 0xffU)) start++;
	for (uint32_t index = start; index < reader->have; index++) reader->buffer[index - start] = reader->buffer[index];
	reader->have -= start;
}

/*
 * frame_feed:
 *
 * Add one received byte. Returns true when `frame` describes a complete frame
 * with valid magic, length and CRC; its payload points into the reader, valid
 * until the next call.
 */
static bool frame_feed(tep_frame_reader_t *reader, uint8_t byte, tep_frame_t *frame)
{
	reader->buffer[reader->have++] = byte;

	for (;;) {
		while (reader->have > 0U && !frame_prefix_ok(reader)) frame_resync(reader);
		if (reader->have < TEP_MB_HEADER_LEN) return false;

		uint32_t length = get16(&reader->buffer[12]);
		uint32_t total = TEP_MB_HEADER_LEN + length + TEP_MB_CRC_LEN;

		if (reader->have < total) return false;

		uint32_t crc = tep_crc32(0xffffffffU, reader->buffer, TEP_MB_HEADER_LEN + length) ^ 0xffffffffU;

		if (crc != get32(&reader->buffer[TEP_MB_HEADER_LEN + length])) {
			frame_resync(reader);
			continue;
		}

		frame->version = reader->buffer[2];
		frame->type = reader->buffer[3];
		frame->command = get16(&reader->buffer[4]);
		frame->status = get16(&reader->buffer[6]);
		frame->request_id = get32(&reader->buffer[8]);
		frame->payload_len = (uint16_t)length;
		frame->payload = &reader->buffer[TEP_MB_HEADER_LEN];
		reader->have = 0U;
		return true;
	}
}

static void frame_send(uint16_t command, uint32_t request_id, const uint8_t *payload, uint16_t payload_len)
{
	uint8_t header[TEP_MB_HEADER_LEN];
	uint8_t trailer[TEP_MB_CRC_LEN];

	put16(&header[0], TEP_MB_MAGIC);
	header[2] = TEP_MB_VERSION;
	header[3] = TEP_MB_TYPE_REQUEST;
	put16(&header[4], command);
	put16(&header[6], 0U);
	put32(&header[8], request_id);
	put16(&header[12], payload_len);
	put16(&header[14], 0U);

	uint32_t crc = tep_crc32(0xffffffffU, header, sizeof(header));

	crc = tep_crc32(crc, payload, payload_len) ^ 0xffffffffU;
	put32(trailer, crc);

	for (uint32_t index = 0U; index < sizeof(header); index++) tep_uart_putc(header[index]);
	for (uint32_t index = 0U; index < payload_len; index++) tep_uart_putc(payload[index]);
	for (uint32_t index = 0U; index < sizeof(trailer); index++) tep_uart_putc(trailer[index]);
}

/* ---- exchanges ---------------------------------------------------------- */

static void exchange_lock(void)
{
	while (__atomic_exchange_n(&g_exchange_busy, 1U, __ATOMIC_ACQUIRE) != 0U) tep_mailbox_wait_us(1000ULL);
}

static void exchange_unlock(void)
{
	__atomic_store_n(&g_exchange_busy, 0U, __ATOMIC_RELEASE);
}

/*
 * tep_exchange:
 *
 * One request/response round trip, whatever the link state. Stale bytes are
 * flushed first; responses to other request ids or commands are skipped.
 */
static tep_request_result_t tep_exchange(uint16_t command, const void *payload, uint16_t payload_len, tep_mailbox_response_t *response)
{
	static tep_frame_reader_t reader;
	tep_request_result_t result = TEP_REQ_TIMEOUT;

	if (!g_uart_ready || response == 0 || payload_len > TEP_MB_MAX_PAYLOAD || (payload_len != 0U && payload == 0)) return TEP_REQ_INVALID;

	exchange_lock();

	while (tep_uart_getc() >= 0) {
	}
	reader.have = 0U;

	uint32_t request_id = g_next_request_id++;

	frame_send(command, request_id, payload, payload_len);

	uint64_t deadline = timer_get_microseconds() + TEP_RESPONSE_TIMEOUT_US;

	while (timer_get_microseconds() < deadline) {
		int byte = tep_uart_getc();
		tep_frame_t frame;

		if (byte < 0) {
			tep_mailbox_wait_us(1000ULL);
			continue;
		}
		if (!frame_feed(&reader, (uint8_t)byte, &frame)) continue;
		if (frame.type != TEP_MB_TYPE_RESPONSE || frame.request_id != request_id || frame.command != command) continue;

		if (frame.version != TEP_MB_VERSION) {
			result = TEP_REQ_INVALID;
			break;
		}
		response->status = frame.status;
		response->payload_len = frame.payload_len;
		memcpy(response->payload, frame.payload, frame.payload_len);
		result = TEP_REQ_OK;
		break;
	}

	exchange_unlock();
	return result;
}

/* ---- monitor ------------------------------------------------------------ */

const char *tep_link_state_name(tep_link_state_t state)
{
	switch (state) {
	case TEP_LINK_ABSENT: return "absent";
	case TEP_LINK_UNAVAILABLE: return "unavailable";
	case TEP_LINK_AVAILABLE: return "available";
	}
	return "unknown";
}

const char *tep_request_result_name(tep_request_result_t result)
{
	switch (result) {
	case TEP_REQ_OK: return "ok";
	case TEP_REQ_UNAVAILABLE: return "tepOS unavailable";
	case TEP_REQ_TIMEOUT: return "timed out";
	case TEP_REQ_INVALID: return "invalid";
	}
	return "unknown";
}

const char *tep_mb_health_name(uint8_t health)
{
	switch (health) {
	case TEP_MB_HEALTH_STARTING: return "starting";
	case TEP_MB_HEALTH_OK: return "ok";
	case TEP_MB_HEALTH_DEGRADED: return "degraded";
	case TEP_MB_HEALTH_FAILED: return "failed";
	}
	return "unknown";
}

/* HELLO; on success the link becomes available. Returns why it failed, or 0. */
static const char *tep_monitor_hello(uint32_t *boot_id)
{
	tep_mailbox_response_t response;
	tep_request_result_t result = tep_exchange(TEP_MB_CMD_HELLO, 0, 0U, &response);

	if (result != TEP_REQ_OK) return tep_request_result_name(result);
	if (response.status != TEP_MB_OK || response.payload_len != TEP_MB_HELLO_LEN) return "HELLO refused or malformed";
	if (get16(&response.payload[0]) != TEP_MB_VERSION) return "protocol version mismatch";

	uint32_t version = get32(&response.payload[4]);

	*boot_id = get32(&response.payload[8]);
	g_features = get16(&response.payload[2]);
	TEP_MAILBOX_LOG("tepOS available\n");
	TEP_MAILBOX_LOG("tepOS protocol: %u\n", (unsigned)TEP_MB_VERSION);
	TEP_MAILBOX_LOG("tepOS version: %u.%u.%u\n", (unsigned)(version >> 16U), (unsigned)((version >> 8U) & 0xffU), (unsigned)(version & 0xffU));
	TEP_MAILBOX_LOG("tepOS boot id: %u\n", (unsigned)*boot_id);
	TEP_MAILBOX_LOG("tepOS crypto services: %s\n", (g_features & TEP_MB_FEATURE_CRYPTO) != 0U ? "yes" : "no");
	TEP_MAILBOX_LOG("tepOS passcode service: %s\n", (g_features & TEP_MB_FEATURE_AUTH) != 0U ? "yes" : "no");
	TEP_MAILBOX_LOG("tepOS boot policy service: %s\n", (g_features & TEP_MB_FEATURE_BOOT) != 0U ? "yes" : "no");
	return 0;
}

/* GET_HEALTH; records and logs changes. Returns why it failed, or 0. */
static const char *tep_monitor_health(void)
{
	tep_mailbox_response_t response;
	tep_request_result_t result = tep_exchange(TEP_MB_CMD_GET_HEALTH, 0, 0U, &response);

	if (result != TEP_REQ_OK) return tep_request_result_name(result);
	if (response.status != TEP_MB_OK || response.payload_len < 4U) return "GET_HEALTH refused or malformed";

	uint8_t services = response.payload[1];

	if (services > TEP_MB_MAX_SERVICES || response.payload_len != TEP_MB_HEALTH_LEN(services)) return "GET_HEALTH malformed";

	uint8_t health = response.payload[0];

	if (health != g_health) {
		g_health = health;
		TEP_MAILBOX_LOG("tepOS health: %s\n", tep_mb_health_name(health));
	}
	return 0;
}

static void tep_mailbox_monitor(void *parameter)
{
	(void)parameter;

	uint32_t failures = 0U;
	uint32_t boot_id = 0U;
	uint32_t last_boot_id = 0U;
	bool seen_tepos = false;
	bool waiting_logged = false;

	for (;;) {
		if (g_link_state != TEP_LINK_AVAILABLE) {
			const char *why = tep_monitor_hello(&boot_id);

			if (why == 0) {
				if (seen_tepos && boot_id != last_boot_id) TEP_MAILBOX_LOG("tepOS restarted since it was last seen\n");
				seen_tepos = true;
				last_boot_id = boot_id;
				failures = 0U;
				waiting_logged = false;
				g_health = 0xffU;
				__atomic_store_n(&g_link_state, TEP_LINK_AVAILABLE, __ATOMIC_RELEASE);
				(void)tep_monitor_health();
			} else if (!waiting_logged) {
				TEP_MAILBOX_LOG("waiting for tepOS: %s\n", why);
				waiting_logged = true;
			}
		} else {
			const char *why = tep_monitor_health();

			if (why == 0) {
				failures = 0U;
			} else if (++failures >= TEP_MONITOR_FAILURES_MAX) {
				__atomic_store_n(&g_link_state, TEP_LINK_UNAVAILABLE, __ATOMIC_RELEASE);
				TEP_MAILBOX_LOG("tepOS unavailable: %s\n", why);
				TEP_MAILBOX_LOG("requests to tepOS now fail closed\n");
				failures = 0U;
			}
		}

		tep_mailbox_wait_us(TEP_MONITOR_PERIOD_US);
	}
}

/* ---- public ------------------------------------------------------------- */

bool tep_mailbox_start(void)
{
	const char *where;
	uint64_t address;

	if (g_started) return true;

	if (!tep_uart_init(&where, &address)) {
		TEP_MAILBOX_LOG("no mailbox UART: tepOS unavailable, requests will fail closed\n");
		return false;
	}
	g_uart_ready = true;
	g_link_state = TEP_LINK_UNAVAILABLE;

	thread_t thread;

	if (!kernel_thread_create(proc_task(proc_kernel()), tep_mailbox_monitor, 0, &thread) || !sched_thread_start(thread)) {
		TEP_MAILBOX_LOG("monitor thread could not be started: tepOS unavailable\n");
		g_link_state = TEP_LINK_ABSENT;
		return false;
	}

	g_started = true;
	TEP_MAILBOX_LOG("Trusted Enclave mailbox on %s at 0x%llx\n", where, (unsigned long long)address);
	return true;
}

tep_link_state_t tep_mailbox_link_state(void)
{
	return __atomic_load_n(&g_link_state, __ATOMIC_ACQUIRE);
}

tep_request_result_t tep_mailbox_request(uint16_t command, const void *payload, uint16_t payload_len, tep_mailbox_response_t *response)
{
	if (tep_mailbox_link_state() != TEP_LINK_AVAILABLE) return TEP_REQ_UNAVAILABLE;
	return tep_exchange(command, payload, payload_len, response);
}

uint16_t tep_mailbox_features(void)
{
	return tep_mailbox_link_state() == TEP_LINK_AVAILABLE ? g_features : 0U;
}

bool tep_mailbox_health(tep_mb_health_t *health)
{
	if (health == 0 || tep_mailbox_link_state() != TEP_LINK_AVAILABLE) return false;
	*health = (tep_mb_health_t)g_health;
	return true;
}
