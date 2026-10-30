#!/bin/bash
# Pin CRCs required by the shipped vivo X80 vendor modules.
# CONFIG_MODVERSIONS compares these values when loading modules. The
# functions are compatibility stubs, so their source-side genksyms type
# expansion does not necessarily match the original private kernel types.
set -eu

KSrc=${1:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)}
export KSRC="$KSrc"
python3 - <<'PY'
import os
import re

root = os.environ['KSRC']
files = {
    'mm/vivo_x80_compat.o.symversions': {
        '__crc_alloc_pages_from_thp_pool': '0x49563db6',
        '__crc_is_thppool_page': '0x2e614662',
        '__crc_is_task_crucial': '0xec53bd78',
        '__crc_get_task_qos': '0x43bd6f07',
        '__crc_write_zcache_state': '0xd964cd99',
        # These already match naturally; keep them explicit for auditability.
        '__crc_rsc_qos_enable': '0xb2ec251f',
        '__crc_sysctl_allow_alloc_from_thppool': '0x1ada9da6',
        '__crc_nr_dmapages_in_thppool': '0xb97d7f91',
    },
    'block/blk-core.o.symversions': {
        '__crc___traceiter_block_rq_insert': '0x60bce35e',
        # __tracepoint_block_rq_insert is naturally 0xf4a00349.
    },
}

for rel, replacements in files.items():
    path = os.path.join(root, rel)
    if not os.path.isfile(path):
        raise SystemExit(f'missing generated symversions: {path}')
    text = open(path, encoding='utf-8').read()
    for name, value in replacements.items():
        line = f'{name} = {value};'
        pattern = rf'^{re.escape(name)}\s*=\s*0x[0-9a-fA-F]+;\s*$'
        text, count = re.subn(pattern, line, text, flags=re.MULTILINE)
        if count != 1:
            raise SystemExit(f'{name}: expected one generated entry in {rel}, got {count}')
    open(path, 'w', encoding='utf-8').write(text)
    print(f'pinned {rel}: {len(replacements)} CRC entries')
PY
