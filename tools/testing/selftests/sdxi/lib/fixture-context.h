#ifndef SELFTEST_SDXI_FIXTURE_CONTEXT_H
#define SELFTEST_SDXI_FIXTURE_CONTEXT_H

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/sdxi.h>

#include "lib.h"

#include "../../kselftest_harness.h"

FIXTURE(sdxi_context) {
	struct sdxi_context context;
};

FIXTURE_SETUP(sdxi_context)
{
	struct sdxi_context *cxt = &self->context;
	int device_fd;

	errno = 0;
	device_fd = open("/dev/sdxi", O_RDWR | O_CLOEXEC);
	ASSERT_GE(device_fd, 0) {
		if (errno == ENOENT)
			SKIP(return,
			     "/dev/sdxi does not exist");
		if (errno == EACCES && geteuid() != 0)
			SKIP(return,
			     "insufficient privileges");
	}

	struct sdxi_create_cxt_args arg = {
		.cxt_type = SDXI_CXT_TYPE_USER,
		.ring_entries = 1024,
	};

	ASSERT_EQ(ioctl(device_fd, SDXI_CREATE_CXT, &arg), 0);
	ASSERT_NE(arg.cxt_id, 0U); // 0 is reserved for admin context

	*cxt = (struct sdxi_context) {
		.device_fd = device_fd,
		.id = arg.cxt_id,
		.doorbell = mmap(NULL, sizeof(*cxt->doorbell),
				 PROT_READ | PROT_WRITE, MAP_SHARED,
				 device_fd, arg.doorbell_mmap_base),
		.write_idx = mmap(NULL, (sizeof(*cxt->write_idx)),
				  PROT_READ | PROT_WRITE, MAP_SHARED,
				  device_fd, arg.write_index_mmap_base),
		.cxt_sts = mmap(NULL, (sizeof(*cxt->cxt_sts)),
				PROT_READ, MAP_SHARED,
				device_fd, arg.cxt_status_mmap_base),
		.ring = mmap(NULL,
			     arg.ring_entries * sizeof(cxt->ring->entry[0]),
			     PROT_WRITE | PROT_READ, MAP_SHARED,
			     device_fd, arg.desc_ring_mmap_base),
		.n_entries = ARRAY_SIZE(cxt->ring->entry),
	};

	EXPECT_NE(cxt->doorbell, MAP_FAILED);
	EXPECT_NE(cxt->write_idx, MAP_FAILED);
	EXPECT_NE(cxt->cxt_sts, MAP_FAILED);
	EXPECT_NE(cxt->ring, MAP_FAILED);
}

FIXTURE_TEARDOWN(sdxi_context)
{
	struct sdxi_context *cxt = &self->context;

	EXPECT_EQ(ioctl(cxt->device_fd, SDXI_CLOSE_CXT,
			&(struct sdxi_close_cxt_args) {
				.cxt_id = cxt->id,
			}),
		  0);
	EXPECT_EQ(close(cxt->device_fd), 0);
}

#endif /* SELFTEST_SDXI_FIXTURE_CONTEXT_H */
