/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM vivo_rsc
#if !defined(_RSC_FREQ_RULE_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _RSC_FREQ_RULE_TRACE_H
#include <linux/tracepoint.h>
TRACE_EVENT(limit_next_freq,
	TP_PROTO(unsigned int cpu, unsigned int req, unsigned int min,
		 unsigned int rule, unsigned int new),
	TP_ARGS(cpu, req, min, rule, new),
	TP_STRUCT__entry(__field(unsigned int, cpu) __field(unsigned int, req)
		__field(unsigned int, min) __field(unsigned int, rule) __field(unsigned int, new)),
	TP_fast_assign(__entry->cpu = cpu; __entry->req = req; __entry->min = min;
		__entry->rule = rule; __entry->new = new;),
	TP_printk("cpu=%u req=%u min=%u rule=%u next=%u", __entry->cpu,
		__entry->req, __entry->min, __entry->rule, __entry->new)
);
#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE rsc_freq_rule_trace
#include <trace/define_trace.h>
