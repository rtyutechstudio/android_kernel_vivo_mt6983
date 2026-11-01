#include <linux/compiler.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/task_work.h>
#include <linux/thread_info.h>
#include <linux/seccomp.h>
#include <linux/bpf.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/init_task.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/mount.h>
#include <linux/namei.h>
#include <linux/nsproxy.h>
#include <linux/path.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/security.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/uidgid.h>
#include <linux/version.h>
#include <linux/binfmts.h>
#include <linux/tty.h>
#ifdef CONFIG_KSU_SUSFS
#include <linux/susfs_def.h>
#include <linux/workqueue.h>
#endif

#include "allowlist.h"
#include "setuid_hook.h"
#include "feature.h"
#include "klog.h" // IWYU pragma: keep
#include "manager.h"
#include "selinux/selinux.h"
#include "seccomp_cache.h"
#include "supercalls.h"
#include "syscall_hook_manager.h"
#include "kernel_umount.h"
#include "app_profile.h"

static bool ksu_enhanced_security_enabled = false;

static int enhanced_security_feature_get(u64 *value)
{
    *value = ksu_enhanced_security_enabled ? 1 : 0;
    return 0;
}

static int enhanced_security_feature_set(u64 value)
{
    bool enable = value != 0;
    ksu_enhanced_security_enabled = enable;
    pr_info("enhanced_security: set to %d\n", enable);
    return 0;
}

static const struct ksu_feature_handler enhanced_security_handler = {
    .feature_id = KSU_FEATURE_ENHANCED_SECURITY,
    .name = "enhanced_security",
    .get_handler = enhanced_security_feature_get,
    .set_handler = enhanced_security_feature_set,
};

static inline bool is_allow_su()
{
    if (is_manager()) {
        // we are manager, allow!
        return true;
    }
    return ksu_is_allow_uid_for_current(current_uid().val);
}

#ifdef CONFIG_KSU_SUSFS
extern u32 susfs_zygote_sid;
extern u32 susfs_zygote_next_sid;
extern void disable_seccomp(void);
extern struct work_struct susfs_extra_works;

static inline void ksu_handle_extra_susfs_work(void)
{
    if (work_pending(&susfs_extra_works))
        return;

    schedule_work(&susfs_extra_works);
}

static int handle_zygote_setresuid(uid_t ruid) {
    // Check if spawned process is isolated service first, and force to do umount if so
    if (is_isolated_process(ruid)) {
        susfs_set_current_proc_no_su();
        susfs_set_current_proc_umounted();
        goto do_umount;
    }

    // - Since ksu maanger app uid is excluded in allow_list_arr, so ksu_uid_should_umount(manager_uid)
    //   will always return true, that's why we need to explicitly check if new_uid belongs to
    //   ksu manager.
    // - Disable seccomp restriction for KSU manager since running with "su" will disable seccomp anyway
    if (((ksu_is_manager_uid_valid() && ksu_get_manager_uid() == ruid) || ksu_is_any_manager(ruid))) {
        spin_lock_irq(&current->sighand->siglock);
        disable_seccomp();
        spin_unlock_irq(&current->sighand->siglock);
        pr_info("install fd for manager: %d\n", ruid);
        ksu_install_fd();
        return 0;
    }

    // - Check if spawned process is normal user app and needs to be umounted
    // - Now app_profile for webview_zygote is available in KernelSU manager
    if (likely(is_appuid(ruid) && ksu_uid_should_umount(ruid))) {
        susfs_set_current_proc_no_su();
        susfs_set_current_proc_umounted();
        goto do_umount;
    }

    // Disable seccomp restriction for root allowed apps since running with "su" will disable seccomp anyway
    if (ksu_is_allow_uid_for_current(ruid)) {
        spin_lock_irq(&current->sighand->siglock);
        disable_seccomp();
        spin_unlock_irq(&current->sighand->siglock);
        return 0;
    }

    // Process not umounted but also root not allowed
    susfs_set_current_proc_no_su();
    return 0;

do_umount:
    {
        // Handle kernel umount
        ksu_handle_umount(current_uid().val, ruid);

        // Handle extra susfs work
        ksu_handle_extra_susfs_work();
    }

    return 0;
}

static int handle_zygote_next_setresuid(uid_t ruid) {
    // Check if spawned process is isolated service first, and force to do umount if so
    if (is_isolated_process(ruid)) {
        susfs_set_current_proc_no_su();
        susfs_set_current_proc_umounted();
        susfs_set_current_proc_umounted_for_zygote_next();
        goto do_susfs_work;
    }

    // - Since ksu maanger app uid is excluded in allow_list_arr, so ksu_uid_should_umount(manager_uid)
    //   will always return true, that's why we need to explicitly check if new_uid belongs to
    //   ksu manager.
    // - Disable seccomp restriction for KSU manager since running with "su" will disable seccomp anyway
    if (((ksu_is_manager_uid_valid() && ksu_get_manager_uid() == ruid) || ksu_is_any_manager(ruid))) {
        spin_lock_irq(&current->sighand->siglock);
        disable_seccomp();
        spin_unlock_irq(&current->sighand->siglock);
        pr_info("install fd for manager: %d\n", ruid);
        ksu_install_fd();
        return 0;
    }

    // - Check if spawned process is normal user app and needs to be umounted
    // - Now app_profile for webview_zygote is available in KernelSU manager
    if (likely(is_appuid(ruid) && ksu_uid_should_umount(ruid))) {
        susfs_set_current_proc_no_su();
        susfs_set_current_proc_umounted();
        susfs_set_current_proc_umounted_for_zygote_next();
        goto do_susfs_work;
    }

    // Disable seccomp restriction for root allowed apps since running with "su" will disable seccomp anyway
    if (ksu_is_allow_uid_for_current(ruid)) {
        spin_lock_irq(&current->sighand->siglock);
        disable_seccomp();
        spin_unlock_irq(&current->sighand->siglock);
        return 0;
    }

    // Process not umounted but also root not allowed
    susfs_set_current_proc_no_su();
    return 0;

do_susfs_work:
    {
        // Do not umount here as we are in init namespace now

        // Handle extra susfs work
        ksu_handle_extra_susfs_work();
    }

    return 0;
}

int ksu_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    uid_t cur_uid = current_uid().val;

    /* Preserve v4.1.0's real enhanced-security feature in manual-hook mode. */
    if (cur_uid != 0 && ksu_enhanced_security_enabled) {
        if ((euid == 0 && !is_ksu_domain()) ||
            (is_appuid(cur_uid) && euid < current_euid().val &&
             !ksu_is_allow_uid_for_current(cur_uid))) {
            pr_warn("blocked suspicious setresuid from uid %u to %u\n",
                    cur_uid, euid);
            force_sig(SIGKILL);
        }
        return 0;
    }

    if (cur_uid != 0)
        return 0;

    // We only interest in process spwaned by zygote or zygote_next
    if (susfs_is_sid_equal(current_cred(), susfs_zygote_sid))
        return handle_zygote_setresuid(ruid);

    if (susfs_is_sid_equal(current_cred(), susfs_zygote_next_sid))
        return handle_zygote_next_setresuid(ruid);

    return 0;

}

#else
int ksu_handle_setresuid(uid_t ruid, uid_t euid, uid_t suid)
{
    uid_t new_uid = ruid;
	uid_t old_uid = current_uid().val;
    
    pr_info("handle_setresuid from %d to %d\n", old_uid, new_uid);

    // if old process is root, ignore it.
    if (old_uid != 0 && ksu_enhanced_security_enabled) {
        // disallow any non-ksu domain escalation from non-root to root!
        // euid is what we care about here as it controls permission
        if (unlikely(euid == 0)) {
            if (!is_ksu_domain()) {
                pr_warn("find suspicious EoP: %d %s, from %d to %d\n", 
                    current->pid, current->comm, old_uid, new_uid);
                force_sig(SIGKILL);
                return 0;
            }
        }
        // disallow appuid decrease to any other uid if it is not allowed to su
        if (is_appuid(old_uid)) {
            if (euid < current_euid().val && !ksu_is_allow_uid_for_current(old_uid)) {
                pr_warn("find suspicious EoP: %d %s, from %d to %d\n", 
                    current->pid, current->comm, old_uid, new_uid);
                force_sig(SIGKILL);
                return 0;
            }
        }
        return 0;
    }

    // if on private space, see if its possibly the manager
    if (new_uid > PER_USER_RANGE && new_uid % PER_USER_RANGE == ksu_get_manager_uid()) {
         ksu_set_manager_uid(new_uid);
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
    if (ksu_get_manager_uid() == new_uid) {
        pr_info("install fd for manager: %d\n", new_uid);
        ksu_install_fd();
        spin_lock_irq(&current->sighand->siglock);
        ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
        ksu_set_task_tracepoint_flag(current);
        spin_unlock_irq(&current->sighand->siglock);
        return 0;
    }

    if (ksu_is_allow_uid_for_current(new_uid)) {
        if (current->seccomp.mode == SECCOMP_MODE_FILTER &&
            current->seccomp.filter) {
            spin_lock_irq(&current->sighand->siglock);
            ksu_seccomp_allow_cache(current->seccomp.filter, __NR_reboot);
            spin_unlock_irq(&current->sighand->siglock);
        }
        ksu_set_task_tracepoint_flag(current);
    } else {
        ksu_clear_task_tracepoint_flag_if_needed(current);
    }
#else
    if (ksu_is_allow_uid_for_current(new_uid)) {
		spin_lock_irq(&current->sighand->siglock);
		disable_seccomp();
		spin_unlock_irq(&current->sighand->siglock);

		if (ksu_get_manager_uid() == new_uid) {
			pr_info("install fd for ksu manager(uid=%d)\n",
				new_uid);
			ksu_install_fd();
		}

		return 0;
	}
#endif

    // Handle kernel umount
    ksu_handle_umount(old_uid, new_uid);

    return 0;
}
#endif

void ksu_setuid_hook_init(void)
{
    ksu_kernel_umount_init();
    if (ksu_register_feature_handler(&enhanced_security_handler)) {
        pr_err("Failed to register enhanced security feature handler\n");
    }
}

void ksu_setuid_hook_exit(void)
{
    pr_info("ksu_core_exit\n");
    ksu_kernel_umount_exit();
    ksu_unregister_feature_handler(KSU_FEATURE_ENHANCED_SECURITY);
}
