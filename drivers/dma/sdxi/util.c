// SPDX-License-Identifier: GPL-2.0-only
// Miscellaneous utility code.

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "sdxi.h"

// Use this to insert artificial delays between critical register and
// data structure updates to more easily recreate issues with function
// and context init/exit.
static unsigned int update_delay_ms = 100;
module_param(update_delay_ms, uint, 0644);
MODULE_PARM_DESC(iowrite_delay_ms, "Artificial delay to insert after critical data structure updates");

// Delay for up to one second, longer doesn't seem useful.
void sdxi_delay(void)
{
	might_sleep();

	if (update_delay_ms)
		fsleep(min(update_delay_ms, 1000) * 1000UL);
}

