#!/usr/bin/env python3
"""Compile read-only layout probes with the kernel's recorded object flags."""
import pathlib
import re
import shlex
import subprocess

import os
ROOT = pathlib.Path(__file__).resolve().parents[1]
command = (ROOT / 'mm/.vivo_x80_compat.o.cmd').read_text().splitlines()[0]
args = shlex.split(command.split(' := ', 1)[1])
args[0] = str(pathlib.Path(os.environ['CLANG_DIR']) / 'bin/clang')
filtered = []
skip = False
for arg in args:
    if skip:
        skip = False
        continue
    if arg == '-o':
        skip = True
        continue
    if arg in ('-c', 'mm/vivo_x80_compat.c') or arg.startswith('-Wp,-MMD,'):
        continue
    filtered.append(arg)

expected = {
    'zone_size': ('sizeof(struct zone)', 0x880),
    'zone_name': ('offsetof(struct zone, name)', 0x80),
    'zone_thp_blocks': ('offsetof(struct zone, thp_pool_reserved_blocks)', 0x78),
    'zone_initialized': ('offsetof(struct zone, initialized)', 0x98),
    'zone_free_area': ('offsetof(struct zone, free_area)', 0xc0),
    'zone_flags': ('offsetof(struct zone, flags)', 0x748),
    'zone_lock': ('offsetof(struct zone, lock)', 0x750),
    'zone_vm_stat': ('offsetof(struct zone, vm_stat)', 0x7c0),
    'free_area_size': ('sizeof(struct free_area)', 0x98),
    'zonelists': ('offsetof(struct pglist_data, node_zonelists)', 0x1980),
    'nr_zones': ('offsetof(struct pglist_data, nr_zones)', 0x19c0),
    'node_id': ('offsetof(struct pglist_data, node_id)', 0x19e0),
    'migrate_types': ('MIGRATE_TYPES', 9),
    'zone_stats': ('NR_VM_ZONE_STAT_ITEMS', 17),
    'pageblock_bits': ('NR_PAGEBLOCK_BITS', 8),
    'migration_mask': ('MIGRATETYPE_MASK', 0x7f),
    # Shipped system_heap_refill_worker inline accesses.
    'page_size': ('sizeof(struct page)', 64),
    'compound_order': ('sizeof(struct page) + offsetof(struct page, compound_order)', 81),
    'page_head_flag': ('PG_head', 16),
    'task_state': ('offsetof(struct task_struct, state)', 48),
    # Stock proc IO output, submit_bio, cpuset/cgroup/futex and LSM accesses.
    'task_pid': ('offsetof(struct task_struct, pid)', 1480),
    'task_comm': ('offsetof(struct task_struct, comm)', 1936),
    'task_ioac': ('offsetof(struct task_struct, ioac)', 2280),
    'task_ioac_size': ('sizeof(struct task_io_accounting)', 112),
    'task_read_bytes': ('offsetof(struct task_struct, ioac.read_bytes)', 2320),
    'task_thread_read': ('offsetof(struct task_struct, ioac.thread_read_bytes)', 2328),
    'task_write_bytes': ('offsetof(struct task_struct, ioac.write_bytes)', 2336),
    'task_thread_write': ('offsetof(struct task_struct, ioac.thread_write_bytes)', 2344),
    'task_last_read': ('offsetof(struct task_struct, ioac.last_read_bytes)', 2352),
    'task_last_write': ('offsetof(struct task_struct, ioac.last_write_bytes)', 2360),
    'task_read_stamp': ('offsetof(struct task_struct, ioac.last_read_time_ns)', 2368),
    'task_write_stamp': ('offsetof(struct task_struct, ioac.last_write_time_ns)', 2376),
    'task_cancelled': ('offsetof(struct task_struct, ioac.cancelled_write_bytes)', 2384),
    'task_mems': ('offsetof(struct task_struct, mems_allowed)', 2424),
    'task_cgroups': ('offsetof(struct task_struct, cgroups)', 2448),
    'task_cg_list': ('offsetof(struct task_struct, cg_list)', 2456),
    'task_robust': ('offsetof(struct task_struct, robust_list)', 2472),
    'task_perf': ('offsetof(struct task_struct, perf_event_ctxp)', 2568),
    'task_rcu': ('offsetof(struct task_struct, rcu)', 2648),
    'task_splice': ('offsetof(struct task_struct, splice_pipe)', 2664),
    'task_delays': ('offsetof(struct task_struct, delays)', 2688),
    'task_security': ('offsetof(struct task_struct, security)', 2824),
    'task_vendor': ('offsetof(struct task_struct, android_vendor_data1)', 2832),
    'task_pem_tg': ('offsetof(struct task_struct, android_vendor_data1[1])', 2840),
    'task_oem': ('offsetof(struct task_struct, android_oem_data1)', 3344),
    'task_oem_size': ('sizeof(((struct task_struct *)0)->android_oem_data1)', 512),
    'task_thread': ('offsetof(struct task_struct, thread.cpu_context)', 3920),
    'task_size': ('sizeof(struct task_struct)', 5056),
    'signal_read_bytes': ('offsetof(struct signal_struct, ioac.read_bytes)', 648),
    'signal_write_bytes': ('offsetof(struct signal_struct, ioac.write_bytes)', 664),
    'signal_cancelled': ('offsetof(struct signal_struct, ioac.cancelled_write_bytes)', 712),
    'controller_count': ('CGROUP_SUBSYS_COUNT', 8),
    'controller_cpu': ('cpu_cgrp_id', 1),
    'controller_cpuacct': ('cpuacct_cgrp_id', 2),
    'controller_iolimit': ('iolimit_cgrp_id', 7),
    'cgroup_subsys': ('offsetof(struct cgroup, subsys)', 440),
    'cgroup_root': ('offsetof(struct cgroup, root)', 504),
    'root_cpu_css': ('offsetof(struct cgroup_root, cgrp.subsys[cpu_cgrp_id])', 464),
    # Stock Image and the actual vendor-ramdisk vkyber-iosched.ko.
    'elevator_ops_size': ('sizeof(struct elevator_mq_ops)', 0xd8),
    'elevator_check_priority': ('offsetof(struct elevator_type, ops.check_priority)', 0xb8),
    'elevator_icq_size': ('offsetof(struct elevator_type, icq_size)', 0xe0),
    'elevator_attrs': ('offsetof(struct elevator_type, elevator_attrs)', 0xf0),
    'elevator_name': ('offsetof(struct elevator_type, elevator_name)', 0xf8),
    'elevator_alias': ('offsetof(struct elevator_type, elevator_alias)', 0x100),
    'elevator_features': ('offsetof(struct elevator_type, elevator_features)', 0x108),
    'elevator_owner': ('offsetof(struct elevator_type, elevator_owner)', 0x110),
    'elevator_list': ('offsetof(struct elevator_type, list)', 0x140),
    'elevator_type_size': ('sizeof(struct elevator_type)', 0x160),
}
source = '#include <linux/mm.h>\n#include <linux/mmzone.h>\n#include <linux/sched/signal.h>\n#include <linux/cgroup.h>\n#include <linux/blkdev.h>\n#include <linux/elevator.h>\n'
source += ''.join('unsigned long audit_%s = %s;\n' % (name, expr)
                  for name, (expr, value) in expected.items())
result = subprocess.run(filtered + ['-x', 'c', '-S', '-emit-llvm', '-o', '-', '-'],
                        cwd=str(ROOT), input=source, universal_newlines=True,
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
values = {name: int(value) for name, value in re.findall(
    r'^@audit_(\w+) = .*?global i64 (\d+),', result.stdout, re.MULTILINE)}
failed = []
for name, (expr, value) in expected.items():
    actual = values.get(name)
    print('%s %s: actual=%s expected=%#x' %
          ('PASS' if actual == value else 'FAIL', name,
           hex(actual) if actual is not None else 'missing', value))
    if actual != value:
        failed.append(name)
if failed:
    raise SystemExit('ABI mismatch: ' + ', '.join(failed))
print('Verified measured zone/pageblock and task ABI anchors; unmeasured stock ABI is not certified.')
