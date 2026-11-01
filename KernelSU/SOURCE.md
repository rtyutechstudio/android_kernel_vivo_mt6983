# Fixed BakaSU kernel source and X80 SUSFS port

- Upstream repository: https://github.com/Baka-SU/BakaSU.git
- Exact upstream release tag: `v4.1.0`
- Exact upstream commit: `0d27e685cb9f1b873cd334371c0d8b6ea4c3aea9`
- Imported subtree: upstream `kernel/` → `KernelSU/kernel/` (no nested Git repository).
- Setup: upstream `kernel/setup.sh` was read, **not executed**. Its driver symlink, Kconfig source and Makefile wiring were reproduced without clone/pull/checkout.
- SUSFS source: official `gki-android12-5.10` at `c3358dd9f0eb46917d76d2e14de5e3c1970b8bee`, version `v2.3.0`.

## Offline version metadata

The original Kbuild queries the latest SukiSU release and current branch commit count online, which do not describe this pinned BakaSU source. Those network queries and Git fetches are removed. The complete version is `v4.1.0-0d27e685@pinned`.

The input repository is shallow (depth 1), so its local commit count is **not** an upstream build number. Numeric `13000` is the original upstream offline fallback and is identified as such in build output; it is not claimed to be the release's historical commit-derived version code. The true release identity is the tag and hash above.

## Real manual hook port

The official SUSFS `10_enable_susfs_for_ksu.patch` targets a newer split `core/`, `feature/`, `hook/`, `manager/` source layout. A complete unmodified dry run was recorded in the integration diagnostics and failed; this import does not pretend the patch applied cleanly. Applicable interfaces are manually ported to the real v4.1.0 implementation:

- Built-in AArch64 SUSFS Kconfig constraints and complete official SUSFS feature menu.
- Source-level filename exec/stat/access hooks, paired post-exec driver fd, init.rc read/fstat, safe-mode input, reboot fd and root-only SUSFS command dispatch.
- `susfs_init`, actual domain SID caching, zygote/zygote_next thread flags, existing per-app unmount policy, deferred SUSFS work and sdcard-monitor boot lifecycle.
- Existing SELinux policy engine and original AVC/type tracking for SUSFS, without disabling SELinux enforcement or compiler control-flow protection.
- Native two-slot `dynamic_manager` retained; certificate hashes remain mandatory, with bounded format/length checks and rejection of an unset/all-zero dynamic certificate. SukiSU keeps its fixed size `0x35c` / SHA-256 `947ae944f3de4ed4c21a7e4f7953ecf351bfa2b36239da37a34111ad29993eef`. The native second manager defaults to the actual official KernelSU certificate (size `0x033b`, SHA-256 `c371061b19d8c7d7d6133c6a9bafe198fa944e50c1b31c9d8daa8d7f1fc2d2d6`); authorized SET/CLEAR and persistence remain supported. Changing trust anchors clears authenticated dynamic UIDs and forces APK revalidation.

The KernelSU certificate anchor and bounded APK parsing helpers are sourced from official `tiann/KernelSU` **v3.3.1**, not guessed from memory or substituted with an all-zero hash:

- https://api.github.com/repos/tiann/KernelSU/contents/kernel/Kbuild?ref=v3.3.1 (default size/hash)
- https://api.github.com/repos/tiann/KernelSU/contents/kernel/manager/apk_sign.c?ref=v3.3.1 (bounded signature-block parser)

The parser integration retains certificate SHA-256 comparison, rejects malformed/truncated/out-of-bounds blocks and unsupported signing schemes, and keeps the real static/dynamic trust-store checks. No manager APK binary was supplied locally, so source-anchor verification must not be misreported as an on-device APK/ABI test.

This older release has **no preinit facility or standalone newer SELinux-hide/status-page feature**. They are not represented by fake functions or fake state. Kernel-side references to those separate facilities must be gated on genuine feature availability.

The upstream `embed_ksud.c` is an empty CI placeholder with no consumers in this release. It is not built or shipped here; no fake embedded daemon is supplied. The real daemon path remains `/data/adb/ksud`, invoked by the upstream init.rc integration. Installation of a compatible userspace daemon/manager is a separate requirement.

## Validation boundary

Object compilation, whole-tree linking and runtime validation are separate steps. Compilation success does not prove compatibility of a manager userspace ioctl ABI, boot initialization or device operation. No device is flashed by this integration.
