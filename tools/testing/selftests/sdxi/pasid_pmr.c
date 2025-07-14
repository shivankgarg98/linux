// SPDX-License-Identifier: GPLv2

#include <argp.h>
#include <error.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "lib/ptedit.h"
#include "lib/lib.h"

#define TEST_BUFFER_ALIGN	4096
#define TEST_BUFFER_SIZE 4096

static const struct argp_option options[] = {
	{ "src",    's', "user|supervisor", 0, "Access for source buffer (default: user)",      0, },
	{ "dst",    'd', "user|supervisor", 0, "Access for destination buffer (default: user)", 0, },
	{ "expect", 'e', "allow|deny",   0, "Expected result (default: allow)",               1, },
	{},
};

typedef enum {
	PTE_US_SUPERVISOR,
	PTE_US_USER,
} pte_access;

typedef enum {
	RESULT_ALLOW,
	RESULT_DENY,
} result;

static const char *result_str(result r)
{
	return r == RESULT_ALLOW ? "allow" : "deny";
}

typedef struct {
	pte_access source_access;
	pte_access dest_access;
	result expected;
} config_s;

static pte_access parse_access(const char *s, const struct argp_state *state)
{
	if (strcmp(s, "user") && strcmp(s, "supervisor"))
		argp_error(state, "unrecognized access '%s'", s);
	return strcmp(s, "user") == 0 ? PTE_US_USER : PTE_US_SUPERVISOR;
}

static result parse_result(const char *s, const struct argp_state *state)
{
	if (strcmp(s, "allow") && strcmp(s, "deny"))
		argp_error(state, "unrecognized result '%s'", s);
	return strcmp(s, "allow") == 0 ? RESULT_ALLOW : RESULT_DENY;
}

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
	config_s *config = state->input;
	error_t ret = 0;

	switch (key) {
	case 's':
		config->source_access = parse_access(arg, state);
		break;
	case 'd':
		config->dest_access = parse_access(arg, state);
		break;
	case 'e':
		config->expected = parse_result(arg, state);
		break;
	default:
		ret = ARGP_ERR_UNKNOWN;
		break;
	}

        return ret;
}

static config_s parse_cmdline(int argc, char **argv) {
	static struct argp argp = {
		.options = options,
		.parser = parse_opt,
	};

        config_s config = {
		.dest_access   = PTE_US_USER,
		.source_access = PTE_US_USER,
		.expected      = RESULT_ALLOW,
	};

	argp_parse(&argp, argc, argv, 0, 0, &config);

        return config;
}

int main(int argc, char **argv)
{
	int err;
	void *src = NULL, *dst = NULL;
	config_s config = parse_cmdline(argc, argv);
	result actual;

	if (ptedit_init())
		error(EXIT_FAILURE, 0, "ptedit_init()");

	/* init source and dest buffers */
	posix_memalign(&src, TEST_BUFFER_ALIGN, TEST_BUFFER_SIZE);
	posix_memalign(&dst, TEST_BUFFER_ALIGN, TEST_BUFFER_SIZE);

	memset(src, 0xAF, TEST_BUFFER_SIZE);
	memset(dst, 0xBE, TEST_BUFFER_SIZE);

	// Sanity check
	if (ptedit_pte_get_bit(src, 0, PTEDIT_PAGE_BIT_USER) != 1)
		error(EXIT_FAILURE, 0, "u/s bit unexpectedly set for source");
	if (ptedit_pte_get_bit(dst, 0, PTEDIT_PAGE_BIT_USER) != 1)
		error(EXIT_FAILURE, 0, "u/s bit unexpectedly set for destination");

	// Clear the u/s bit on either buffer if requested. If PASID
	// Privileged Mode is enabled for the device, and this
	// context's control structures are not marked privileged
	// (contexts exposed to user space should never be marked
	// privileged, normally), then clearing the u/s bit for either
	// buffer should cause the IOMMU to deny the access from the
	// device, causing the operation to fail.
	if (config.source_access == PTE_US_SUPERVISOR)
		ptedit_pte_clear_bit(src, 0, PTEDIT_PAGE_BIT_USER);
	if (config.dest_access == PTE_US_SUPERVISOR)
		ptedit_pte_clear_bit(dst, 0, PTEDIT_PAGE_BIT_USER);

	// Submit a copy descriptor.
	struct sdxi_desc copy = sdxi_dsc_encode_copy(dst, src,
						     TEST_BUFFER_SIZE);
	err = sdxi_submit_oneshot(&copy);
	if (err)
		error(EXIT_FAILURE, 0, "copy descriptor submission failed");

	// Restore any altered u/s bits before accessing the test
	// buffers again.
        if (config.source_access == PTE_US_SUPERVISOR)
		ptedit_pte_set_bit(src, 0, PTEDIT_PAGE_BIT_USER);
	if (config.dest_access == PTE_US_SUPERVISOR)
		ptedit_pte_set_bit(dst, 0, PTEDIT_PAGE_BIT_USER);

        ptedit_cleanup();

	actual = (memcmp(src, dst, TEST_BUFFER_SIZE) == 0) ?
		RESULT_ALLOW : RESULT_DENY;

	if (actual != config.expected) {
		error(EXIT_FAILURE, 0, "expected %s but actual result is %s",
		      result_str(config.expected), result_str(actual));
	}

        return EXIT_SUCCESS;
}
