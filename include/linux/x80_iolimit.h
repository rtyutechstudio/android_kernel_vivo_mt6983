/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_X80_IOLIMIT_H
#define _LINUX_X80_IOLIMIT_H
#include <linux/types.h>
#ifdef CONFIG_RSC_IOLIMIT
void do_io_read_bandwidth_control(size_t bytes);
void do_io_write_bandwidth_control(size_t bytes);
#else
static inline void do_io_read_bandwidth_control(size_t bytes) { }
static inline void do_io_write_bandwidth_control(size_t bytes) { }
#endif
#endif
