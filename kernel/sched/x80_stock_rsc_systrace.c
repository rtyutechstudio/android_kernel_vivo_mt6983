// SPDX-License-Identifier: GPL-2.0
/* Finite formatter __rsc_systrace_c [471b6c,471d50). */
#include <linux/compiler.h>
#include <linux/kernel.h>
#include <stdarg.h>
#include <linux/string.h>
#include <linux/types.h>

/* Genuine generic tracing provider must define immediately after its static
 * tracing_mark_write(): unsigned long tracing_mark_write_ext =
 *                                      (unsigned long)tracing_mark_write;
 * Stock default points to tracing_mark_write.cfi_jt, not zero or _THIS_IP_.
 */
extern unsigned long tracing_mark_write_ext;

void __rsc_systrace_c(int pid, void *address, s64 value, const char *format, ...)
{
	char name[0x100] = { 0 };
	char line[0x100] = { 0 };
	va_list args;
	int ret;

	va_start(args, format);
	ret = vsnprintf(name, sizeof(name), format, args);
	va_end(args);
	if (ret < 0)
		return;
	if (ret == sizeof(name))
		name[sizeof(name) - 1] = 0;
	if (address)
		ret = snprintf(line, sizeof(line), "C|%d|%s|%lld|0x%llx\n",
			       pid, name, value, (unsigned long long)address);
	else
		ret = snprintf(line, sizeof(line), "C|%d|%s|%lld\n", pid, name, value);
	if (ret < 0)
		return;
	if (ret == sizeof(line))
		line[sizeof(line) - 1] = 0;
	__trace_puts(READ_ONCE(tracing_mark_write_ext), line, strlen(line));
}
