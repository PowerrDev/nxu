/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        drivers/tep/tep_boot.c
 *
 * See drivers/tep/tep_boot.h.
 */

#include <drivers/tep/tep_boot.h>

#include <drivers/tep/tep_crypto.h>
#include <drivers/tep/tep_mailbox.h>

#include <kern/console/console.h>
#include <kern/memory/heap.h>
#include <kern/process/proc.h>
#include <kern/process/thread.h>
#include <kern/sched_prism/sched.h>
#include <libk/sha256.h>
#include <vfs/vfs.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TEP_BOOT_LOG(format, ...) kprintf("%s: " format, __func__, ##__VA_ARGS__)

#define TEP_BOOT_POLL_US 1000000ULL
/* How long the disk may take to show bootd before the check gives up. */
#define TEP_BOOT_DISK_WAIT_POLLS 60U
/* tepOS answers that are neither a verdict nor "unavailable" before giving up. */
#define TEP_BOOT_ATTEMPTS_MAX 3U
#define TEP_BOOT_CHUNK 8192U

static volatile tep_boot_check_t g_check = TEP_BOOT_CHECK_PENDING;
static volatile uint32_t g_detail;
static bool g_started;

static void tep_boot_finish(tep_boot_check_t check, uint32_t detail)
{
	g_detail = detail;
	__atomic_store_n(&g_check, check, __ATOMIC_RELEASE);
}

static bool tep_boot_exists(const char *path)
{
	vnode_t vnode;

	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return false;

	vnode_rele(vnode);
	return true;
}

bool tep_boot_read_manifest(const char *path, uint8_t manifest[TEP_BOOT_MANIFEST_SIZE])
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	vnode_t vnode;
	uint32_t descriptor;
	uint64_t done = 0ULL;

	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return false;

	uint64_t length = vnode->v_size;

	vnode_rele(vnode);

	if (length != TEP_BOOT_MANIFEST_SIZE) {
		TEP_BOOT_LOG("%s is %llu bytes, not %u\n", path, (unsigned long long)length, (unsigned)TEP_BOOT_MANIFEST_SIZE);
		return false;
	}
	if (vfs_open(filedesc, path, VFS_OPEN_READ, &descriptor) != VFS_STATUS_OK) return false;

	while (done < length) {
		uint64_t got = 0ULL;

		if (vfs_read(filedesc, descriptor, manifest + done, length - done, &got) != VFS_STATUS_OK || got == 0ULL) break;
		done += got;
	}

	(void)vfs_close(filedesc, descriptor);
	return done == length;
}

/* Read in chunks, so the image never has to fit in memory. */
bool tep_boot_measure(const char *path, uint8_t digest[SHA256_DIGEST_SIZE], uint64_t *size)
{
	filedesc_t filedesc = &proc_kernel()->p_fd;
	vnode_t vnode;
	uint32_t descriptor;
	uint64_t done = 0ULL;
	sha256_context_t context;

	if (vfs_lookup(path, &vnode) != VFS_STATUS_OK) return false;

	uint64_t length = vnode->v_size;

	vnode_rele(vnode);

	uint8_t *chunk = kmalloc(TEP_BOOT_CHUNK);

	if (chunk == 0) return false;

	if (vfs_open(filedesc, path, VFS_OPEN_READ, &descriptor) != VFS_STATUS_OK) {
		(void)kfree(chunk);
		return false;
	}

	sha256_init(&context);

	while (done < length) {
		uint64_t got = 0ULL;
		uint64_t want = length - done < TEP_BOOT_CHUNK ? length - done : TEP_BOOT_CHUNK;

		if (vfs_read(filedesc, descriptor, chunk, want, &got) != VFS_STATUS_OK || got == 0ULL) break;

		sha256_update(&context, chunk, (size_t)got);
		done += got;
	}

	(void)vfs_close(filedesc, descriptor);
	(void)kfree(chunk);

	if (done != length) {
		TEP_BOOT_LOG("read %llu of %llu bytes of %s\n", (unsigned long long)done, (unsigned long long)length, path);
		return false;
	}

	sha256_final(&context, digest);
	*size = length;
	return true;
}

/* libk's SHA-256 against the FIPS 180-4 "abc" example, before trusting a measurement to it. */
static bool tep_boot_sha256_selftest(void)
{
	static const uint8_t expected[SHA256_DIGEST_SIZE] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad
	};
	uint8_t digest[SHA256_DIGEST_SIZE];

	sha256("abc", 3U, digest);
	return memcmp(digest, expected, sizeof(expected)) == 0;
}

static void tep_boot_hex(const uint8_t *bytes, uint32_t size, char *out)
{
	static const char digits[] = "0123456789abcdef";

	for (uint32_t index = 0U; index < size; index++) {
		out[2U * index] = digits[bytes[index] >> 4U];
		out[2U * index + 1U] = digits[bytes[index] & 0xfU];
	}
	out[2U * size] = '\0';
}

/* Whether the manifest's (still unverified) name field is TEP_BOOT_IMAGE_NAME, NUL padded. */
static bool tep_boot_names_bootd(const uint8_t manifest[TEP_BOOT_MANIFEST_SIZE])
{
	uint8_t name[16] = { 0 };

	memcpy(name, TEP_BOOT_IMAGE_NAME, sizeof(TEP_BOOT_IMAGE_NAME) - 1U);
	return memcmp(manifest + 24U, name, sizeof(name)) == 0;
}

/*
 * tep_boot_check_thread:
 *
 * Waits for bootd on disk and for tepOS's boot policy service, then measures
 * and asks once. Only "tepOS did not answer" is retried; a verdict is final
 * for this boot. Nothing here delays or stops bootd.
 */
static void tep_boot_check_thread(void *parameter)
{
	(void)parameter;

	uint8_t manifest[TEP_BOOT_MANIFEST_SIZE];
	uint8_t digest[SHA256_DIGEST_SIZE];
	char hex[2U * SHA256_DIGEST_SIZE + 1U];
	uint64_t size = 0ULL;
	uint32_t polls = 0U;

	while (!tep_boot_exists(TEP_BOOT_IMAGE_PATH)) {
		if (++polls >= TEP_BOOT_DISK_WAIT_POLLS) {
			TEP_BOOT_LOG("bootd not checked: %s not found\n", TEP_BOOT_IMAGE_PATH);
			tep_boot_finish(TEP_BOOT_CHECK_FAILED, 0U);
			return;
		}
		tep_mailbox_wait_us(TEP_BOOT_POLL_US);
	}

	if (!tep_boot_exists(TEP_BOOT_MANIFEST_PATH)) {
		TEP_BOOT_LOG("bootd not checked: the disk has no signed manifest for it\n");
		tep_boot_finish(TEP_BOOT_CHECK_NO_MANIFEST, 0U);
		return;
	}

	if (!tep_boot_read_manifest(TEP_BOOT_MANIFEST_PATH, manifest)) {
		TEP_BOOT_LOG("bootd not checked: %s could not be read\n", TEP_BOOT_MANIFEST_PATH);
		tep_boot_finish(TEP_BOOT_CHECK_FAILED, 0U);
		return;
	}
	if (!tep_boot_names_bootd(manifest)) {
		TEP_BOOT_LOG("bootd NOT verified (not enforced): the manifest names another image\n");
		tep_boot_finish(TEP_BOOT_CHECK_REFUSED, TEP_BOOT_WRONG_IMAGE);
		return;
	}

	if (!tep_boot_sha256_selftest()) {
		TEP_BOOT_LOG("bootd not checked: SHA-256 self-test failed\n");
		tep_boot_finish(TEP_BOOT_CHECK_FAILED, 0U);
		return;
	}
	if (!tep_boot_measure(TEP_BOOT_IMAGE_PATH, digest, &size)) {
		TEP_BOOT_LOG("bootd not checked: %s could not be measured\n", TEP_BOOT_IMAGE_PATH);
		tep_boot_finish(TEP_BOOT_CHECK_FAILED, 0U);
		return;
	}

	tep_boot_hex(digest, SHA256_DIGEST_SIZE, hex);
	TEP_BOOT_LOG("bootd measured: %llu bytes\n", (unsigned long long)size);
	TEP_BOOT_LOG("bootd SHA-256: %s\n", hex);

	bool waiting_logged = false;
	uint32_t attempts = 0U;

	for (;;) {
		if ((tep_mailbox_features() & TEP_MB_FEATURE_BOOT) == 0U) {
			if (!waiting_logged) {
				TEP_BOOT_LOG("waiting for tepOS's boot policy service to check bootd\n");
				waiting_logged = true;
			}
			tep_mailbox_wait_us(TEP_BOOT_POLL_US);
			continue;
		}

		uint32_t detail = 0U;
		tep_result_t result = tep_boot_verify(manifest, digest, &detail);

		switch (result) {
		case TEP_OK:
			TEP_BOOT_LOG("bootd verified by tepOS (not enforced): version %u\n", (unsigned)detail);
			tep_boot_finish(TEP_BOOT_CHECK_VERIFIED, detail);
			return;
		case TEP_ERR_DENIED:
			TEP_BOOT_LOG("bootd NOT verified by tepOS (not enforced): %s\n", tep_boot_reason_name(detail));
			tep_boot_finish(TEP_BOOT_CHECK_REFUSED, detail);
			return;
		case TEP_ERR_ROLLBACK:
			TEP_BOOT_LOG("bootd NOT verified by tepOS (not enforced): rollback, minimum version %u\n", (unsigned)detail);
			tep_boot_finish(TEP_BOOT_CHECK_REFUSED, detail);
			return;
		case TEP_ERR_UNAVAILABLE:
			break;
		default:
			if (++attempts >= TEP_BOOT_ATTEMPTS_MAX) {
				TEP_BOOT_LOG("bootd not checked: tepOS answered %s\n", tep_result_name(result));
				tep_boot_finish(TEP_BOOT_CHECK_FAILED, 0U);
				return;
			}
			break;
		}
		tep_mailbox_wait_us(TEP_BOOT_POLL_US);
	}
}

bool tep_boot_check_start(void)
{
	thread_t thread;

	if (g_started) return true;

	if (!kernel_thread_create(proc_task(proc_kernel()), tep_boot_check_thread, 0, &thread) || !sched_thread_start(thread)) {
		TEP_BOOT_LOG("check thread could not be started: bootd will not be checked\n");
		return false;
	}
	g_started = true;
	return true;
}

tep_boot_check_t tep_boot_check_state(uint32_t *detail)
{
	tep_boot_check_t check = __atomic_load_n(&g_check, __ATOMIC_ACQUIRE);

	if (detail != 0) *detail = g_detail;
	return check;
}

const char *tep_boot_check_name(tep_boot_check_t check)
{
	switch (check) {
	case TEP_BOOT_CHECK_PENDING: return "pending";
	case TEP_BOOT_CHECK_VERIFIED: return "verified";
	case TEP_BOOT_CHECK_REFUSED: return "refused";
	case TEP_BOOT_CHECK_NO_MANIFEST: return "no manifest";
	case TEP_BOOT_CHECK_FAILED: return "failed";
	}
	return "unknown";
}
