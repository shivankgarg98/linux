#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/sdxi.h>

#include "lib.h"
#include "lib/fixture-context.h"
#include "../kselftest_harness.h"

// This basically duplicates the normal context fixture, maybe this
// could be fixed by using a variant?
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

TEST_F(sdxi_context_nomap, context_create_and_destroy_nomap)
{
	// Just run the fixture setup and teardown
}

TEST_F(sdxi_context, context_create_and_destroy)
{
	// Just run the fixture setup and teardown
}

TEST_HARNESS_MAIN
