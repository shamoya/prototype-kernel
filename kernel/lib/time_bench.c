/*
 * Benchmarking code execution time inside the kernel
 *
 * Copyright (C) 2014, Red Hat, Inc., Jesper Dangaard Brouer
 *  for licensing details see kernel-base/COPYING
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/time.h>
#include <linux/time_bench.h>

#include <linux/perf_event.h> /* perf_event_create_kernel_counter() */

static int verbose=1;

/** TSC (Time-Stamp Counter) based **
 */

/** Wall-clock based **
 */

/** PMU (Performance Monitor Unit) based **
 */
#define PERF_FORMAT					    \
	PERF_FORMAT_GROUP | PERF_FORMAT_ID |		    \
	PERF_FORMAT_TOTAL_TIME_ENABLED |		    \
	PERF_FORMAT_TOTAL_TIME_RUNNING

struct raw_perf_event {
	uint64_t  config;  /* event */
	uint64_t  config1; /* umask */
	struct perf_event *save;
	char *    desc;
};

/* if HT is enable a maximum of 4 events (5 if one is instructions
 * retired can be specified, if HT is disabled a maximum of 8 (9 if
 * one is instructions retired) can be specified.
 *
 * From Table 19-1. Architectural Performance Events
 * Architectures Software Developer’s Manual Volume 3: System Programming Guide
 */
struct raw_perf_event perf_events[] = {
	{ 0x3c, 0x00, NULL, "Unhalted CPU Cycles" },
	{ 0xc0, 0x00, NULL, "Instruction Retired" }
};

#define NUM_EVTS (sizeof(perf_events)/sizeof(struct raw_perf_event))

bool time_bench_PMU_config(bool enable)
{
	int i;
	struct perf_event_attr perf_conf;
	struct perf_event *perf_event;
	int cpu;

	preempt_disable();
	cpu = smp_processor_id();
	pr_info("DEBUG: cpu:%d\n", cpu);
	preempt_enable();

	memset(&perf_conf, 0, sizeof(struct perf_event_attr));
	perf_conf.type           = PERF_TYPE_RAW;
	perf_conf.size           = sizeof(struct perf_event_attr);
	perf_conf.read_format    = PERF_FORMAT;
	perf_conf.pinned         = 1;
	perf_conf.exclude_user   = 1; /* No userspace events */
	perf_conf.exclude_kernel = 0; /* Only kernel events */

	for (i =0; i < NUM_EVTS; i++) {
		perf_conf.disabled = enable;
		//perf_conf.disabled = (i == 0) ? 1 : 0;
		perf_conf.config   = perf_events[i].config;
		perf_conf.config1  = perf_events[i].config1;
		if (verbose)
			pr_info("%s() enable PMU counter: %s\n",
				__func__, perf_events[i].desc);
		perf_event = perf_event_create_kernel_counter(&perf_conf, cpu,
						 NULL /* task */,
						 NULL /* overflow_handler*/,
						 NULL /* context */);
		if (perf_event) {
			perf_events[i].save = perf_event;
			pr_info("%s():DEBUG perf_event success\n", __func__);

			perf_event_enable(perf_event);
		} else {
			pr_info("%s():DEBUG perf_event is NULL\n", __func__);
		}
	}

	return true;
}
EXPORT_SYMBOL_GPL(time_bench_PMU_config);

//perf_event_create_kernel_counter()

/** Generic functions **
 */

/* Calculate stats, store results in record */
bool time_bench_calc_stats(struct time_bench_record *rec)
{
#define NANOSEC_PER_SEC 1000000000 /* 10^9 */
	uint64_t ns_per_call_tmp_rem   = 0;
	uint32_t ns_per_call_remainder = 0;
	uint64_t pmc_ipc_tmp_rem   = 0;
	uint32_t pmc_ipc_remainder = 0;
	uint32_t pmc_ipc_div = 0;
	uint32_t invoked_cnt_precision = 0;
	uint32_t invoked_cnt = 0; /* 32-bit due to div_u64_rem() */

	if (rec->flags & TIME_BENCH_LOOP) {
		if (rec->invoked_cnt < 1000) {
			pr_err("ERR: need more(>1000) loops(%llu) for timing\n",
			       rec->invoked_cnt);
			return false;
		}
		if (rec->invoked_cnt > ((1ULL<<32)-1)) {
			/* div_u64_rem() can only support div with 32bit*/
			pr_err("ERR: Invoke cnt(%llu) too big overflow 32bit\n",
			       rec->invoked_cnt);
			return false;
		}
		invoked_cnt = (uint32_t)rec->invoked_cnt;
	}

	/* TSC (Time-Stamp Counter) records */
	if (rec->flags & TIME_BENCH_TSC) {
		rec->tsc_interval = rec->tsc_stop - rec->tsc_start;
		if (rec->tsc_interval == 0) {
			pr_err("ABORT: timing took ZERO TSC time\n");
			return false;
		}
		/* Calculate stats */
		if (rec->flags & TIME_BENCH_LOOP)
			rec->tsc_cycles = rec->tsc_interval / invoked_cnt;
		else
			rec->tsc_cycles = rec->tsc_interval;
	}

	/* Wall-clock time calc */
	if (rec->flags & TIME_BENCH_WALLCLOCK) {
		rec->time_start = rec->ts_start.tv_nsec +
			(NANOSEC_PER_SEC * rec->ts_start.tv_sec);
		rec->time_stop  = rec->ts_stop.tv_nsec +
			(NANOSEC_PER_SEC * rec->ts_stop.tv_sec);
		rec->time_interval = rec->time_stop - rec->time_start;
		if (rec->time_interval == 0) {
			pr_err("ABORT: timing took ZERO wallclock time\n");
			return false;
		}
		/* Calculate stats */
		/*** Division in kernel it tricky ***/
		/* Orig: time_sec = (time_interval / NANOSEC_PER_SEC); */
		/* remainder only correct because NANOSEC_PER_SEC is 10^9 */
		rec->time_sec = div_u64_rem(rec->time_interval, NANOSEC_PER_SEC,
			      &rec->time_sec_remainder);
		//TODO: use existing struct timespec records instead of div?

		if (rec->flags & TIME_BENCH_LOOP) {
			/*** Division in kernel it tricky ***/
			/* Orig: ns = ((double)time_interval / invoked_cnt); */
			/* First get quotient */
			rec->ns_per_call_quotient =
				div_u64_rem(rec->time_interval,
					    invoked_cnt,
					    &ns_per_call_remainder);
			/* Now get decimals .xxx precision (incorrect roundup)*/
			ns_per_call_tmp_rem = ns_per_call_remainder;
			invoked_cnt_precision = invoked_cnt/1000;
			if (invoked_cnt_precision > 0) {
				rec->ns_per_call_decimal =
					div_u64_rem(ns_per_call_tmp_rem,
						    invoked_cnt_precision,
						    &ns_per_call_remainder);
			}
		}
	}

	/* Performance Monitor Unit (PMU) counters */
	if (rec->flags & TIME_BENCH_PMU) {
		//FIXME: Overflow handling???
		rec->pmc_inst = rec->pmc_inst_stop - rec->pmc_inst_start;
		rec->pmc_clk  = rec->pmc_clk_stop  - rec->pmc_clk_start;

		/* Calc Instruction Per Cycle (IPC) */
		/* First get quotient */
		rec->pmc_ipc_quotient =
			div_u64_rem(rec->pmc_inst, rec->pmc_clk,
				    &pmc_ipc_remainder);
		/* Now get decimals .xxx precision (incorrect roundup)*/
 		pmc_ipc_tmp_rem = pmc_ipc_remainder;
		pmc_ipc_div = rec->pmc_clk/1000;
		if (pmc_ipc_div > 0) {
			rec->pmc_ipc_decimal =
				div_u64_rem(pmc_ipc_tmp_rem, pmc_ipc_div,
					    &pmc_ipc_remainder);
		}
	}

	return true;
}

/* Generic function for invoking a loop function and calculating
 * execution time stats.  The function being called/timed is assumed
 * to perform a tight loop, and update the timing record struct.
 */
bool time_bench_loop(uint32_t loops, int step, char *txt, void *data,
		     int (*func)(struct time_bench_record *record, void *data)
	)
{
	struct time_bench_record rec;

	/* Setup record */
	memset(&rec, 0, sizeof(rec)); /* zero func might not update all */
	rec.version_abi = 1;
	rec.loops       = loops;
	rec.step        = step;
	//rec.flags       = (TIME_BENCH_LOOP|TIME_BENCH_TSC|TIME_BENCH_WALLCLOCK);
	rec.flags       = (TIME_BENCH_LOOP|TIME_BENCH_TSC|\
			   TIME_BENCH_WALLCLOCK|TIME_BENCH_PMU);
	//TODO: Add/copy txt to rec

	/*** Loop function being timed ***/
	if (!func(&rec, data)) {
		pr_err("ABORT: function being timed failed\n");
		return false;
	}

	if (rec.invoked_cnt < loops)
		pr_warn("WARNING: Invoke count(%llu) smaller than loops(%d)\n",
			rec.invoked_cnt, loops);

	/* Calculate stats */
	time_bench_calc_stats(&rec);

	//TODO: Add flags test?? hardcoded above...
	pr_info("Type:%s Per elem: %llu cycles(tsc) %llu.%03llu ns (step:%d)"
		" - (measurement period time:%llu.%09u sec time_interval:%llu)"
		" - (invoke count:%llu tsc_interval:%llu)\n",
		txt, rec.tsc_cycles,
		 rec.ns_per_call_quotient, rec.ns_per_call_decimal, rec.step,
		rec.time_sec, rec.time_sec_remainder, rec.time_interval,
		rec.invoked_cnt, rec.tsc_interval);
/*	pr_info("DEBUG check is %llu/%llu == %llu.%03llu ?\n",
		rec.time_interval, rec.invoked_cnt,
		rec.ns_per_call_quotient, rec.ns_per_call_decimal);
*/
	if (rec.flags & TIME_BENCH_PMU) {
		pr_info("Type:%s PMU inst/clock"
			"%llu/%llu = %llu.%03llu IPC (inst per cycle)\n",
			txt, rec.pmc_inst, rec.pmc_clk,
			rec.pmc_ipc_quotient, rec.pmc_ipc_decimal);
	}
	return true;
}
EXPORT_SYMBOL_GPL(time_bench_loop);

static int __init time_bench_module_init(void)
{
	if (verbose)
		pr_info("Loaded\n");
	//time_bench_PMU_config(true);
	return 0;
}
module_init(time_bench_module_init);

static void __exit time_bench_module_exit(void)
{
	if (verbose)
		pr_info("Unloaded\n");
}
module_exit(time_bench_module_exit);

MODULE_DESCRIPTION("Benchmarking code execution time inside the kernel");
MODULE_AUTHOR("Jesper Dangaard Brouer <netoptimizer@brouer.com>");
MODULE_LICENSE("GPL");