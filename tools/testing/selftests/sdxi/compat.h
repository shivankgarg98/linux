#ifndef SELFTEST_SDXI_COMPAT_H
#define SELFTEST_SDXI_COMPAT_H

// Only defined in compiler-gcc.h for some reason
#ifndef __aligned
#define __aligned(x) __attribute__((aligned(x)))
#endif

#endif/* SELFTEST_SDXI_COMPAT_H */
