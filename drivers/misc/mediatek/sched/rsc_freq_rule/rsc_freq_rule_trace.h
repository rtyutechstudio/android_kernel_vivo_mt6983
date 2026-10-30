/* SPDX-License-Identifier: GPL-2.0-only */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM vivo_rsc
#if !defined(_RSC_FREQ_RULE_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _RSC_FREQ_RULE_TRACE_H
#include <linux/tracepoint.h>
#ifndef _RSC_CAPACITY_SAMPLE_DEFINED
#define _RSC_CAPACITY_SAMPLE_DEFINED
struct rsc_capacity_sample {
	unsigned int cpu, source, mid_state, little_state;
	unsigned long cap_in, cap_opp, irq, rt, dl, thermal;
	unsigned long available, cap_out, rq_capacity;
};
#endif
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
TRACE_EVENT(capacity_apply,
	TP_PROTO(const struct rsc_capacity_sample *s),
	TP_ARGS(s),
	TP_STRUCT__entry(
		__field(unsigned int, cpu) __field(unsigned int, source)
		__field(unsigned int, mid_state) __field(unsigned int, little_state)
		__field(unsigned long, cap_in) __field(unsigned long, cap_opp)
		__field(unsigned long, irq) __field(unsigned long, rt)
		__field(unsigned long, dl) __field(unsigned long, thermal)
		__field(unsigned long, available) __field(unsigned long, cap_out)
		__field(unsigned long, rq_capacity)
	),
	TP_fast_assign(
		__entry->cpu=s->cpu; __entry->source=s->source;
		__entry->mid_state=s->mid_state; __entry->little_state=s->little_state;
		__entry->cap_in=s->cap_in; __entry->cap_opp=s->cap_opp;
		__entry->irq=s->irq; __entry->rt=s->rt; __entry->dl=s->dl;
		__entry->thermal=s->thermal; __entry->available=s->available;
		__entry->cap_out=s->cap_out; __entry->rq_capacity=s->rq_capacity;
	),
	TP_printk("cpu=%u source=%u mi=%u li=%u in=%lu opp=%lu irq=%lu rt=%lu dl=%lu thermal=%lu available=%lu out=%lu rq=%lu",
		__entry->cpu, __entry->source, __entry->mid_state, __entry->little_state,
		__entry->cap_in, __entry->cap_opp, __entry->irq, __entry->rt,
		__entry->dl, __entry->thermal, __entry->available,
		__entry->cap_out, __entry->rq_capacity)
);
#endif
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE rsc_freq_rule_trace
#include <trace/define_trace.h>
