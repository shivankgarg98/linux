// SPDX-License-Identifier: GPL-2.0-only
// Miscellaneous utility code.

#include <asm/bug.h>
#include <linux/array_size.h>

#include "sdxi.h"

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
