#pragma once
/**
 * @file
 * @brief walb diff utiltities for files.
 * @author HOSHINO Takashi
 *
 * (C) 2013 Cybozu Labs, Inc.
 */
#include <cstdint>
#include "linux/walb/util.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WALB_DIFF_VERSION 2

/**
 * Sorted wdiff file format.
 *
 * [sizeof: walb_diff_file_header]
 * [[4KiB: walb_diff_pack, [walb_diff_record, ...]]
 *  [compressed IO data, ...], ...]
 * [4KiB: walb_diff_pack: end flag on]
 *
 * All IOs are sorted by address.
 * There is no overlap of IO range.
 */

/**
 * Indexed wdiff file format.
 *
 * [sizeof: walb_diff_file_header]
 * [compressed IO data, ...]
 * [padding data 0-7 bytes in order to align index records to 8 bytes]
 * [[sizeof: walb_indexed_diff_record], ...]
 * [sizeof: walb_diff_index_super: super block for the index]
 *
 * All uncompressed IO data size are aligned to 2^N (N >= 9).
 * Compressed ones are of course not.
 * IO data may not be sorted by address while index records must be sorted.
 */

/**
 * Walb diff format type.
 */
enum {
    WALB_DIFF_TYPE_SORTED = 0,
    WALB_DIFF_TYPE_INDEXED,
    WALB_DIFF_TYPE_MAX
};

/**
 * Walb diff flag bit indicators.
 *
 * ALLZERO and DISCARD is exclusive.
 */
enum {
    WALB_DIFF_FLAG_EXIST_SHIFT = 0,
    WALB_DIFF_FLAG_ALLZERO_SHIFT,
    WALB_DIFF_FLAG_DISCARD_SHIFT,
    WALB_DIFF_FLAGS_SHIFT_MAX,
};

#define WALB_DIFF_FLAG(name) (1U << WALB_DIFF_FLAG_ ## name ## _SHIFT)

/**
 * Walb diff compression type.
 */
enum {
    WALB_DIFF_CMPR_NONE = 0,
    WALB_DIFF_CMPR_GZIP,
    WALB_DIFF_CMPR_SNAPPY,
    WALB_DIFF_CMPR_LZMA,
    WALB_DIFF_CMPR_LZ4,
    WALB_DIFF_CMPR_ZSTD,
    WALB_DIFF_CMPR_MAX
};


/**
 * Walb diff file header.
 */
struct walb_diff_file_header
{
    uint32_t checksum;       /* header block checksum. salt is 0. */
    uint16_t version;        /* WalB diff version */
    uint8_t type;            /* WALB_DIFF_TYPE_XXX */
    uint8_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
    uint8_t uuid[UUID_SIZE]; /* Identifier of the target block device. */
} __attribute__((packed, aligned(8)));


/**
 * Walb diff metadata record for an IO.
 *
 * If the flags is 0, the record is invalid.
 */
struct walb_diff_record
{
    uint64_t io_address; /* [logical block] */
    uint32_t io_blocks; /* [logical block] */
    uint8_t flags; /* see WALB_DIFF_FLAG_XXX. */
    uint8_t compression_type; /* see WALB_DIFF_CMPR_XXX. */
    uint16_t reserved1;
    uint32_t data_offset; /* [byte] */
    uint32_t data_size; /* [byte] */
    uint32_t checksum; /* compressed data checksum with salt 0. */
    uint32_t reserved2;
} __attribute__((packed, aligned(8)));

/**
 * Flag bits of walb_diff_pack.flags.
 */
enum
{
    WALB_DIFF_PACK_END = 0,
};

/**
 * Walb record pack.
 * 4KB data.
 */
struct walb_diff_pack
{
    uint32_t checksum; /* pack block (4KiB) checksum. salt is 0. */
    uint16_t n_records;
    uint8_t flags;
    uint8_t reserved0;
    uint32_t total_size; /* [byte]. whole pack size is
                            WALB_DIFF_PACK_SIZE + total_size. */
    uint32_t reserved1;
    struct walb_diff_record record[0];
} __attribute__((packed, aligned(8)));

const size_t WALB_DIFF_PACK_SIZE = 4096; /* 4KiB */
const size_t MAX_N_RECORDS_IN_WALB_DIFF_PACK =
    (WALB_DIFF_PACK_SIZE - sizeof(struct walb_diff_pack)) / sizeof(struct walb_diff_record);
const size_t WALB_DIFF_PACK_MAX_SIZE = 32 * 1024 * 1024; /* 32MiB */


/**
 * WalB diff index record.
 * If the flags is 0, the record is invalid.
 */
struct walb_indexed_diff_record
{
    uint64_t io_address; /* [logical block] */

    uint32_t io_blocks; /* [logical block] */
    uint8_t flags; /* see WALB_DIFF_FLAG_XXX. */
    uint8_t compression_type; /* see WALB_DIFF_CMPR_XXX. */
    uint16_t reserved1;

    uint64_t data_offset; /* [byte] offset of the compressed image in the whole file. */

    uint32_t data_size; /* [byte] size of the compressed image. */
    uint32_t io_offset; /* [logical block]. offset in the decompressed image. */

    uint32_t orig_blocks; /* [logical block] size of the decompressed image. */
    uint32_t reserved2;

    uint32_t io_checksum; /* chcksum of the compressed image with salt 0. */
    uint32_t rec_checksum; /* self checksum. */
} __attribute__((packed, aligned(8)));


struct walb_diff_index_super
{
    uint64_t index_offset; /* [byte] in the whole file. */
    uint32_t n_records; /* number of index records. */
    uint32_t n_data;  /* number of compressed images. */
    uint32_t reserved1;
    uint32_t checksum; /* self checksum */
} __attribute__((packed, aligned(8)));


#ifdef __cplusplus
}
#endif
