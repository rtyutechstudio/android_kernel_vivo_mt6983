/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 ARM Ltd.
 */
#ifndef _ASM_VERMAGIC_H
#define _ASM_VERMAGIC_H

/*
 * vivo stock GKI kernels advertise a custom arch vermagic ("vivo aarch64")
 * so only their vendor modules load. same_magic() compares the vermagic
 * after the kernel version, so the string must match the vendor modules
 * in /vendor/lib/modules exactly.
 */
#ifdef CONFIG_VIVO_MODULE_VERMAGIC
#define MODULE_ARCH_VERMAGIC	"vivo aarch64"
#else
#define MODULE_ARCH_VERMAGIC	"aarch64"
#endif

#endif /* _ASM_VERMAGIC_H */
