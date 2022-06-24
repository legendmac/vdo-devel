// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright Red Hat
 */

#include "index-layout.h"

#include <linux/murmurhash3.h>

#include "buffer.h"
#include "compiler.h"
#include "config.h"
#include "logger.h"
#include "memory-alloc.h"
#include "numeric.h"
#include "open-chapter.h"
#include "random.h"
#include "time-utils.h"
#include "volume-index-ops.h"

/*
 * Overall layout of an index on disk:
 *
 * The layout is divided into a number of fixed-size regions, the sizes of
 * which are computed when the index is created. Every header and region
 * begins on 4K block boundary. Save regions are further sub-divided into
 * regions of their own.
 *
 * Each region has a kind and an instance number. Some kinds only have one
 * instance and therefore use RL_SOLE_INSTANCE (-1) as the instance number.
 * The RL_KIND_INDEX used to use instances to represent sub-indices; now,
 * however there is only ever one sub-index and therefore one instance. A save
 * region holds a clean shutdown. The instances determine which available save
 * slot is used. The RL_KIND_VOLUME_INDEX uses instances to record which zone
 * is being saved.
 *
 *     +-+-+--------+--------+--------+-----+---  -+-+
 *     | | |   I N D E X   0      101, 0    | ...  | |
 *     |H|C+--------+--------+--------+-----+---  -+S|
 *     |D|f| Volume | Save   | Save   |     |      |e|
 *     |R|g| Region | Region | Region | ... | ...  |a|
 *     | | | 201 -1 | 202  0 | 202  1 |     |      |l|
 *     +-+-+--------+--------+--------+-----+---  -+-+
 *
 * The header contains the encoded region layout table as well as the saved
 * index configuration record. The sub-index region and its subdivisions are
 * maintained in the same table.
 *
 * There are at least two save regions to preserve the old state should the
 * saving of a state be incomplete. They are used in a round-robin fashion.
 *
 * Anatomy of a save region:
 *
 *     +-+-----+------+------+-----+   -+-----+
 *     |H| IPM | MI   | MI   |     |    | OC  |
 *     |D|     | zone | zone | ... |    |     |
 *     |R| 301 | 302  | 302  |     |    | 303 |
 *     | | -1  | 0    | 1    |     |    | -1  |
 *     +-+-----+------+------+-----+   -+-----+
 *
 * Every region header has a type (and version).
 *
 * The header contains the encoded region layout table as well as the index
 * state record for that save. Each save has a unique generation number and
 * nonce which is used to seed the checksums of those regions.
 */
#ifdef TEST_INTERNAL

atomic_t saves_begun;
#endif /* TEST_INTERNAL */

enum {
	NONCE_INFO_SIZE = 32,
};

enum region_type {
	RH_TYPE_FREE = 0, /* unused */
	RH_TYPE_SUPER = 1,
	RH_TYPE_SAVE = 2,
	RH_TYPE_CHECKPOINT = 3, /* unused */
	RH_TYPE_UNSAVED = 4,
};

enum {
	RL_SOLE_INSTANCE = 65535,
};

static const uint64_t REGION_MAGIC = 0x416c6252676e3031; /* 'AlbRgn01' */

struct region_header {
	uint64_t magic;         /* REGION_MAGIC */
	uint64_t region_blocks; /* size of whole region */
	uint16_t type;          /* RH_TYPE_... */
	uint16_t version;       /* 1 */
	uint16_t num_regions;   /* number of layouts in the table */
	uint16_t payload;       /* extra data beyond region table */
};

struct layout_region {
	uint64_t start_block;
	uint64_t num_blocks;
	uint32_t checksum; /* only used for save regions */
	uint16_t kind;
	uint16_t instance;
};

struct region_table {
	struct region_header header;
	struct layout_region regions[];
};

struct index_save_data {
	uint64_t timestamp; /* ms since epoch... */
	uint64_t nonce;
	uint32_t version; /* 1 */
	uint32_t unused__;
};

enum index_save_type {
	IS_SAVE,
	NO_SAVE = 9999,
};

struct index_save_layout {
	struct layout_region index_save;
	struct layout_region header;
	unsigned int num_zones;
	struct layout_region index_page_map;
	struct layout_region free_space;
	struct layout_region *volume_index_zones;
	struct layout_region *open_chapter;
	enum index_save_type save_type;
	struct index_save_data save_data;
	struct buffer *index_state_buffer;
	bool read;
	bool written;
};

struct sub_index_layout {
	struct layout_region sub_index;
	uint64_t nonce;
	struct layout_region volume;
	struct index_save_layout *saves;
};

struct super_block_data {
	byte magic_label[32];
	byte nonce_info[NONCE_INFO_SIZE];
	uint64_t nonce;
	uint32_t version; /* 2 or 3 for normal, 7 for converted */
	uint32_t block_size; /* for verification */
	uint16_t num_indexes; /* always 1 */
	uint16_t max_saves;
	byte padding[4]; /* pad to 64 bit boundary */
	uint64_t open_chapter_blocks;
	uint64_t page_map_blocks;
	uint64_t volume_offset;
	uint64_t start_offset;
};

struct index_layout {
	struct io_factory *factory;
	size_t factory_size;
	off_t offset;
	struct super_block_data super;
	struct layout_region header;
	struct layout_region config;
	struct sub_index_layout index;
	struct layout_region seal;
	uint64_t total_blocks;
};

struct index_state_version {
	int32_t signature;
	int32_t version_id;
};

struct index_state_data301 {
	uint64_t newest_chapter;
	uint64_t oldest_chapter;
	uint64_t last_save;
	uint32_t unused;
	uint32_t padding;
};

static const struct index_state_version INDEX_STATE_VERSION_301 = {
	.signature  = -1,
	.version_id = 301,
};

/**
 * Structure used to compute single file layout sizes.
 *
 * Note that the volume_index_blocks represent all zones and are sized for
 * the maximum number of blocks that would be needed regardless of the number
 * of zones (up to the maximum value) that are used at run time.
 **/
struct save_layout_sizes {
	unsigned int num_saves; /* per sub-index */
	size_t block_size; /* in bytes */
	uint64_t volume_blocks; /* per sub-index */
	uint64_t volume_index_blocks; /* per save */
	uint64_t page_map_blocks; /* per save */
	uint64_t open_chapter_blocks; /* per save */
	uint64_t save_blocks; /* per sub-index */
	uint64_t sub_index_blocks; /* per sub-index */
	uint64_t total_blocks; /* for whole layout */
	size_t total_size; /* in bytes, for whole layout */
};

/*
 * Version 3 is the normal version used from RHEL8.2 onwards.
 *
 * Versions 4 through 6 were incremental development versions and are not
 * supported.
 *
 * Version 7 is used for volumes which have been reduced in size by one chapter
 * in order to make room to prepend LVM metadata to an existing VDO without
 * losing all deduplication.
 */
enum {
	SUPER_VERSION_MINIMUM = 3,
	SUPER_VERSION_CURRENT = 3,
	SUPER_VERSION_MAXIMUM = 7,
	INDEX_STATE_BUFFER_SIZE = 512,
	MAX_SAVES = 5,
};

static const byte SINGLE_FILE_MAGIC_1[32] = "*ALBIREO*SINGLE*FILE*LAYOUT*001*";
enum { SINGLE_FILE_MAGIC_1_LENGTH = sizeof(SINGLE_FILE_MAGIC_1) };

static INLINE bool is_converted_super_block(struct super_block_data *super)
{
	return (super->version == 7);
}

static INLINE uint64_t block_count(uint64_t bytes, uint32_t block_size)
{
	uint64_t blocks = bytes / block_size;

	if (bytes % block_size > 0) {
		++blocks;
	}
	return blocks;
}

static int __must_check compute_sizes(const struct configuration *config,
				      struct save_layout_sizes *sls)
{
	struct geometry *geometry = config->geometry;
	int result;

	if ((geometry->bytes_per_page % UDS_BLOCK_SIZE) != 0) {
		return uds_log_error_strerror(UDS_INCORRECT_ALIGNMENT,
					      "page size not a multiple of block size");
	}

	memset(sls, 0, sizeof(*sls));

	sls->num_saves = 2;
	sls->block_size = UDS_BLOCK_SIZE;
	sls->volume_blocks = geometry->bytes_per_volume / sls->block_size;

	result = compute_volume_index_save_blocks(config,
						  sls->block_size,
						  &sls->volume_index_blocks);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot compute index save size");
	}

	sls->page_map_blocks =
		block_count(compute_index_page_map_save_size(geometry),
			    sls->block_size);
	sls->open_chapter_blocks =
		block_count(compute_saved_open_chapter_size(geometry),
			    sls->block_size);
	sls->save_blocks =
		1 + (sls->volume_index_blocks + sls->page_map_blocks +
		     sls->open_chapter_blocks);
	sls->sub_index_blocks =
		sls->volume_blocks + (sls->num_saves * sls->save_blocks);
	sls->total_blocks = 3 + sls->sub_index_blocks;
	sls->total_size = sls->total_blocks * sls->block_size;

	return UDS_SUCCESS;
}

int uds_compute_index_size(const struct uds_parameters *parameters,
			   uint64_t *index_size)
{
	struct configuration *index_config;
	struct save_layout_sizes sizes;
	int result;

	if (index_size == NULL) {
		uds_log_error("Missing output size pointer");
		return -EINVAL;
	}

	result = make_configuration(parameters, &index_config);
	if (result != UDS_SUCCESS) {
		uds_log_error_strerror(result, "cannot compute index size");
		return uds_map_to_system_error(result);
	}

	result = compute_sizes(index_config, &sizes);
	free_configuration(index_config);
	if (result != UDS_SUCCESS) {
		return uds_map_to_system_error(result);
	}

	*index_size = sizes.total_size;
	return UDS_SUCCESS;
}

/**
 * Create NONCE_INFO_SIZE (32) bytes of unique data for generating a
 * nonce, using the current time and a pseudorandom number.
 *
 * @param buffer        Where to put the data
 **/
static void create_unique_nonce_data(byte *buffer)
{
	ktime_t now = current_time_ns(CLOCK_REALTIME);
	uint32_t rand = random_in_range(1, (1 << 30) - 1);
	size_t offset = 0;

	/*
	 * Fill NONCE_INFO_SIZE bytes with copies of the time and a
	 * pseudorandom number.
	 */
	memcpy(buffer + offset, &now, sizeof(now));
	offset += sizeof(now);
	memcpy(buffer + offset, &rand, sizeof(rand));
	offset += sizeof(rand);
	while (offset < NONCE_INFO_SIZE) {
		size_t len = min(NONCE_INFO_SIZE - offset, offset);

		memcpy(buffer + offset, buffer, len);
		offset += len;
	}
}

static uint64_t hash_stuff(uint64_t start, const void *data, size_t len)
{
	uint32_t seed = start ^ (start >> 27);
	byte hash_buffer[16];

	murmurhash3_128(data, len, seed, hash_buffer);
	return get_unaligned_le64(hash_buffer + 4);
}

/**
 * Generate a primary nonce, using the specified data.
 *
 * @param data          Some arbitrary information.
 * @param len           The length of the information.
 *
 * @return a number which will be fairly unique
 **/
static uint64_t generate_primary_nonce(const void *data, size_t len)
{
	return hash_stuff(0xa1b1e0fc, data, len);
}

/**
 * Deterministically generate a secondary nonce based on an existing
 * nonce and some arbitrary data. Effectively hashes the nonce and
 * the data to produce a new nonce which is deterministic.
 *
 * @param nonce         An existing nonce which is well known.
 * @param data          Some data related to the creation of this nonce.
 * @param len           The length of the data.
 *
 * @return a number which will be fairly unique and depend solely on
 *      the nonce and the data.
 **/
static uint64_t
generate_secondary_nonce(uint64_t nonce, const void *data, size_t len)
{
	return hash_stuff(nonce + 1, data, len);
}

static int __must_check
open_layout_reader(struct index_layout *layout,
		   struct layout_region *lr,
		   off_t offset,
		   struct buffered_reader **reader_ptr)
{
	off_t start = (lr->start_block + offset) * layout->super.block_size;
	size_t size = lr->num_blocks * layout->super.block_size;

	return open_uds_buffered_reader(layout->factory, start, size,
					reader_ptr);
}

static int __must_check
open_layout_writer(struct index_layout *layout,
		   struct layout_region *lr,
		   off_t offset,
		   struct buffered_writer **writer_ptr)
{
	off_t start = (lr->start_block + offset) * layout->super.block_size;
	size_t size = lr->num_blocks * layout->super.block_size;

	return open_uds_buffered_writer(layout->factory, start, size,
					writer_ptr);
}

static int __must_check
decode_index_save_data(struct buffer *buffer,
		       struct index_save_data *save_data)
{
	int result = get_uint64_le_from_buffer(buffer, &save_data->timestamp);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &save_data->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &save_data->version);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = skip_forward(buffer, sizeof(uint32_t));
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
				 "%zu bytes decoded of %zu expected",
				 buffer_length(buffer),
				 sizeof(*save_data));
	if (result != UDS_SUCCESS) {
		return UDS_CORRUPT_DATA;
	}
	return result;
}

static int __must_check
decode_region_header(struct buffer *buffer, struct region_header *header)
{
	int result = get_uint64_le_from_buffer(buffer, &header->magic);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &header->region_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint16_le_from_buffer(buffer, &header->type);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint16_le_from_buffer(buffer, &header->version);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint16_le_from_buffer(buffer, &header->num_regions);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint16_le_from_buffer(buffer, &header->payload);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
				 "%zu bytes decoded of %zu expected",
				 buffer_length(buffer),
				 sizeof(*header));
	if (result != UDS_SUCCESS) {
		return UDS_CORRUPT_DATA;
	}
	return result;
}

static int __must_check
decode_layout_region(struct buffer *buffer, struct layout_region *region)
{
	size_t cl1 = content_length(buffer);

	int result = get_uint64_le_from_buffer(buffer, &region->start_block);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &region->num_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &region->checksum);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint16_le_from_buffer(buffer, &region->kind);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint16_le_from_buffer(buffer, &region->instance);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(cl1 - content_length(buffer) == sizeof(*region),
				 "%zu bytes decoded, of %zu expected",
				 cl1 - content_length(buffer),
				 sizeof(*region));
	if (result != UDS_SUCCESS) {
		return UDS_CORRUPT_DATA;
	}
	return result;
}

static int __must_check load_region_table(struct buffered_reader *reader,
					  struct region_table **table_ptr)
{
	unsigned int i;
	struct region_header header;
	struct region_table *table;
	struct buffer *buffer;

	int result = make_buffer(sizeof(struct region_header), &buffer);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return uds_log_error_strerror(result,
					      "cannot read region table header");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = decode_region_header(buffer, &header);
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (header.magic != REGION_MAGIC) {
		return UDS_NO_INDEX;
	}

	if (header.version != 1) {
		return uds_log_error_strerror(UDS_UNSUPPORTED_VERSION,
					      "unknown region table version %hu",
					      header.version);
	}

	result = UDS_ALLOCATE_EXTENDED(struct region_table,
				       header.num_regions,
				       struct layout_region,
				       "single file layout region table",
				       &table);
	if (result != UDS_SUCCESS) {
		return result;
	}

	table->header = header;
	result = make_buffer(header.num_regions * sizeof(struct layout_region),
			     &buffer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return result;
	}

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		free_buffer(UDS_FORGET(buffer));
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "cannot read region table layouts");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	for (i = 0; i < header.num_regions; i++) {
		result = decode_layout_region(buffer, &table->regions[i]);
		if (result != UDS_SUCCESS) {
			UDS_FREE(table);
			free_buffer(UDS_FORGET(buffer));
			return result;
		}
	}

	free_buffer(UDS_FORGET(buffer));
	*table_ptr = table;
	return UDS_SUCCESS;
}

static int __must_check
decode_super_block_data(struct buffer *buffer, struct super_block_data *super)
{
	int result = get_bytes_from_buffer(buffer, 32, super->magic_label);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_bytes_from_buffer(buffer, NONCE_INFO_SIZE,
				       super->nonce_info);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &super->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &super->version);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint32_le_from_buffer(buffer, &super->block_size);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint16_le_from_buffer(buffer, &super->num_indexes);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint16_le_from_buffer(buffer, &super->max_saves);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = skip_forward(buffer, 4); /* aligment */
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &super->open_chapter_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &super->page_map_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if (is_converted_super_block(super)) {
		result = get_uint64_le_from_buffer(buffer,
						   &super->volume_offset);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = get_uint64_le_from_buffer(buffer,
						   &super->start_offset);
		if (result != UDS_SUCCESS) {
			return result;
		}
	} else {
		super->volume_offset = 0;
		super->start_offset = 0;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
				 "%zu bytes decoded of %zu expected",
				 buffer_length(buffer),
				 sizeof(*super));
	if (result != UDS_SUCCESS) {
		return UDS_CORRUPT_DATA;
	}
	return result;
}

static int __must_check read_super_block_data(struct buffered_reader *reader,
					      struct index_layout *layout,
					      size_t saved_size)
{
	struct super_block_data *super = &layout->super;
	struct buffer *buffer;
	int result;

	if (sizeof(super->magic_label) != SINGLE_FILE_MAGIC_1_LENGTH) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "super block magic label size incorrect");
	}

	result = make_buffer(saved_size, &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_from_buffered_reader(reader,
					   get_buffer_contents(buffer),
					   buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return uds_log_error_strerror(result,
					      "cannot read region table header");
	}

	result = reset_buffer_end(buffer, buffer_length(buffer));
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = decode_super_block_data(buffer, super);
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot read super block data");
	}

	if (memcmp(super->magic_label,
		   SINGLE_FILE_MAGIC_1,
		   SINGLE_FILE_MAGIC_1_LENGTH) != 0) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "unknown superblock magic label");
	}

	if ((super->version < SUPER_VERSION_MINIMUM) ||
	    (super->version == 4) ||
	    (super->version == 5) ||
	    (super->version == 6) ||
	    (super->version > SUPER_VERSION_MAXIMUM)) {
		return uds_log_error_strerror(UDS_UNSUPPORTED_VERSION,
					      "unknown superblock version number %u",
					      super->version);
	}

	if (super->volume_offset < super->start_offset) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "inconsistent offsets (start %llu, volume %llu)",
					      (unsigned long long) super->start_offset,
					      (unsigned long long) super->volume_offset);
	}

	/*
	 * We dropped the usage of multiple subindices before we ever ran UDS
	 * code in the kernel.  We do not have code that will handle multiple
	 * subindices.
	 */
	if (super->num_indexes != 1) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "invalid subindex count %u",
					      super->num_indexes);
	}

	if (generate_primary_nonce(super->nonce_info,
				   sizeof(super->nonce_info)) != super->nonce) {
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "inconsistent superblock nonce");
	}

	return UDS_SUCCESS;
}

static int __must_check
allocate_single_file_parts(struct index_layout *layout)
{
	int result = UDS_ALLOCATE(layout->super.max_saves,
				  struct index_save_layout,
				  __func__,
				  &layout->index.saves);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_SUCCESS;
}

static void define_sub_index_nonce(struct index_layout *layout,
				   unsigned int index_id)
{
	struct sub_index_nonce_data {
		uint64_t offset;
		uint16_t index_id;
	};

	struct sub_index_layout *sil = &layout->index;
	uint64_t primary_nonce = layout->super.nonce;
	byte buffer[sizeof(struct sub_index_nonce_data)] = { 0 };
	size_t offset = 0;

	encode_uint64_le(buffer, &offset, sil->sub_index.start_block);
	encode_uint16_le(buffer, &offset, index_id);
	sil->nonce = generate_secondary_nonce(primary_nonce,
					      buffer,
					      sizeof(buffer));
	if (sil->nonce == 0) {
		sil->nonce = generate_secondary_nonce(~primary_nonce + 1,
						      buffer,
						      sizeof(buffer));
	}
}

struct region_iterator {
	struct layout_region *next_region;
	struct layout_region *last_region;
	uint64_t next_block;
	int result;
};

__printf(2, 3)
static void iter_error(struct region_iterator *iter, const char *fmt, ...)
{
	int r;
	va_list args;

	va_start(args, fmt);
	r = uds_vlog_strerror(UDS_LOG_ERR, UDS_UNEXPECTED_RESULT, NULL, fmt, args);
	va_end(args);
	if (iter->result == UDS_SUCCESS) {
		iter->result = r;
	}
}

/**
 * Set the next layout region in the layout according to a region table
 * iterator, unless the iterator already contains an error
 *
 * @param expect        whether to record an error or return false
 * @param lr            the layout region field to set
 * @param iter          the region iterator, which also holds the cumulative
 *                        result
 * @param num_blocks     if non-zero, the expected number of blocks
 * @param kind          the expected kind of the region
 * @param instance      the expected instance number of the region
 *
 * @return true if we meet expectations, false if we do not
 **/
static bool expect_layout(bool expect,
			  struct layout_region *lr,
			  struct region_iterator *iter,
			  uint64_t num_blocks,
			  enum region_kind kind,
			  unsigned int instance)
{
	if (iter->result != UDS_SUCCESS) {
		return false;
	}

	if (iter->next_region == iter->last_region) {
		if (expect) {
			iter_error(iter,
				   "ran out of layout regions in region table");
		}
		return false;
	}

	if (iter->next_region->start_block != iter->next_block) {
		iter_error(iter, "layout region not at expected offset");
		return false;
	}

	if (iter->next_region->kind != kind) {
		if (expect) {
			iter_error(iter, "layout region has incorrect kind");
		}
		return false;
	}

	if (iter->next_region->instance != instance) {
		iter_error(iter, "layout region has incorrect instance");
		return false;
	}

	if (num_blocks > 0 && iter->next_region->num_blocks != num_blocks) {
		iter_error(iter, "layout region size is incorrect");
		return false;
	}

	if (lr != NULL) {
		*lr = *iter->next_region;
	}

	iter->next_block += iter->next_region->num_blocks;
	iter->next_region++;
	return true;
}

static void expect_sub_index(struct index_layout *layout,
			     struct region_iterator *iter,
			     unsigned int instance)
{
	unsigned int i;
	struct sub_index_layout *sil = &layout->index;
	uint64_t start_block = iter->next_block;
	uint64_t end_block;

	if (iter->result != UDS_SUCCESS) {
		return;
	}

	expect_layout(true, &sil->sub_index, iter, 0, RL_KIND_INDEX, instance);

	end_block = iter->next_block;
	iter->next_block = start_block;

	expect_layout(true, &sil->volume, iter, 0, RL_KIND_VOLUME,
		      RL_SOLE_INSTANCE);

	iter->next_block += layout->super.volume_offset;
	end_block += layout->super.volume_offset;

	for (i = 0; i < layout->super.max_saves; ++i) {
		struct index_save_layout *isl = &sil->saves[i];

		expect_layout(true, &isl->index_save, iter, 0, RL_KIND_SAVE, i);
	}

	if (iter->next_block != end_block) {
		iter_error(iter, "sub index region does not span all saves");
	}

	define_sub_index_nonce(layout, instance);
}


/**
 * Initialize a single file layout from the region table and super block data
 * stored in stable storage.
 *
 * @param layout       the layout to initialize
 * @param table        the region table read from the superblock
 * @param first_block  the first block number in the region
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check
reconstitute_single_file_layout(struct index_layout *layout,
				struct region_table *table,
				uint64_t first_block)
{
	struct region_iterator iter = {
		.next_region = table->regions,
		.last_region = table->regions + table->header.num_regions,
		.next_block = first_block,
		.result = UDS_SUCCESS
	};

	layout->total_blocks = table->header.region_blocks;

	expect_layout(true,
		      &layout->header,
		      &iter,
		      1,
		      RL_KIND_HEADER,
		      RL_SOLE_INSTANCE);
	expect_layout(true,
		      &layout->config,
		      &iter,
		      1,
		      RL_KIND_CONFIG,
		      RL_SOLE_INSTANCE);
	expect_sub_index(layout, &iter, 0);
	expect_layout(true, &layout->seal, &iter, 1, RL_KIND_SEAL,
		      RL_SOLE_INSTANCE);

	if (iter.result != UDS_SUCCESS) {
		return iter.result;
	}

	if ((iter.next_block - layout->super.volume_offset) !=
	    (first_block + layout->total_blocks)) {
		return uds_log_error_strerror(UDS_UNEXPECTED_RESULT,
					      "layout table does not span total blocks");
	}
	return UDS_SUCCESS;
}

static int __must_check
load_super_block(struct index_layout *layout,
		 size_t block_size,
		 uint64_t first_block,
		 struct buffered_reader *reader)
{
	struct region_table *table = NULL;
	struct super_block_data *super = &layout->super;
	int result = load_region_table(reader, &table);

	if (result != UDS_SUCCESS) {
		return result;
	}

	if (table->header.type != RH_TYPE_SUPER) {
		UDS_FREE(table);
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "not a superblock region table");
	}

	result = read_super_block_data(reader, layout, table->header.payload);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return uds_log_error_strerror(result,
					      "unknown superblock format");
	}

	if (super->block_size != block_size) {
		UDS_FREE(table);
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "superblock saved block_size %u differs from supplied block_size %zu",
					      super->block_size,
					      block_size);
	}

	result = allocate_single_file_parts(layout);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return result;
	}

	first_block -= (super->volume_offset - super->start_offset);
	result = reconstitute_single_file_layout(layout, table, first_block);
	UDS_FREE(table);
	return result;
}

static int __must_check
read_index_save_data(struct buffered_reader *reader,
		     struct index_save_data *save_data,
		     size_t saved_size,
		     struct buffer **buffer_ptr)
{
	struct buffer *buffer = NULL;
	int result = UDS_SUCCESS;

	if (saved_size == 0) {
		memset(save_data, 0, sizeof(*save_data));
	} else {
		if (saved_size < sizeof(struct index_save_data)) {
			return uds_log_error_strerror(UDS_CORRUPT_DATA,
						      "unexpected index save data size %zu",
						      saved_size);
		}

		result = make_buffer(sizeof(*save_data), &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}

		result = read_from_buffered_reader(reader,
						   get_buffer_contents(buffer),
						   buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return uds_log_error_strerror(result,
						      "cannot read index save data");
		}

		result = reset_buffer_end(buffer, buffer_length(buffer));
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}

		result = decode_index_save_data(buffer, save_data);
		free_buffer(UDS_FORGET(buffer));
		if (result != UDS_SUCCESS) {
			return result;
		}

		saved_size -= sizeof(struct index_save_data);

		if (save_data->version > 1) {
			return uds_log_error_strerror(UDS_UNSUPPORTED_VERSION,
						      "unknown index save version number %u",
						      save_data->version);
		}

		if (saved_size > INDEX_STATE_BUFFER_SIZE) {
			return uds_log_error_strerror(UDS_CORRUPT_DATA,
						      "unexpected index state buffer size %zu",
						      saved_size);
		}
	}

	if (save_data->version != 0) {
		result = make_buffer(INDEX_STATE_BUFFER_SIZE, &buffer);
		if (result != UDS_SUCCESS) {
			return result;
		}

		if (saved_size > 0) {
			result = read_from_buffered_reader(reader,
							   get_buffer_contents(buffer),
							   saved_size);
			if (result != UDS_SUCCESS) {
				free_buffer(UDS_FORGET(buffer));
				return result;
			}

			result = reset_buffer_end(buffer, saved_size);
			if (result != UDS_SUCCESS) {
				free_buffer(UDS_FORGET(buffer));
				return result;
			}
		}
	}

	*buffer_ptr = buffer;
	return UDS_SUCCESS;
}

static void setup_layout(struct layout_region *lr,
			 uint64_t *next_addr_ptr,
			 uint64_t region_size,
			 unsigned int kind,
			 unsigned int instance)
{
	*lr = (struct layout_region){
		.start_block = *next_addr_ptr,
		.num_blocks = region_size,
		.checksum = 0,
		.kind = kind,
		.instance = instance,
	};
	*next_addr_ptr += region_size;
}

static void populate_index_save_layout(struct index_save_layout *isl,
				       struct super_block_data *super,
				       unsigned int num_zones,
				       enum index_save_type save_type)
{
	uint64_t blocks_avail;
	uint64_t next_block = isl->index_save.start_block;

	setup_layout(&isl->header, &next_block, 1, RL_KIND_HEADER,
		     RL_SOLE_INSTANCE);
	setup_layout(&isl->index_page_map,
		     &next_block,
		     super->page_map_blocks,
		     RL_KIND_INDEX_PAGE_MAP,
		     RL_SOLE_INSTANCE);

	blocks_avail = (isl->index_save.num_blocks -
			(next_block - isl->index_save.start_block) -
			super->open_chapter_blocks);

	if (num_zones > 0) {
		uint64_t mi_block_count = blocks_avail / num_zones;
		unsigned int z;

		for (z = 0; z < num_zones; ++z) {
			struct layout_region *miz = &isl->volume_index_zones[z];

			setup_layout(miz,
				     &next_block,
				     mi_block_count,
				     RL_KIND_VOLUME_INDEX,
				     z);
		}
	}
	if (save_type == IS_SAVE && isl->open_chapter != NULL) {
		setup_layout(isl->open_chapter,
			     &next_block,
			     super->open_chapter_blocks,
			     RL_KIND_OPEN_CHAPTER,
			     RL_SOLE_INSTANCE);
	}
	setup_layout(&isl->free_space,
		     &next_block,
		     (isl->index_save.num_blocks -
		      (next_block - isl->index_save.start_block)),
		     RL_KIND_SCRATCH,
		     RL_SOLE_INSTANCE);
}

static int __must_check
reconstruct_index_save(struct index_save_layout *isl,
		       struct index_save_data *save_data,
		       struct super_block_data *super,
		       struct region_table *table)
{
	int result = UDS_SUCCESS;
	unsigned int z, n = 0;
	struct region_iterator iter, tmp_iter;

	isl->num_zones = 0;
	isl->save_data = *save_data;
	isl->read = false;
	isl->written = false;

	if (table->header.type == RH_TYPE_SAVE) {
		isl->save_type = IS_SAVE;
	} else {
		isl->save_type = NO_SAVE;
	}

	if ((table->header.num_regions == 0) ||
	    ((table->header.num_regions == 1) &&
	     (table->regions[0].kind == RL_KIND_SCRATCH))) {
		populate_index_save_layout(isl, super, 0, NO_SAVE);
		return UDS_SUCCESS;
	}

	iter = (struct region_iterator) {
		.next_region = table->regions,
		.last_region = table->regions + table->header.num_regions,
		.next_block = isl->index_save.start_block,
		.result = UDS_SUCCESS,
	};

	expect_layout(true,
		      &isl->header,
		      &iter,
		      1,
		      RL_KIND_HEADER,
		      RL_SOLE_INSTANCE);
	expect_layout(true,
		      &isl->index_page_map,
		      &iter,
		      0,
		      RL_KIND_INDEX_PAGE_MAP,
		      RL_SOLE_INSTANCE);
	for (tmp_iter = iter;
	     expect_layout(false, NULL, &tmp_iter, 0, RL_KIND_VOLUME_INDEX, n);
	     ++n)
		;
	isl->num_zones = n;

	if (isl->num_zones > 0) {
		result = UDS_ALLOCATE(n,
				      struct layout_region,
				      "volume index layout regions",
				      &isl->volume_index_zones);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	if (isl->save_type == IS_SAVE) {
		result = UDS_ALLOCATE(1,
				      struct layout_region,
				      "open chapter layout region",
				      &isl->open_chapter);
		if (result != UDS_SUCCESS) {
			UDS_FREE(isl->volume_index_zones);
			return result;
		}
	}

	for (z = 0; z < isl->num_zones; ++z) {
		expect_layout(true,
			      &isl->volume_index_zones[z],
			      &iter,
			      0,
			      RL_KIND_VOLUME_INDEX,
			      z);
	}
	if (isl->save_type == IS_SAVE) {
		expect_layout(true,
			      isl->open_chapter,
			      &iter,
			      0,
			      RL_KIND_OPEN_CHAPTER,
			      RL_SOLE_INSTANCE);
	}
	if (!expect_layout(false,
			   &isl->free_space,
			   &iter,
			   0,
			   RL_KIND_SCRATCH,
			   RL_SOLE_INSTANCE)) {
		isl->free_space = (struct layout_region){
			.start_block = iter.next_block,
			.num_blocks = (isl->index_save.start_block +
				       isl->index_save.num_blocks) -
				      iter.next_block,
			.checksum = 0,
			.kind = RL_KIND_SCRATCH,
			.instance = RL_SOLE_INSTANCE,
		};
		iter.next_block = isl->free_space.start_block +
				  isl->free_space.num_blocks;
	}

	if (iter.result != UDS_SUCCESS) {
		return iter.result;
	}
	if (iter.next_region != iter.last_region) {
		return uds_log_error_strerror(UDS_UNEXPECTED_RESULT,
					      "expected %ld additional regions",
					      iter.last_region - iter.next_region);
	}
	if (iter.next_block !=
	    isl->index_save.start_block + isl->index_save.num_blocks) {
		return uds_log_error_strerror(UDS_UNEXPECTED_RESULT,
					      "index save layout table incomplete");
	}

	return UDS_SUCCESS;
}

static int __must_check load_index_save(struct index_save_layout *isl,
					struct super_block_data *super,
					struct buffered_reader *reader,
					unsigned int save_id)
{
	struct index_save_data index_data;
	struct region_table *table = NULL;
	int result = load_region_table(reader, &table);

	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot read index 0 save %u header",
					      save_id);
	}

	if (table->header.region_blocks != isl->index_save.num_blocks) {
		uint64_t region_blocks = table->header.region_blocks;

		UDS_FREE(table);
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "unexpected index 0 save %u "
					      "region block count %llu",
					      save_id,
					      (unsigned long long) region_blocks);
	}

	if (table->header.type != RH_TYPE_SAVE &&
	    table->header.type != RH_TYPE_UNSAVED) {
		unsigned int type = table->header.type;

		UDS_FREE(table);
		return uds_log_error_strerror(UDS_CORRUPT_DATA,
					      "unexpected index 0 save %u header type %u",
					      save_id,
					      type);
	}

	result = read_index_save_data(reader,
				      &index_data,
				      table->header.payload,
				      &isl->index_state_buffer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return uds_log_error_strerror(result,
					      "unknown index 0 save %u data format",
					      save_id);
	}

	result = reconstruct_index_save(isl, &index_data, super, table);
	UDS_FREE(table);

	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(isl->index_state_buffer));
		return uds_log_error_strerror(result,
					      "cannot reconstruct index 0 save %u",
					      save_id);
	}

	isl->read = true;
	return UDS_SUCCESS;
}

static int __must_check load_sub_index_regions(struct index_layout *layout)
{
	unsigned int j;

	for (j = 0; j < layout->super.max_saves; ++j) {
		struct index_save_layout *isl = &layout->index.saves[j];

		struct buffered_reader *reader;
		int result =
			open_layout_reader(layout,
					   &isl->index_save,
					   -layout->super.start_offset,
					   &reader);

		if (result != UDS_SUCCESS) {
			uds_log_error_strerror(result,
					       "cannot get reader for index 0 save %u",
					       j);
			while (j-- > 0) {
				struct index_save_layout *isl =
					&layout->index.saves[j];
				UDS_FREE(isl->volume_index_zones);
				UDS_FREE(isl->open_chapter);
				free_buffer(UDS_FORGET(isl->index_state_buffer));
			}

			return result;
		}

		result = load_index_save(isl, &layout->super, reader, j);
		free_buffered_reader(reader);
		if (result != UDS_SUCCESS) {
			while (j-- > 0) {
				struct index_save_layout *isl =
					&layout->index.saves[j];
				UDS_FREE(isl->volume_index_zones);
				UDS_FREE(isl->open_chapter);
				free_buffer(UDS_FORGET(isl->index_state_buffer));
			}
			return result;
		}
	}

	return UDS_SUCCESS;
}

/**
 * Read the index configuration, and verify that it matches the given
 * configuration.
 *
 * @param layout  the generic index layout
 * @param config  the index configuration
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check
verify_uds_index_config(struct index_layout *layout,
			struct configuration *config)
{
	struct buffered_reader *reader = NULL;
	uint64_t offset = layout->super.volume_offset -
		layout->super.start_offset;
	int result = open_layout_reader(layout,
					&layout->config,
					offset,
					&reader);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "failed to open config reader");
	}

	result = validate_config_contents(reader, config);
	if (result != UDS_SUCCESS) {
		free_buffered_reader(reader);
		return uds_log_error_strerror(result,
					      "failed to read config region");
	}
	free_buffered_reader(reader);
	return UDS_SUCCESS;
}

static int load_index_layout(struct index_layout *layout,
			     struct configuration *config)
{
	struct buffered_reader *reader;
	int result = open_uds_buffered_reader(layout->factory, layout->offset,
					      UDS_BLOCK_SIZE, &reader);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "unable to read superblock");
	}

	result = load_super_block(layout,
				  UDS_BLOCK_SIZE,
				  layout->offset / UDS_BLOCK_SIZE,
				  reader);
	free_buffered_reader(reader);
	if (result != UDS_SUCCESS) {
		UDS_FREE(layout->index.saves);
		layout->index.saves = NULL;
		return result;
	}

	result = load_sub_index_regions(layout);
	if (result != UDS_SUCCESS) {
		UDS_FREE(layout->index.saves);
		layout->index.saves = NULL;
		return result;
	}

	return verify_uds_index_config(layout, config);
}

static void generate_super_block_data(size_t block_size,
				      unsigned int max_saves,
				      uint64_t open_chapter_blocks,
				      uint64_t page_map_blocks,
				      struct super_block_data *super)
{
	memset(super, 0, sizeof(*super));
	memcpy(super->magic_label,
	       SINGLE_FILE_MAGIC_1,
	       SINGLE_FILE_MAGIC_1_LENGTH);
	create_unique_nonce_data(super->nonce_info);

	super->nonce = generate_primary_nonce(super->nonce_info,
					      sizeof(super->nonce_info));
	super->version = SUPER_VERSION_CURRENT;
	super->block_size = block_size;
	super->num_indexes = 1;
	super->max_saves = max_saves;
	super->open_chapter_blocks = open_chapter_blocks;
	super->page_map_blocks = page_map_blocks;
	super->volume_offset = 0;
	super->start_offset = 0;
}

static int __must_check
reset_index_save_layout(struct index_save_layout *isl,
			uint64_t *next_block_ptr,
			uint64_t save_blocks,
			uint64_t page_map_blocks,
			unsigned int instance)
{
	uint64_t remaining;
	uint64_t start_block = *next_block_ptr;

	if (isl->volume_index_zones) {
		UDS_FREE(isl->volume_index_zones);
	}

	if (isl->open_chapter) {
		UDS_FREE(isl->open_chapter);
	}

	if (isl->index_state_buffer) {
		free_buffer(UDS_FORGET(isl->index_state_buffer));
	}

	memset(isl, 0, sizeof(*isl));
	isl->save_type = NO_SAVE;
	setup_layout(&isl->index_save,
		     &start_block,
		     save_blocks,
		     RL_KIND_SAVE,
		     instance);
	setup_layout(&isl->header,
		     next_block_ptr,
		     1,
		     RL_KIND_HEADER,
		     RL_SOLE_INSTANCE);
	setup_layout(&isl->index_page_map,
		     next_block_ptr,
		     page_map_blocks,
		     RL_KIND_INDEX_PAGE_MAP,
		     RL_SOLE_INSTANCE);
	remaining = start_block - *next_block_ptr;
	setup_layout(&isl->free_space,
		     next_block_ptr,
		     remaining,
		     RL_KIND_SCRATCH,
		     RL_SOLE_INSTANCE);
	/*
	 * number of zones is a save-time parameter
	 * presence of open chapter is a save-time parameter
	 */
	return UDS_SUCCESS;
}

static int __must_check setup_sub_index(struct index_layout *layout,
					uint64_t *next_block_ptr,
					struct save_layout_sizes *sls,
					unsigned int instance)
{
	struct sub_index_layout *sil = &layout->index;
	uint64_t start_block = *next_block_ptr;
	unsigned int i;

	setup_layout(&sil->sub_index,
		     &start_block,
		     sls->sub_index_blocks,
		     RL_KIND_INDEX,
		     instance);
	setup_layout(&sil->volume,
		     next_block_ptr,
		     sls->volume_blocks,
		     RL_KIND_VOLUME,
		     RL_SOLE_INSTANCE);
	for (i = 0; i < sls->num_saves; ++i) {
		int result = reset_index_save_layout(&sil->saves[i],
						     next_block_ptr,
						     sls->save_blocks,
						     sls->page_map_blocks,
						     i);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	if (start_block != *next_block_ptr) {
		return uds_log_error_strerror(UDS_UNEXPECTED_RESULT,
					      "sub index layout regions don't agree");
	}

	define_sub_index_nonce(layout, instance);
	return UDS_SUCCESS;
}

/**
 * Initialize a single file layout using the save layout sizes specified.
 *
 * @param layout  the layout to initialize
 * @param sls     a populated struct save_layout_sizes object
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check
init_single_file_layout(struct index_layout *layout,
			struct save_layout_sizes *sls)
{
	uint64_t next_block;
	int result;

	layout->total_blocks = sls->total_blocks;
	generate_super_block_data(sls->block_size,
				  sls->num_saves,
				  sls->open_chapter_blocks,
				  sls->page_map_blocks,
				  &layout->super);

	result = allocate_single_file_parts(layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	next_block = layout->offset / sls->block_size;

	setup_layout(&layout->header,
		     &next_block,
		     1,
		     RL_KIND_HEADER,
		     RL_SOLE_INSTANCE);
	setup_layout(&layout->config,
		     &next_block,
		     1,
		     RL_KIND_CONFIG,
		     RL_SOLE_INSTANCE);
	result = setup_sub_index(layout, &next_block, sls, 0);

	if (result != UDS_SUCCESS) {
		return result;
	}
	setup_layout(&layout->seal, &next_block, 1, RL_KIND_SEAL,
		     RL_SOLE_INSTANCE);
	if (next_block * sls->block_size > layout->offset + sls->total_size) {
		return uds_log_error_strerror(UDS_UNEXPECTED_RESULT,
					      "layout does not fit as expected");
	}
	return UDS_SUCCESS;
}

static int __must_check
make_single_file_region_table(struct index_layout *layout,
			      unsigned int *num_regions_ptr,
			      struct region_table **table_ptr)
{
	unsigned int num_regions = 1 + /* header */
				   1 + /* config */
				   1 + /* index */
				   1 + /* volume */
				   layout->super.max_saves + /* saves */
				   1; /* seal */

	struct region_table *table;
	struct sub_index_layout *sil;
	unsigned int j;
	struct layout_region *lr;
	int result = UDS_ALLOCATE_EXTENDED(struct region_table,
					   num_regions,
					   struct layout_region,
					   "layout region table",
					   &table);
	if (result != UDS_SUCCESS) {
		return result;
	}

	lr = &table->regions[0];
	*lr++ = layout->header;
	*lr++ = layout->config;
	sil = &layout->index;
	*lr++ = sil->sub_index;
	*lr++ = sil->volume;

	for (j = 0; j < layout->super.max_saves; ++j) {
		*lr++ = sil->saves[j].index_save;
	}
	*lr++ = layout->seal;

	result = ASSERT((lr == &table->regions[num_regions]),
			"incorrect number of regions");
	if (result != UDS_SUCCESS) {
		return result;
	}

	*num_regions_ptr = num_regions;
	*table_ptr = table;
	return UDS_SUCCESS;
}

static int __must_check
encode_index_save_data(struct buffer *buffer,
		       struct index_save_data *save_data)
{
	int result = put_uint64_le_into_buffer(buffer, save_data->timestamp);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, save_data->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, save_data->version);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = zero_bytes(buffer, sizeof(uint32_t)); /* padding */
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) == sizeof *save_data,
				 "%zu bytes encoded of %zu expected",
				 content_length(buffer),
				 sizeof(*save_data));
	return result;
}

static int __must_check
encode_region_header(struct buffer *buffer, struct region_header *header)
{
	size_t starting_length = content_length(buffer);
	int result = put_uint64_le_into_buffer(buffer, REGION_MAGIC);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, header->region_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint16_le_into_buffer(buffer, header->type);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint16_le_into_buffer(buffer, header->version);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint16_le_into_buffer(buffer, header->num_regions);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint16_le_into_buffer(buffer, header->payload);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) - starting_length ==
				 sizeof(*header),
				 "%zu bytes encoded, of %zu expected",
				 content_length(buffer) - starting_length,
				 sizeof(*header));
	return result;
}

static int __must_check
encode_layout_region(struct buffer *buffer, struct layout_region *region)
{
	size_t starting_length = content_length(buffer);
	int result = put_uint64_le_into_buffer(buffer, region->start_block);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, region->num_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, region->checksum);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint16_le_into_buffer(buffer, region->kind);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint16_le_into_buffer(buffer, region->instance);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) - starting_length ==
				 sizeof(*region),
				 "%zu bytes encoded, of %zu expected",
				 content_length(buffer) - starting_length,
				 sizeof(*region));
	return result;
}

static int __must_check encode_super_block_data(struct buffer *buffer,
						struct super_block_data *super)
{
	int result = put_bytes(buffer, 32, &super->magic_label);

	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_bytes(buffer, NONCE_INFO_SIZE, &super->nonce_info);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, super->nonce);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, super->version);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer, super->block_size);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint16_le_into_buffer(buffer, super->num_indexes);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint16_le_into_buffer(buffer, super->max_saves);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = zero_bytes(buffer, 4); /* aligment */
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, super->open_chapter_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, super->page_map_blocks);
	if (result != UDS_SUCCESS) {
		return result;
	}
	if (is_converted_super_block(super)) {
		result = put_uint64_le_into_buffer(buffer,
						   super->volume_offset);
		if (result != UDS_SUCCESS) {
			return result;
		}
		result = put_uint64_le_into_buffer(buffer,
						   super->start_offset);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	return ASSERT_LOG_ONLY(content_length(buffer) == buffer_length(buffer),
			       "%zu bytes encoded, of %zu expected",
			       content_length(buffer), buffer_length(buffer));
}

static int __must_check
make_index_save_region_table(struct index_save_layout *isl,
			     unsigned int *num_regions_ptr,
			     struct region_table **table_ptr)
{
	unsigned int z;
	struct region_table *table;
	struct layout_region *lr;
	int result;
	unsigned int num_regions = 1 + /* header */
				   1 + /* index page map */
				   isl->num_zones + /* volume index zones */
				   (bool) isl->open_chapter; /* open chapter if */
							     /* needed */

	if (isl->free_space.num_blocks > 0) {
		num_regions++;
	}

	result = UDS_ALLOCATE_EXTENDED(struct region_table,
				       num_regions,
				       struct layout_region,
				       "layout region table for ISL",
				       &table);
	if (result != UDS_SUCCESS) {
		return result;
	}

	lr = &table->regions[0];
	*lr++ = isl->header;
	*lr++ = isl->index_page_map;
	for (z = 0; z < isl->num_zones; ++z) {
		*lr++ = isl->volume_index_zones[z];
	}
	if (isl->open_chapter) {
		*lr++ = *isl->open_chapter;
	}
	if (isl->free_space.num_blocks > 0) {
		*lr++ = isl->free_space;
	}

	result = ASSERT((lr == &table->regions[num_regions]),
			"incorrect number of ISL regions");
	if (result != UDS_SUCCESS) {
		return result;
	}

	*num_regions_ptr = num_regions;
	*table_ptr = table;
	return UDS_SUCCESS;
}

static unsigned int region_type_for_save_type(enum index_save_type save_type)
{
	switch (save_type) {
	case IS_SAVE:
		return RH_TYPE_SAVE;

	default:
		break;
	}

	return RH_TYPE_UNSAVED;
}

static int __must_check
write_index_save_header(struct index_save_layout *isl,
			struct region_table *table,
			unsigned int num_regions,
			struct buffered_writer *writer)
{
	unsigned int i;
	struct buffer *buffer;
	int result;
	size_t payload = sizeof(isl->save_data);
	size_t table_size = sizeof(struct region_table) +
		num_regions * sizeof(struct layout_region);

	if (isl->index_state_buffer != NULL) {
		payload += content_length(isl->index_state_buffer);
	}

	table->header = (struct region_header) {
		.magic = REGION_MAGIC,
		.region_blocks = isl->index_save.num_blocks,
		.type = region_type_for_save_type(isl->save_type),
		.version = 1,
		.num_regions = num_regions,
		.payload = payload,
	};

	result = make_buffer(table_size, &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_region_header(buffer, &table->header);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	for (i = 0; i < num_regions; i++) {
		result = encode_layout_region(buffer, &table->regions[i]);
		if (result != UDS_SUCCESS) {
			free_buffer(UDS_FORGET(buffer));
			return result;
		}
	}
	result = ASSERT_LOG_ONLY(content_length(buffer) == table_size,
				 "%zu bytes encoded of %zu expected",
				 content_length(buffer),
				 table_size);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(writer, get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = make_buffer(sizeof(isl->save_data), &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_index_save_data(buffer, &isl->save_data);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(writer, get_buffer_contents(buffer),
					  content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (isl->index_state_buffer != NULL) {
		result = write_to_buffered_writer(writer,
					          get_buffer_contents(isl->index_state_buffer),
					          content_length(isl->index_state_buffer));
		if (result != UDS_SUCCESS) {
			return result;
		}
	}

	return flush_buffered_writer(writer);
}

static int write_index_save_layout(struct index_layout *layout,
				   struct index_save_layout *isl)
{
	unsigned int num_regions;
	struct region_table *table;
	struct buffered_writer *writer = NULL;
	int result = make_index_save_region_table(isl, &num_regions, &table);

	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_layout_writer(layout, &isl->header,
				    -layout->super.start_offset,
				    &writer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return result;
	}

	result = write_index_save_header(isl, table, num_regions, writer);
	UDS_FREE(table);
	free_buffered_writer(writer);

	isl->written = true;
	return result;
}

static int __must_check save_sub_index_regions(struct index_layout *layout)
{
	struct sub_index_layout *sil = &layout->index;
	unsigned int j;

	for (j = 0; j < layout->super.max_saves; ++j) {
		struct index_save_layout *isl = &sil->saves[j];
		int result = write_index_save_layout(layout, isl);

		if (result != UDS_SUCCESS) {
			return uds_log_error_strerror(result,
						      "unable to format index %u save 0 layout",
						      j);
		}
	}
	return UDS_SUCCESS;
}

static int __must_check
write_single_file_header(struct index_layout *layout,
			 struct region_table *table,
			 unsigned int num_regions,
			 struct buffered_writer *writer)
{
	unsigned int i;
	uint16_t payload;
	size_t table_size = sizeof(struct region_table) + num_regions *
		sizeof(struct layout_region);
	struct buffer *buffer;
	int result;

	if (is_converted_super_block(&layout->super)) {
		payload = sizeof(struct super_block_data);
	} else {
		payload = sizeof(struct super_block_data) -
			sizeof(layout->super.volume_offset) -
			sizeof(layout->super.start_offset);
	}

	table->header = (struct region_header) {
		.magic = REGION_MAGIC,
		.region_blocks = layout->total_blocks,
		.type = RH_TYPE_SUPER,
		.version = 1,
		.num_regions = num_regions,
		.payload = payload,
	};

	result = make_buffer(table_size, &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_region_header(buffer, &table->header);

	for (i = 0; i < num_regions; i++) {
		if (result == UDS_SUCCESS) {
			result = encode_layout_region(buffer,
						      &table->regions[i]);
		}
	}

	if (result == UDS_SUCCESS) {
		result = write_to_buffered_writer(writer,
					          get_buffer_contents(buffer),
					          content_length(buffer));
	}

	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = make_buffer(payload, &buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = encode_super_block_data(buffer, &layout->super);
	if (result != UDS_SUCCESS) {
		free_buffer(UDS_FORGET(buffer));
		return result;
	}

	result = write_to_buffered_writer(writer, get_buffer_contents(buffer),
				          content_length(buffer));
	free_buffer(UDS_FORGET(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}
	return flush_buffered_writer(writer);
}

/**
 * Save an index layout table to persistent storage using the io_factory in
 * the layout.
 *
 * @param layout  The layout to save
 * @param offset  A block offset to apply when writing the layout
 *
 * @return UDS_SUCCESS or an error code
 */
static int __must_check
save_single_file_layout(struct index_layout *layout, off_t offset)
{
	struct buffered_writer *writer = NULL;
	struct region_table *table;
	unsigned int num_regions;

	int result = make_single_file_region_table(layout,
						   &num_regions,
						   &table);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_layout_writer(layout, &layout->header, offset, &writer);
	if (result != UDS_SUCCESS) {
		UDS_FREE(table);
		return result;
	}

	result = write_single_file_header(layout, table, num_regions, writer);
	UDS_FREE(table);
	free_buffered_writer(writer);

	return result;
}

/**
 * Write the index configuration.
 *
 * @param layout  the generic index layout
 * @param config  the index configuration to write
 * @param offset  A block offset to apply when writing the configuration
 *
 * @return UDS_SUCCESS or an error code
 **/
static int __must_check
write_uds_index_config(struct index_layout *layout,
		       struct configuration *config,
		       off_t offset)
{
	struct buffered_writer *writer = NULL;
	int result = open_layout_writer(layout,
					&layout->config,
					offset,
					&writer);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "failed to open config region");
	}

	result = write_config_contents(writer, config, layout->super.version);
	if (result != UDS_SUCCESS) {
		free_buffered_writer(writer);
		return uds_log_error_strerror(result,
					      "failed to write config region");
	}
	result = flush_buffered_writer(writer);
	if (result != UDS_SUCCESS) {
		free_buffered_writer(writer);
		return uds_log_error_strerror(result,
					      "cannot flush config writer");
	}
	free_buffered_writer(writer);
	return UDS_SUCCESS;
}

static int create_index_layout(struct index_layout *layout,
			       struct configuration *config)
{
	struct save_layout_sizes sizes;
	int result;

	result = compute_sizes(config, &sizes);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = init_single_file_layout(layout, &sizes);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = save_sub_index_regions(layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = save_single_file_layout(layout, 0);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = write_uds_index_config(layout, config, 0);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_SUCCESS;
}

/**
 * Make an IO factory from a name string.
 *
 * @param layout      The layout in which to store the parsed values
 * @param config      The index configuration for the new layout
 * @param new_layout  Whether this is a new layout
 *
 * @return UDS_SUCCESS or an error code
 **/
static int create_layout_factory(struct index_layout *layout,
				 const struct configuration *config,
				 bool new_layout)
{
	size_t writable_size;
	struct io_factory *factory = NULL;
	int result;

#ifdef __KERNEL__
	result = make_uds_io_factory(config->name, &factory);
#else
	enum file_access access =
		(new_layout ? FU_CREATE_READ_WRITE : FU_READ_WRITE);
	result = make_uds_io_factory(config->name, access, &factory);
#endif
	if (result != UDS_SUCCESS) {
		return result;
	}

	writable_size = get_uds_writable_size(factory) & -UDS_BLOCK_SIZE;
	if (writable_size < config->size + config->offset) {
		put_uds_io_factory(factory);
		uds_log_error("index storage (%zu) is smaller than the requested size %zu",
			      writable_size,
			      config->size + config->offset);
		return -ENOSPC;
	}

	layout->factory = factory;
	layout->factory_size =
		(config->size > 0) ? config->size : writable_size;
	layout->offset = config->offset;
	return UDS_SUCCESS;
}

int make_uds_index_layout(struct configuration *config,
			  bool new_layout,
			  struct index_layout **layout_ptr)
{
	struct index_layout *layout = NULL;
	struct save_layout_sizes sizes;
	int result;

	result = compute_sizes(config, &sizes);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = UDS_ALLOCATE(1, struct index_layout, __func__, &layout);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = create_layout_factory(layout, config, new_layout);
	if (result != UDS_SUCCESS) {
		free_uds_index_layout(layout);
		return result;
	}

	if (layout->factory_size < sizes.total_size) {
		uds_log_error("index storage (%zu) is smaller than the required size %llu",
			      layout->factory_size,
			      (unsigned long long) sizes.total_size);
		free_uds_index_layout(layout);
		return -ENOSPC;
	}

	if (new_layout) {
		/* Populate the layout from the UDS configuration */
		result = create_index_layout(layout, config);
	} else {
		/* Populate the layout from the saved index. */
		result = load_index_layout(layout, config);
	}
	if (result != UDS_SUCCESS) {
		free_uds_index_layout(layout);
		return result;
	}

	*layout_ptr = layout;
	return UDS_SUCCESS;
}

void free_uds_index_layout(struct index_layout *layout)
{
	struct sub_index_layout *sil;

	if (layout == NULL) {
		return;
	}

	sil = &layout->index;
	if (sil->saves != NULL) {
		unsigned int j;

		for (j = 0; j < layout->super.max_saves; ++j) {
			struct index_save_layout *isl = &sil->saves[j];

			UDS_FREE(isl->volume_index_zones);
			UDS_FREE(isl->open_chapter);
			free_buffer(UDS_FORGET(isl->index_state_buffer));
		}
	}

	UDS_FREE(sil->saves);

	if (layout->factory != NULL) {
		put_uds_io_factory(layout->factory);
	}
	UDS_FREE(layout);
}

int replace_index_layout_storage(struct index_layout *layout,
				 const char *name)
{
	return replace_uds_storage(layout->factory, name);
}

#ifdef __KERNEL__
int open_uds_volume_bufio(struct index_layout *layout,
			  size_t block_size,
			  unsigned int reserved_buffers,
			  struct dm_bufio_client **client_ptr)
{
	off_t offset = (layout->index.volume.start_block +
			layout->super.volume_offset -
			layout->super.start_offset) *
		layout->super.block_size;
	return make_uds_bufio(layout->factory,
			      offset,
			      block_size,
			      reserved_buffers,
			      client_ptr);
}
#else
int open_uds_volume_region(struct index_layout *layout,
			   struct io_region **region_ptr)
{
	struct layout_region *lr = &layout->index.volume;
	off_t start = (lr->start_block + layout->super.volume_offset -
		       layout->super.start_offset) *
		layout->super.block_size;
	size_t size = lr->num_blocks * layout->super.block_size;
	int result =
		make_uds_io_region(layout->factory, start, size, region_ptr);
	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "cannot access index volume region");
	}
	return UDS_SUCCESS;
}
#endif

uint64_t get_uds_volume_nonce(struct index_layout *layout)
{
	return layout->index.nonce;
}

static uint64_t generate_index_save_nonce(uint64_t volume_nonce,
					  struct index_save_layout *isl)
{
	struct SaveNonceData {
		struct index_save_data data;
		uint64_t offset;
	} nonce_data;
	byte buffer[sizeof(nonce_data)];
	size_t offset = 0;

	nonce_data.data = isl->save_data;
	nonce_data.data.nonce = 0;
	nonce_data.offset = isl->index_save.start_block;

	encode_uint64_le(buffer, &offset, nonce_data.data.timestamp);
	encode_uint64_le(buffer, &offset, nonce_data.data.nonce);
	encode_uint32_le(buffer, &offset, nonce_data.data.version);
	encode_uint32_le(buffer, &offset, 0U); /* padding */
	encode_uint64_le(buffer, &offset, nonce_data.offset);
	ASSERT_LOG_ONLY(offset == sizeof(nonce_data),
			"%zu bytes encoded of %zu expected",
			offset,
			sizeof(nonce_data));
	return generate_secondary_nonce(volume_nonce, buffer, sizeof(buffer));
}

static int validate_index_save_layout(struct index_save_layout *isl,
				      uint64_t volume_nonce,
				      uint64_t *save_time_ptr)
{
	if (isl->save_type == NO_SAVE || isl->num_zones == 0 ||
	    isl->save_data.timestamp == 0) {
		return UDS_BAD_STATE;
	}
	if (isl->save_data.nonce !=
	    generate_index_save_nonce(volume_nonce, isl)) {
		return UDS_BAD_STATE;
	}
	if (save_time_ptr != NULL) {
		*save_time_ptr = isl->save_data.timestamp;
	}
	return UDS_SUCCESS;
}

static int __must_check
select_oldest_index_save_layout(struct sub_index_layout *sil,
				unsigned int max_saves,
				struct index_save_layout **isl_ptr)
{
	struct index_save_layout *oldest = NULL;
	uint64_t oldest_time = 0;
	int result;

	/* find the oldest valid or first invalid slot */
	struct index_save_layout *isl;

	for (isl = sil->saves; isl < sil->saves + max_saves; ++isl) {
		uint64_t save_time = 0;
		int result =
			validate_index_save_layout(isl, sil->nonce, &save_time);
		if (result != UDS_SUCCESS) {
			save_time = 0;
		}
		if (oldest == NULL || save_time < oldest_time) {
			oldest = isl;
			oldest_time = save_time;
		}
	}

	result = ASSERT((oldest != NULL), "no oldest or free save slot");
	if (result != UDS_SUCCESS) {
		return result;
	}
	*isl_ptr = oldest;
	return UDS_SUCCESS;
}

static int __must_check
select_latest_index_save_layout(struct sub_index_layout *sil,
				unsigned int max_saves,
				struct index_save_layout **isl_ptr)
{
	struct index_save_layout *latest = NULL;
	uint64_t latest_time = 0;

	/* find the latest valid save slot */
	struct index_save_layout *isl;

	for (isl = sil->saves; isl < sil->saves + max_saves; ++isl) {
		uint64_t save_time = 0;
		int result =
			validate_index_save_layout(isl, sil->nonce, &save_time);
		if (result != UDS_SUCCESS) {
			continue;
		}
		if (save_time > latest_time) {
			latest = isl;
			latest_time = save_time;
		}
	}

	if (latest == NULL) {
		uds_log_error("No valid index save found");
		return UDS_INDEX_NOT_SAVED_CLEANLY;
	}
	*isl_ptr = latest;
	return UDS_SUCCESS;
}

static int __must_check
instantiate_index_save_layout(struct index_save_layout *isl,
			      struct super_block_data *super,
			      uint64_t volume_nonce,
			      unsigned int num_zones)
{
	int result = UDS_SUCCESS;

	if (isl->open_chapter == NULL) {
		result = UDS_ALLOCATE(1,
				      struct layout_region,
				      "open chapter layout",
				      &isl->open_chapter);
		if (result != UDS_SUCCESS) {
			return result;
		}
	}
	if (num_zones != isl->num_zones) {
		if (isl->volume_index_zones != NULL) {
			UDS_FREE(isl->volume_index_zones);
		}
		result = UDS_ALLOCATE(num_zones,
				      struct layout_region,
				      "volume index zone layouts",
				      &isl->volume_index_zones);
		if (result != UDS_SUCCESS) {
			return result;
		}
		isl->num_zones = num_zones;
	}

	populate_index_save_layout(isl, super, num_zones, IS_SAVE);

	result = make_buffer(INDEX_STATE_BUFFER_SIZE, &isl->index_state_buffer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	isl->read = isl->written = false;
	isl->save_type = IS_SAVE;
	memset(&isl->save_data, 0, sizeof(isl->save_data));
	isl->save_data.timestamp =
		ktime_to_ms(current_time_ns(CLOCK_REALTIME));
	isl->save_data.version = 1;
	isl->save_data.nonce = generate_index_save_nonce(volume_nonce, isl);

	return UDS_SUCCESS;
}

static int __must_check
invalidate_old_save(struct index_layout *layout, struct index_save_layout *isl)
{
	uint64_t start_block = isl->index_save.start_block;
	uint64_t save_blocks = isl->index_save.num_blocks;
	unsigned int save = isl->index_save.instance;

	int result = reset_index_save_layout(isl,
					     &start_block,
					     save_blocks,
					     layout->super.page_map_blocks,
					     save);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return write_index_save_layout(layout, isl);
}

static int setup_uds_index_save_slot(struct index_layout *layout,
				     unsigned int num_zones,
				     unsigned int *save_slot_ptr)
{
	struct sub_index_layout *sil = &layout->index;

	struct index_save_layout *isl = NULL;
	int result = select_oldest_index_save_layout(sil,
						     layout->super.max_saves,
						     &isl);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = invalidate_old_save(layout, isl);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = instantiate_index_save_layout(isl, &layout->super, sil->nonce,
					       num_zones);
	if (result != UDS_SUCCESS) {
		return result;
	}

	*save_slot_ptr = isl - sil->saves;
	return UDS_SUCCESS;
}

static int find_latest_uds_index_save_slot(struct index_layout *layout,
					   unsigned int *num_zones_ptr,
					   unsigned int *slot_ptr)
{
	struct sub_index_layout *sil = &layout->index;

	struct index_save_layout *isl = NULL;
	int result = select_latest_index_save_layout(sil,
						     layout->super.max_saves,
						     &isl);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (num_zones_ptr != NULL) {
		*num_zones_ptr = isl->num_zones;
	}
	if (slot_ptr != NULL) {
		*slot_ptr = isl - sil->saves;
	}
	return UDS_SUCCESS;
}

static int commit_uds_index_save(struct index_layout *layout,
				 unsigned int save_slot)
{
	struct index_save_layout *isl;

	isl = &layout->index.saves[save_slot];
	return write_index_save_layout(layout, isl);
}


static void mutilate_index_save_info(struct index_save_layout *isl)
{
	memset(&isl->save_data, 0, sizeof(isl->save_data));
	isl->read = isl->written = 0;
	isl->save_type = NO_SAVE;
	isl->num_zones = 0;
	free_buffer(UDS_FORGET(isl->index_state_buffer));
}

static void cancel_uds_index_save(struct index_layout *layout,
				  unsigned int save_slot)
{
	mutilate_index_save_info(&layout->index.saves[save_slot]);
}

static int discard_uds_index_saves(struct index_layout *layout)
{
	unsigned int i;
	int result;
	int saved_result = UDS_SUCCESS;
	struct sub_index_layout *sil = &layout->index;

	for (i = 0; i < layout->super.max_saves; ++i) {
		result = invalidate_old_save(layout, &sil->saves[i]);
		if (result != UDS_SUCCESS) {
			saved_result = result;
		}
	}

	return saved_result;
}

static struct buffer *get_uds_index_state_buffer(struct index_layout *layout,
						 unsigned int slot)
{
	return layout->index.saves[slot].index_state_buffer;
}

static int find_layout_region(struct index_layout *layout,
			      unsigned int slot,
			      const char *operation,
			      enum region_kind kind,
			      unsigned int zone,
			      struct layout_region **lr_ptr)
{
	struct index_save_layout *isl = &layout->index.saves[slot];
	struct layout_region *lr = NULL;
	int result = ASSERT((slot < layout->super.max_saves), "%s not started",
			    operation);
	if (result != UDS_SUCCESS) {
		return result;
	}

	switch (kind) {
	case RL_KIND_INDEX_PAGE_MAP:
		lr = &isl->index_page_map;
		break;

	case RL_KIND_OPEN_CHAPTER:
		if (isl->open_chapter == NULL) {
			return uds_log_error_strerror(UDS_UNEXPECTED_RESULT,
						      "%s: %s has no open chapter",
						      __func__,
						      operation);
		}
		lr = isl->open_chapter;
		break;

	case RL_KIND_VOLUME_INDEX:
		if (isl->volume_index_zones == NULL || zone >= isl->num_zones) {
			return uds_log_error_strerror(UDS_UNEXPECTED_RESULT,
						      "%s: %s has no volume index zone %u",
						      __func__,
						      operation,
						      zone);
		}
		lr = &isl->volume_index_zones[zone];
		break;

	default:
		return uds_log_error_strerror(UDS_INVALID_ARGUMENT,
					      "%s: unexpected kind %u",
					      __func__,
					      kind);
	}

	*lr_ptr = lr;
	return UDS_SUCCESS;
}

static int open_uds_index_buffered_reader(struct index_layout *layout,
					  unsigned int slot,
					  enum region_kind kind,
					  unsigned int zone,
					  struct buffered_reader **reader_ptr)
{
	struct layout_region *lr = NULL;
	int result = find_layout_region(layout, slot, "load", kind, zone, &lr);

	if (result != UDS_SUCCESS) {
		return result;
	}
	return open_layout_reader(layout, lr, -layout->super.start_offset,
				  reader_ptr);
}

static int open_uds_index_buffered_writer(struct index_layout *layout,
					  unsigned int slot,
					  enum region_kind kind,
					  unsigned int zone,
					  struct buffered_writer **writer_ptr)
{
	struct layout_region *lr = NULL;
	int result = find_layout_region(layout, slot, "save", kind, zone, &lr);

	if (result != UDS_SUCCESS) {
		return result;
	}
	return open_layout_writer(layout, lr, -layout->super.start_offset,
				  writer_ptr);
}
/**
 * The index state buffer reader.
 *
 * @param state      The index state
 * @param index      The index
 * @param load_slot  The load slot to use
 *
 * @return UDS_SUCCESS or an error code
 **/
static int read_index_state_data(struct index_layout *layout,
				 struct uds_index *index,
				 unsigned int load_slot)
{
	struct index_state_data301 state_data;
	struct index_state_version file_version;
	struct buffer *buffer;
	int result;

	buffer = get_uds_index_state_buffer(layout, load_slot);
	result = rewind_buffer(buffer, uncompacted_amount(buffer));
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = get_int32_le_from_buffer(buffer, &file_version.signature);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_int32_le_from_buffer(buffer, &file_version.version_id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	if (file_version.signature != -1 || file_version.version_id != 301) {
		return uds_log_error_strerror(UDS_UNSUPPORTED_VERSION,
					      "index state version %d,%d is unsupported",
					      file_version.signature,
					      file_version.version_id);
	}

	result = get_uint64_le_from_buffer(buffer, &state_data.newest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &state_data.oldest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = get_uint64_le_from_buffer(buffer, &state_data.last_save);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = skip_forward(buffer, sizeof(uint32_t));
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = skip_forward(buffer, sizeof(uint32_t));
	if (result != UDS_SUCCESS) {
		return result;
	}

	index->newest_virtual_chapter = state_data.newest_chapter;
	index->oldest_virtual_chapter = state_data.oldest_chapter;
	index->last_save = state_data.last_save;
	return UDS_SUCCESS;
}

int load_index_state(struct index_layout *layout, struct uds_index *index)
{
	int result;
	unsigned int zone;
	unsigned int load_zones;
	unsigned int load_slot;
	struct buffered_reader *readers[MAX_ZONES];

	result = find_latest_uds_index_save_slot(layout,
						 &load_zones,
						 &load_slot);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_index_state_data(layout, index, load_slot);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_uds_index_buffered_reader(layout,
						load_slot,
						RL_KIND_OPEN_CHAPTER,
						0,
						&readers[0]);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = load_open_chapters(index, readers[0]);
	free_buffered_reader(readers[0]);
	if (result != UDS_SUCCESS) {
		return result;
	}

	for (zone = 0; zone < load_zones; zone++) {
		result = open_uds_index_buffered_reader(layout,
							load_slot,
							RL_KIND_VOLUME_INDEX,
							zone,
							&readers[zone]);
		if (result != UDS_SUCCESS) {
			for (; zone > 0; zone--) {
				free_buffered_reader(readers[zone - 1]);
			}

			return result;
		}
	}

	result = load_volume_index(index->volume_index, readers, load_zones);
	for (zone = 0; zone < load_zones; zone++) {
		free_buffered_reader(readers[zone]);
	}
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_uds_index_buffered_reader(layout,
						load_slot,
						RL_KIND_INDEX_PAGE_MAP,
						0,
						&readers[0]);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = read_index_page_map(index->volume->index_page_map,
				     readers[0]);
	free_buffered_reader(readers[0]);
	if (result != UDS_SUCCESS) {
		return result;
	}

	return UDS_SUCCESS;
}

/**
 * The index state buffer writer.
 *
 * @param layout     The index layout
 * @param index      The index
 * @param save_slot  The save slot to use
 *
 * @return UDS_SUCCESS or an error code
 **/
static int write_index_state_data(struct index_layout *layout,
				  struct uds_index *index,
				  unsigned int save_slot)
{
	struct index_state_data301 state_data;
	struct buffer *buffer;
	int result;

	buffer = get_uds_index_state_buffer(layout, save_slot);
	result = reset_buffer_end(buffer, 0);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   INDEX_STATE_VERSION_301.signature);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint32_le_into_buffer(buffer,
					   INDEX_STATE_VERSION_301.version_id);
	if (result != UDS_SUCCESS) {
		return result;
	}

	state_data = (struct index_state_data301) {
		.newest_chapter = index->newest_virtual_chapter,
		.oldest_chapter = index->oldest_virtual_chapter,
		.last_save = index->last_save,
	};

	result = put_uint64_le_into_buffer(buffer, state_data.newest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, state_data.oldest_chapter);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = put_uint64_le_into_buffer(buffer, state_data.last_save);
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = zero_bytes(buffer, sizeof(uint32_t));
	if (result != UDS_SUCCESS) {
		return result;
	}
	result = zero_bytes(buffer, sizeof(uint32_t));
	if (result != UDS_SUCCESS) {
		return result;
	}
	return UDS_SUCCESS;
}

int save_index_state(struct index_layout *layout, struct uds_index *index)
{
	int result;
	unsigned int zone;
	unsigned int save_slot;
	struct buffered_writer *writers[MAX_ZONES];

	result = setup_uds_index_save_slot(layout,
					   index->zone_count,
					   &save_slot);
	if (result != UDS_SUCCESS) {
		return result;
	}

#ifdef TEST_INTERNAL
	/*
	 * We may be synchronizing with a test waiting for a save to start,
	 * so we need a memory barrier here.
	 */
	smp_mb__before_atomic();
	atomic_inc(&saves_begun);
#endif /* TEST_INTERNAL */

	result = write_index_state_data(layout, index, save_slot);
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(layout, save_slot);
		return result;
	}

	result = open_uds_index_buffered_writer(layout,
						save_slot,
						RL_KIND_OPEN_CHAPTER,
						0,
						&writers[0]);
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(layout, save_slot);
		return result;
	}

	result = save_open_chapters(index, writers[0]);
	free_buffered_writer(writers[0]);
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(layout, save_slot);
		return result;
	}

	for (zone = 0; zone < index->zone_count; zone++) {
		result = open_uds_index_buffered_writer(layout,
							save_slot,
							RL_KIND_VOLUME_INDEX,
							zone,
							&writers[zone]);
		if (result != UDS_SUCCESS) {
			for (; zone > 0; zone--) {
				free_buffered_writer(writers[zone - 1]);
			}

			cancel_uds_index_save(layout, save_slot);
			return result;
		}
	}

	result = save_volume_index(index->volume_index,
				   writers,
				   index->zone_count);
	for (zone = 0; zone < index->zone_count; zone++) {
		free_buffered_writer(writers[zone]);
	}
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(layout, save_slot);
		return result;
	}

	result = open_uds_index_buffered_writer(layout,
						save_slot,
						RL_KIND_INDEX_PAGE_MAP,
						0,
						&writers[0]);
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(layout, save_slot);
		return result;
	}

	result = write_index_page_map(index->volume->index_page_map,
				      writers[0]);
	free_buffered_writer(writers[0]);
	if (result != UDS_SUCCESS) {
		cancel_uds_index_save(layout, save_slot);
		return result;
	}

	return commit_uds_index_save(layout, save_slot);
}

int discard_index_state_data(struct index_layout *layout)
{
	int result = discard_uds_index_saves(layout);

	if (result != UDS_SUCCESS) {
		return uds_log_error_strerror(result,
					      "%s: cannot destroy all index saves",
					      __func__);
	}
	return UDS_SUCCESS;
}

int discard_open_chapter(struct index_layout *layout)
{
	int result;
	unsigned int num_zones = 0;
	unsigned int save_slot = 0;
	struct buffered_writer *writer;

	result = find_latest_uds_index_save_slot(layout,
						 &num_zones,
						 &save_slot);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = open_uds_index_buffered_writer(layout,
						save_slot,
						RL_KIND_OPEN_CHAPTER,
						0,
						&writer);
	if (result != UDS_SUCCESS) {
		return result;
	}

	result = write_zeros_to_buffered_writer(writer,
						UDS_BLOCK_SIZE);
	if (result != UDS_SUCCESS) {
		free_buffered_writer(writer);
		return result;
	}

	result = flush_buffered_writer(writer);
	free_buffered_writer(writer);
	return result;
}
#ifdef TEST_INTERNAL

int update_uds_layout(struct index_layout *layout,
		      struct configuration *config,
		      off_t lvm_offset,
		      off_t offset)
{
	int result = UDS_SUCCESS;
	off_t offset_blocks = offset / UDS_BLOCK_SIZE;
	off_t lvm_blocks = lvm_offset / UDS_BLOCK_SIZE;
	struct super_block_data super = layout->super;
	struct sub_index_layout index = layout->index;

	layout->super.start_offset = lvm_blocks;
	layout->super.volume_offset = offset_blocks;
	layout->index.sub_index.num_blocks -= offset_blocks;
	layout->index.volume.num_blocks -= offset_blocks;
	layout->total_blocks -= offset_blocks;
	layout->super.version = 7;
	result = save_single_file_layout(layout, offset_blocks);
	if (result == UDS_SUCCESS) {
		result = write_uds_index_config(layout, config, offset_blocks);
	}
	layout->index = index;
	layout->super = super;
	return result;
}
#endif /* TEST_INTERNAL */