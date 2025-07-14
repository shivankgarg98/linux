#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include <linux/sdxi.h>

#include "lib.h"
#include "lib/fixture-device.h"
#include "../kselftest_harness.h"

TEST_F(dev_sdxi, open_and_close_device)
{
	// Just run the fixture setup and teardown
}

// TODO:
// - Open as many contexts as possible, until error
// - Try mapping context status writable (should be denied)

TEST_HARNESS_MAIN
