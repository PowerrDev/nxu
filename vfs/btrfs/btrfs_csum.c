/*
 * Copyright (c) 2026 NXU Project. All rights reserved.
 */
/*
 * File:        vfs/btrfs/btrfs_csum.c
 *
 * Checksums and hashes of the Btrfs format, plus the status-code names.
 *
 * The core carries its own table-driven CRC-32C (libk's crc32c() is bitwise,
 * one branch-free loop per bit, which is slow over 16 KiB tree blocks) so it
 * needs nothing from the kernel and builds unchanged on the host. The table is
 * the constant CRC-32C (Castagnoli, reflected polynomial 0x82F63B78) table.
 *
 * Btrfs stores its default checksum as the standard CRC-32C (initial value
 * ~0, final complement) in the first four bytes of the 32-byte csum field,
 * little-endian. Directory-entry names are hashed with the *running* form,
 * crc32c(~1, name), without the final complement (btrfs_name_hash()).
 */

#include "btrfs_format.h"

#include <string.h>

static const uint32_t g_crc32c_table[256] = {
	0x00000000U, 0xF26B8303U, 0xE13B70F7U, 0x1350F3F4U,
	0xC79A971FU, 0x35F1141CU, 0x26A1E7E8U, 0xD4CA64EBU,
	0x8AD958CFU, 0x78B2DBCCU, 0x6BE22838U, 0x9989AB3BU,
	0x4D43CFD0U, 0xBF284CD3U, 0xAC78BF27U, 0x5E133C24U,
	0x105EC76FU, 0xE235446CU, 0xF165B798U, 0x030E349BU,
	0xD7C45070U, 0x25AFD373U, 0x36FF2087U, 0xC494A384U,
	0x9A879FA0U, 0x68EC1CA3U, 0x7BBCEF57U, 0x89D76C54U,
	0x5D1D08BFU, 0xAF768BBCU, 0xBC267848U, 0x4E4DFB4BU,
	0x20BD8EDEU, 0xD2D60DDDU, 0xC186FE29U, 0x33ED7D2AU,
	0xE72719C1U, 0x154C9AC2U, 0x061C6936U, 0xF477EA35U,
	0xAA64D611U, 0x580F5512U, 0x4B5FA6E6U, 0xB93425E5U,
	0x6DFE410EU, 0x9F95C20DU, 0x8CC531F9U, 0x7EAEB2FAU,
	0x30E349B1U, 0xC288CAB2U, 0xD1D83946U, 0x23B3BA45U,
	0xF779DEAEU, 0x05125DADU, 0x1642AE59U, 0xE4292D5AU,
	0xBA3A117EU, 0x4851927DU, 0x5B016189U, 0xA96AE28AU,
	0x7DA08661U, 0x8FCB0562U, 0x9C9BF696U, 0x6EF07595U,
	0x417B1DBCU, 0xB3109EBFU, 0xA0406D4BU, 0x522BEE48U,
	0x86E18AA3U, 0x748A09A0U, 0x67DAFA54U, 0x95B17957U,
	0xCBA24573U, 0x39C9C670U, 0x2A993584U, 0xD8F2B687U,
	0x0C38D26CU, 0xFE53516FU, 0xED03A29BU, 0x1F682198U,
	0x5125DAD3U, 0xA34E59D0U, 0xB01EAA24U, 0x42752927U,
	0x96BF4DCCU, 0x64D4CECFU, 0x77843D3BU, 0x85EFBE38U,
	0xDBFC821CU, 0x2997011FU, 0x3AC7F2EBU, 0xC8AC71E8U,
	0x1C661503U, 0xEE0D9600U, 0xFD5D65F4U, 0x0F36E6F7U,
	0x61C69362U, 0x93AD1061U, 0x80FDE395U, 0x72966096U,
	0xA65C047DU, 0x5437877EU, 0x4767748AU, 0xB50CF789U,
	0xEB1FCBADU, 0x197448AEU, 0x0A24BB5AU, 0xF84F3859U,
	0x2C855CB2U, 0xDEEEDFB1U, 0xCDBE2C45U, 0x3FD5AF46U,
	0x7198540DU, 0x83F3D70EU, 0x90A324FAU, 0x62C8A7F9U,
	0xB602C312U, 0x44694011U, 0x5739B3E5U, 0xA55230E6U,
	0xFB410CC2U, 0x092A8FC1U, 0x1A7A7C35U, 0xE811FF36U,
	0x3CDB9BDDU, 0xCEB018DEU, 0xDDE0EB2AU, 0x2F8B6829U,
	0x82F63B78U, 0x709DB87BU, 0x63CD4B8FU, 0x91A6C88CU,
	0x456CAC67U, 0xB7072F64U, 0xA457DC90U, 0x563C5F93U,
	0x082F63B7U, 0xFA44E0B4U, 0xE9141340U, 0x1B7F9043U,
	0xCFB5F4A8U, 0x3DDE77ABU, 0x2E8E845FU, 0xDCE5075CU,
	0x92A8FC17U, 0x60C37F14U, 0x73938CE0U, 0x81F80FE3U,
	0x55326B08U, 0xA759E80BU, 0xB4091BFFU, 0x466298FCU,
	0x1871A4D8U, 0xEA1A27DBU, 0xF94AD42FU, 0x0B21572CU,
	0xDFEB33C7U, 0x2D80B0C4U, 0x3ED04330U, 0xCCBBC033U,
	0xA24BB5A6U, 0x502036A5U, 0x4370C551U, 0xB11B4652U,
	0x65D122B9U, 0x97BAA1BAU, 0x84EA524EU, 0x7681D14DU,
	0x2892ED69U, 0xDAF96E6AU, 0xC9A99D9EU, 0x3BC21E9DU,
	0xEF087A76U, 0x1D63F975U, 0x0E330A81U, 0xFC588982U,
	0xB21572C9U, 0x407EF1CAU, 0x532E023EU, 0xA145813DU,
	0x758FE5D6U, 0x87E466D5U, 0x94B49521U, 0x66DF1622U,
	0x38CC2A06U, 0xCAA7A905U, 0xD9F75AF1U, 0x2B9CD9F2U,
	0xFF56BD19U, 0x0D3D3E1AU, 0x1E6DCDEEU, 0xEC064EEDU,
	0xC38D26C4U, 0x31E6A5C7U, 0x22B65633U, 0xD0DDD530U,
	0x0417B1DBU, 0xF67C32D8U, 0xE52CC12CU, 0x1747422FU,
	0x49547E0BU, 0xBB3FFD08U, 0xA86F0EFCU, 0x5A048DFFU,
	0x8ECEE914U, 0x7CA56A17U, 0x6FF599E3U, 0x9D9E1AE0U,
	0xD3D3E1ABU, 0x21B862A8U, 0x32E8915CU, 0xC083125FU,
	0x144976B4U, 0xE622F5B7U, 0xF5720643U, 0x07198540U,
	0x590AB964U, 0xAB613A67U, 0xB831C993U, 0x4A5A4A90U,
	0x9E902E7BU, 0x6CFBAD78U, 0x7FAB5E8CU, 0x8DC0DD8FU,
	0xE330A81AU, 0x115B2B19U, 0x020BD8EDU, 0xF0605BEEU,
	0x24AA3F05U, 0xD6C1BC06U, 0xC5914FF2U, 0x37FACCF1U,
	0x69E9F0D5U, 0x9B8273D6U, 0x88D28022U, 0x7AB90321U,
	0xAE7367CAU, 0x5C18E4C9U, 0x4F48173DU, 0xBD23943EU,
	0xF36E6F75U, 0x0105EC76U, 0x12551F82U, 0xE03E9C81U,
	0x34F4F86AU, 0xC69F7B69U, 0xD5CF889DU, 0x27A40B9EU,
	0x79B737BAU, 0x8BDCB4B9U, 0x988C474DU, 0x6AE7C44EU,
	0xBE2DA0A5U, 0x4C4623A6U, 0x5F16D052U, 0xAD7D5351U,
};

uint32_t btrfs_crc32c(uint32_t crc, const void *data, size_t length)
{
	const uint8_t *bytes = data;

	for (size_t index = 0U; index < length; index++) {
		crc = g_crc32c_table[(crc ^ bytes[index]) & 0xFFU] ^ (crc >> 8U);
	}

	return crc;
}

uint32_t btrfs_csum_crc32c(const void *data, size_t length)
{
	return ~btrfs_crc32c(~0U, data, length);
}

static uint64_t btrfs_rotl64(uint64_t value, unsigned bits)
{
	return (value << bits) | (value >> (64U - bits));
}

static uint64_t btrfs_xxh_round(uint64_t acc, uint64_t input)
{
	return btrfs_rotl64(acc + input * 14029467366897019727ULL, 31U) * 11400714785074694791ULL;
}

static uint64_t btrfs_xxh_merge(uint64_t acc, uint64_t value)
{
	return (acc ^ btrfs_xxh_round(0ULL, value)) * 11400714785074694791ULL + 9650029242287828579ULL;
}

uint64_t btrfs_xxh64(const void *data, size_t length, uint64_t seed)
{
	const uint8_t *p = data;
	const uint8_t *end = p + length;
	uint64_t h;

	if (length >= 32U) {
		uint64_t v1 = seed + 11400714785074694791ULL + 14029467366897019727ULL;
		uint64_t v2 = seed + 14029467366897019727ULL;
		uint64_t v3 = seed;
		uint64_t v4 = seed - 11400714785074694791ULL;

		do {
			v1 = btrfs_xxh_round(v1, btrfs_get_le64(p));
			v2 = btrfs_xxh_round(v2, btrfs_get_le64(p + 8));
			v3 = btrfs_xxh_round(v3, btrfs_get_le64(p + 16));
			v4 = btrfs_xxh_round(v4, btrfs_get_le64(p + 24));
			p += 32;
		} while ((size_t)(end - p) >= 32U);

		h = btrfs_rotl64(v1, 1U) + btrfs_rotl64(v2, 7U) + btrfs_rotl64(v3, 12U) + btrfs_rotl64(v4, 18U);
		h = btrfs_xxh_merge(h, v1);
		h = btrfs_xxh_merge(h, v2);
		h = btrfs_xxh_merge(h, v3);
		h = btrfs_xxh_merge(h, v4);
	} else {
		h = seed + 2870177450012600261ULL;
	}

	h += (uint64_t)length;

	while ((size_t)(end - p) >= 8U) {
		h ^= btrfs_xxh_round(0ULL, btrfs_get_le64(p));
		h = btrfs_rotl64(h, 27U) * 11400714785074694791ULL + 9650029242287828579ULL;
		p += 8;
	}

	if ((size_t)(end - p) >= 4U) {
		h ^= (uint64_t)btrfs_get_le32(p) * 11400714785074694791ULL;
		h = btrfs_rotl64(h, 23U) * 14029467366897019727ULL + 1609587929392839161ULL;
		p += 4;
	}

	while (p < end) {
		h ^= (uint64_t)*p++ * 2870177450012600261ULL;
		h = btrfs_rotl64(h, 11U) * 11400714785074694791ULL;
	}

	h ^= h >> 33U;
	h *= 14029467366897019727ULL;
	h ^= h >> 29U;
	h *= 1609587929392839161ULL;
	h ^= h >> 32U;
	return h;
}

bool btrfs_csum_data(uint32_t type, const void *data, size_t length, uint8_t out[BTRFS_CSUM_SIZE])
{
	for (uint32_t index = 0U; index < BTRFS_CSUM_SIZE; index++) out[index] = 0U;

	switch (type) {
	case BTRFS_CSUM_TYPE_CRC32:
		btrfs_put_le32(out, btrfs_csum_crc32c(data, length));
		return true;
	case BTRFS_CSUM_TYPE_XXHASH:
		btrfs_put_le64(out, btrfs_xxh64(data, length, 0ULL));
		return true;
	case BTRFS_CSUM_TYPE_SHA256:
		btrfs_sha256(data, length, out);
		return true;
	case BTRFS_CSUM_TYPE_BLAKE2:
		btrfs_blake2b_256(data, length, out);
		return true;
	default:
		return false;
	}
}

bool btrfs_csum_matches(uint32_t type, const uint8_t *stored, const void *data, size_t length)
{
	uint8_t actual[BTRFS_CSUM_SIZE];
	uint32_t size = btrfs_csum_type_size(type);

	if (size == 0U || !btrfs_csum_data(type, data, length, actual)) return false;

	return memcmp(actual, stored, size) == 0;
}

uint32_t btrfs_name_hash(const uint8_t *name, size_t length)
{
	return btrfs_crc32c((uint32_t)~1U, name, length);
}

uint64_t btrfs_extref_hash(uint64_t parent_objectid, const uint8_t *name, size_t length)
{
	uint8_t parent[8];
	btrfs_put_le64(parent, parent_objectid);

	uint32_t lower = ~btrfs_crc32c(~0U, name, length);
	uint32_t higher = ~btrfs_crc32c(~0U, parent, sizeof(parent));
	return ((uint64_t)higher << 32U) | lower;
}

uint32_t btrfs_csum_type_size(uint32_t type)
{
	switch (type) {
	case BTRFS_CSUM_TYPE_CRC32: return 4U;
	case BTRFS_CSUM_TYPE_XXHASH: return 8U;
	case BTRFS_CSUM_TYPE_SHA256: return 32U;
	case BTRFS_CSUM_TYPE_BLAKE2: return 32U;
	default: return 0U;
	}
}

const char *btrfs_csum_type_name(uint32_t type)
{
	switch (type) {
	case BTRFS_CSUM_TYPE_CRC32: return "crc32c";
	case BTRFS_CSUM_TYPE_XXHASH: return "xxhash64";
	case BTRFS_CSUM_TYPE_SHA256: return "sha256";
	case BTRFS_CSUM_TYPE_BLAKE2: return "blake2b";
	default: return "unknown";
	}
}

const char *btrfs_compression_name(uint32_t compression)
{
	switch (compression) {
	case BTRFS_COMPRESS_NONE: return "uncompressed";
	case BTRFS_COMPRESS_ZLIB: return "zlib";
	case BTRFS_COMPRESS_LZO: return "lzo";
	case BTRFS_COMPRESS_ZSTD: return "zstd";
	default: return "unknown compression";
	}
}

const char *btrfs_status_name(btrfs_status_t status)
{
	switch (status) {
	case BTRFS_OK: return "ok";
	case BTRFS_ERR_INVALID: return "invalid argument";
	case BTRFS_ERR_NOMEM: return "out of memory";
	case BTRFS_ERR_IO: return "I/O error";
	case BTRFS_ERR_BAD_MAGIC: return "not a Btrfs filesystem";
	case BTRFS_ERR_CSUM: return "checksum mismatch";
	case BTRFS_ERR_CORRUPT: return "corrupt metadata";
	case BTRFS_ERR_TRUNCATED: return "device smaller than the filesystem";
	case BTRFS_ERR_NOT_FOUND: return "not found";
	case BTRFS_ERR_END: return "end of iteration";
	case BTRFS_ERR_NOT_DIRECTORY: return "not a directory";
	case BTRFS_ERR_IS_DIRECTORY: return "is a directory";
	case BTRFS_ERR_NAME_TOO_LONG: return "name too long";
	case BTRFS_ERR_UNSUPPORTED: return "not supported";
	case BTRFS_ERR_UNSUPPORTED_FEATURE: return "unsupported feature";
	case BTRFS_ERR_UNSUPPORTED_CSUM: return "unsupported checksum type";
	case BTRFS_ERR_UNSUPPORTED_PROFILE: return "unsupported RAID or multi-device profile";
	case BTRFS_ERR_UNSUPPORTED_COMPRESSION: return "unsupported compression";
	case BTRFS_ERR_UNSUPPORTED_ENCRYPTION: return "unsupported encryption";
	case BTRFS_ERR_LOG_TREE: return "unreplayed log tree";
	default: return "unknown";
	}
}
