#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/sdxi.h>

#include "lib.h"
#include "../kselftest_harness.h"

FIXTURE(dev_sdxi) {
	int fd;
};

FIXTURE_SETUP(dev_sdxi) {
	errno = 0;
	self->fd = open("/dev/sdxi", O_RDONLY | O_CLOEXEC);
	ASSERT_GE(self->fd, 0) {
		if (errno == ENOENT)
			SKIP(return,
			     "/dev/sdxi does not exist");
		if (errno == EACCES && geteuid() != 0)
			SKIP(return,
			     "insufficient privileges");
	}
}

FIXTURE_TEARDOWN(dev_sdxi) {
	EXPECT_EQ(close(self->fd), 0);
}

FIXTURE(sdxi_context_nomap) {
	uint32_t id;
	int device_fd; // We have to keep the /dev/sdxi fd open for the life of
	               // the context due to driver fragility :-(
};

FIXTURE_SETUP(sdxi_context_nomap)
{
	int device_fd;

	errno = 0;
	device_fd = open("/dev/sdxi", O_RDONLY | O_CLOEXEC);
	ASSERT_GE(device_fd, 0) {
		if (errno == ENOENT)
			SKIP(return,
			     "/dev/sdxi does not exist");
		if (errno == EACCES && geteuid() != 0)
			SKIP(return,
			     "insufficient privileges");
	}

	struct sdxi_create_cxt_args arg = {
		// These are the only values the driver will accept right now.
		.cxt_type = SDXI_CXT_TYPE_USER,
		.ring_entries = 1024,
	};

	ASSERT_EQ(ioctl(device_fd, SDXI_CREATE_CXT, &arg), 0);
	EXPECT_NE(arg.cxt_id, 0U); // 0 is reserved for admin context

	self->id = arg.cxt_id;
	self->device_fd = device_fd;
}

FIXTURE_TEARDOWN(sdxi_context_nomap) {
	int device_fd = self->device_fd;

	EXPECT_EQ(ioctl(device_fd, SDXI_CLOSE_CXT,
			&(struct sdxi_close_cxt_args) {
				.cxt_id = self->id,
			}),
		  0);
	EXPECT_EQ(close(device_fd), 0);
}

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

TEST_F(dev_sdxi, open_and_close_device)
{
	// Just run the fixture setup and teardown
}

TEST_F(sdxi_context_nomap, context_create_and_destroy_nomap)
{
	// Just run the fixture setup and teardown
}

TEST_F(sdxi_context, context_create_and_destroy)
{
	// Just run the fixture setup and teardown
}

TEST_F(sdxi_context, nop)
{
	struct sdxi_desc d = sdxi_dsc_encode_nop();

	EXPECT_EQ(sdxi_submit(&self->context, &d), 0);
}

TEST_F(sdxi_context, copy)
{
	const long src = 0xabcdef;
	long dest = 0;
	struct sdxi_desc copy = sdxi_dsc_encode_copy(&dest, &src, sizeof(src));
	struct sdxi_desc nop = sdxi_dsc_encode_nop();

	EXPECT_EQ(sdxi_submit(&self->context, &copy), 0);

	// sdxi_submit() won't return until the read index advances
	// and we're enforcing serial processing of descriptors, so we
	// can assume the copy is done after submitting a nop.
	EXPECT_EQ(sdxi_submit(&self->context, &nop), 0);

	EXPECT_EQ(dest, src);
}

TEST_F(sdxi_context, nops_wrap)
{
	struct sdxi_desc d = sdxi_dsc_encode_nop();

	for (size_t i = 0; i < self->context.n_entries * 2; ++i)
		EXPECT_EQ(sdxi_submit(&self->context, &d), 0);
}

TEST_F(sdxi_context, copy_wrap)
{

	for (size_t i = 0; i < self->context.n_entries * 2; ++i) {
		const long src = random();
		long dest = 0;
		struct sdxi_desc copy = sdxi_dsc_encode_copy(&dest, &src, sizeof(src));
		struct sdxi_desc nop = sdxi_dsc_encode_nop();

		EXPECT_EQ(sdxi_submit(&self->context, &copy), 0);

		// sdxi_submit() won't return until the read index advances
		// and we're enforcing serial processing of descriptors, so we
		// can assume the copy is done after submitting a nop.
		EXPECT_EQ(sdxi_submit(&self->context, &nop), 0);

		EXPECT_EQ(dest, src);
	}
}

// TODO:
// - Open as many contexts as possible, until error
// - Try mapping context status writable (should be denied)

TEST_HARNESS_MAIN
