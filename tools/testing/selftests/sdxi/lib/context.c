#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/kernel.h>
#include <linux/sdxi.h>

#include "lib.h"

#define alloc(...)					\
	({						\
		size_t sz = sizeof(__VA_ARGS__);	\
		void *ptr = malloc(sz);			\
		if (ptr)				\
			memset(ptr, 0, sz);		\
		(typeof(__VA_ARGS__) *)ptr;		\
	})

struct sdxi_context *sdxi_context_create(void)
{
	struct sdxi_context *cxt;
	int fd;

	cxt = alloc(*cxt);
	if (!cxt)
		return NULL;

	errno = 0;
	fd = open("/dev/sdxi", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		goto free_cxt;

	struct sdxi_create_cxt_args arg = {
		.cxt_type = SDXI_CXT_TYPE_USER,
		.ring_entries = 1024,
	};

	int err = ioctl(fd, SDXI_CREATE_CXT, &arg);
	if (err)
		goto close_fd;

	*cxt = (struct sdxi_context) {
		.device_fd = fd,
		.id = arg.cxt_id,
		.doorbell = mmap(NULL, sizeof(*cxt->doorbell),
				 PROT_READ | PROT_WRITE, MAP_SHARED,
				 fd, arg.doorbell_mmap_base),
		.write_idx = mmap(NULL, (sizeof(*cxt->write_idx)),
				  PROT_READ | PROT_WRITE, MAP_SHARED,
				  fd, arg.write_index_mmap_base),
		.cxt_sts = mmap(NULL, (sizeof(*cxt->cxt_sts)),
				PROT_READ, MAP_SHARED,
				fd, arg.cxt_status_mmap_base),
		.ring = mmap(NULL,
			     arg.ring_entries * sizeof(cxt->ring->entry[0]),
			     PROT_WRITE | PROT_READ, MAP_SHARED,
			     fd, arg.desc_ring_mmap_base),
		.n_entries = ARRAY_SIZE(cxt->ring->entry),
	};

	if (cxt->doorbell == MAP_FAILED ||
	    cxt->write_idx == MAP_FAILED ||
	    cxt->cxt_sts == MAP_FAILED ||
	    cxt->ring == MAP_FAILED)
		goto unmap;

	return cxt;

unmap:
	(void)munmap(cxt->doorbell, sizeof(*cxt->doorbell));
	(void)munmap(cxt->write_idx, sizeof(*cxt->write_idx));
	(void)munmap(cxt->cxt_sts, sizeof(*cxt->cxt_sts));
	(void)munmap(cxt->ring, arg.ring_entries * sizeof(cxt->ring->entry[0]));

	(void)ioctl(fd, SDXI_CLOSE_CXT,
	      &(struct sdxi_close_cxt_args) {
		      .cxt_id = arg.cxt_id,
	      });
close_fd:
	(void)close(fd);
free_cxt:
	free(cxt);
	return NULL;
}

void sdxi_context_destroy(struct sdxi_context *cxt)
{
	(void)munmap(cxt->doorbell, sizeof(*cxt->doorbell));
	(void)munmap(cxt->write_idx, sizeof(*cxt->write_idx));
	(void)munmap(cxt->cxt_sts, sizeof(*cxt->cxt_sts));
	(void)munmap(cxt->ring, cxt->n_entries * sizeof(cxt->ring->entry[0]));
	(void)ioctl(cxt->device_fd, SDXI_CLOSE_CXT,
	      &(struct sdxi_close_cxt_args) {
		      .cxt_id = cxt->id,
	      });
	(void)close(cxt->device_fd);
	free(cxt);
}

bool sdxi_context_running(const struct sdxi_context *cxt)
{
	struct sdxi_cxt_sts *sts = cxt->cxt_sts;
	enum {
		CXTV_RUN = 1,
	};

	// fixme: should use FIELD_GET
	return sts->state == CXTV_RUN;
}
