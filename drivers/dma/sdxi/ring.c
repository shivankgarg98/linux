#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/range.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <asm/barrier.h>
#include <asm/byteorder.h>
#include <asm/div64.h>
#include <asm/rwonce.h>

#include "ring.h"
#include "hw.h"

/*
 * Initialize ring management state. Caller is responsible for
 * allocating, mapping, and initializing the actual control structures
 * shared with hardware: the indexes and ring array.
 */
void sdxi_ring_state_init(struct sdxi_ring_state *rs, const __le64 *read_index,
			  __le64 *write_index, u32 entries,
			  struct sdxi_desc descs[static SZ_1K])
{
	WARN_ON_ONCE(!read_index);
	WARN_ON_ONCE(!write_index);
	WARN_ON_ONCE(entries < SZ_1K); /* SDXI minimum ring size */

	*rs = (typeof(*rs)) {
		.write_index = le64_to_cpu(*write_index),
		.write_index_ptr = write_index,
		.read_index_ptr = read_index,
		.entries = entries,
		.entry = descs,
	};
	spin_lock_init(&rs->lock);

	pr_debug("initialized ring state at %p with %u entries\n",
		 rs, rs->entries);
}

int sdxi_ring_reserve(struct sdxi_ring_state *rs, size_t nr,
		      struct sdxi_ring_resv *resv)
{
	unsigned long flags;
	u64 ri, wi, pwi;
	int err;

	/*
	 * Caller bug, warn and reject.
	 */
	if (WARN_ONCE(nr < 1 || nr > rs->entries,
		      "Reservation of size %zu requested from ring of size %u\n",
		      nr, rs->entries))
		return -EINVAL;

	spin_lock_irqsave(&rs->lock, flags);

	ri = le64_to_cpu(READ_ONCE(*rs->read_index_ptr));
	pwi = rs->write_index;

	if (ri > pwi) {
		/*
		 * Bug: the read index should never exceed the write index.
		 * TODO: sdxi_err() or similar; need a reference to
		 * the device.
		 */
		err = -EIO;
		goto unlock;
	}

	wi = pwi + nr;
	if (wi - ri > rs->entries) {
		/*
		 * Not enough space available right now.
		 * TODO: sdxi_dbg() or tracepoint here.
		 */
		err = -EBUSY;
		goto unlock;
	}

	*rs->write_index_ptr = cpu_to_le64(rs->write_index = wi);

	spin_unlock_irqrestore(&rs->lock, flags);

	*resv = (typeof(*resv)) {
		.rs = rs,
		.range = {
			.start = pwi,
			.end = wi - 1,
		},
		.iter = pwi,
	};

	WARN_ON_ONCE(range_len(&resv->range) != nr);

	return 0;
unlock:
	spin_unlock_irqrestore(&rs->lock, flags);
	return err;
}

static struct sdxi_desc *
sdxi_desc_ring_entry(const struct sdxi_ring_state *rs, u64 index)
{
	return &rs->entry[do_div(index, rs->entries)];
}

struct sdxi_desc *sdxi_ring_resv_next(struct sdxi_ring_resv *resv)
{
	if (resv->range.start <= resv->iter && resv->iter <= resv->range.end)
		return sdxi_desc_ring_entry(resv->rs, resv->iter++);
	/*
	 * Caller has iterated to the end of the reservation.
	 */
	if (resv->iter == resv->range.end + 1)
		return NULL;
	/*
	 * Should happen only if caller messed with internal
	 * reservation state.
	 */
	WARN_ONCE(1, "reservation[%llu,%llu] with iter %llu",
		  resv->range.start, resv->range.end, resv->iter);
	return NULL;
}
