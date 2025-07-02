// SPDX-License-Identifier: GPL-2.0-only
// Miscellaneous utility code.

#include <linux/delay.h>
#include <linux/kernel.h>
#include <linux/module.h>

#include "sdxi.h"

// Use this to insert artificial delays between critical register and
// data structure updates to more easily recreate issues with function
// and context init/exit.
static unsigned int update_delay_ms;
module_param(update_delay_ms, uint, 0644);
MODULE_PARM_DESC(iowrite_delay_ms, "Artificial delay to insert after critical data structure updates");

// Delay for up to one second, longer doesn't seem useful.
void sdxi_delay(void)
{
	might_sleep();

	if (update_delay_ms)
		fsleep(min(update_delay_ms, 1000) * 1000UL);
}

const char *cxt_sts_state_str(cxt_sts_state_t state)
{
	static const char *const context_states[] = {
		[CXTV_STOP_SW]  = "stopped (software)",
		[CXTV_RUN]      = "running",
		[CXTV_STOPG_SW] = "stopping (software)",
		[CXTV_STOP_FN]  = "stopped (function)",
		[CXTV_STOPG_FN] = "stopping (function)",
		[CXTV_ERR_FN]   = "error",
	};
	const char *str = NULL;

	if ((size_t)state < ARRAY_SIZE(context_states))
		str = context_states[(size_t)state];

	WARN_ONCE(!str, "unexpected context state %u\n", state);

	return str ? str : "unknown";
}
