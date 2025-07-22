#ifndef SELFTEST_SDXI_LIB_H
#define SELFTEST_SDXI_LIB_H

#include <inttypes.h>
#include <stdbool.h>

#include "descriptor.h"
#include "linux/build_bug.h"
#include "linux/types.h"

struct sdxi_cxt_sts {
	__u8   state;
	__u8   misc0;
	__u8   rsvd_0[6];
	__le64 read_index;
};

enum {
	SDXI_DESC_RING_ENTRIES = 1024,
};

struct descriptor_ring {
	struct sdxi_desc entry[SDXI_DESC_RING_ENTRIES];
};

struct sdxi_context {
	// We have to keep the /dev/sdxi fd open for the life of the
	// context due to driver fragility...
	int device_fd;

	uint32_t id;
	__le64 *doorbell;
	__le64 *write_idx;
	size_t n_entries;
	struct sdxi_cxt_sts *cxt_sts;
	struct descriptor_ring *ring;
};

struct sdxi_context *sdxi_context_create(void);
void sdxi_context_destroy(struct sdxi_context *cxt);
bool sdxi_context_running(const struct sdxi_context *cxt);

struct sdxi_desc sdxi_dsc_encode_copy(void *dest, const void *src, size_t n);
struct sdxi_desc sdxi_dsc_encode_nop(void);
struct sdxi_desc sdxi_dsc_encode_rsvd(void);

int sdxi_submit_sync(struct sdxi_context *cxt, const struct sdxi_desc *desc);
int sdxi_submit_oneshot(const struct sdxi_desc *desc);

#endif/* SELFTEST_SDXI_LIB_H */
