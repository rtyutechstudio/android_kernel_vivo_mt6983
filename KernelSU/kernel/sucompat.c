#include <linux/compiler_types.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/uaccess.h>
#include <asm/current.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/sched/task_stack.h>
#include <linux/ptrace.h>
#include <linux/jump_label.h>
#include <linux/namei.h>
#include <linux/fs_struct.h>
#include <linux/susfs_def.h>
#include "selinux/selinux.h"
#include "supercalls.h"

#include "allowlist.h"
#include "feature.h"
#include "klog.h" // IWYU pragma: keep
#include "ksud.h"
#include "sucompat.h"
#include "app_profile.h"
#include "util.h"

#include "sulog.h"

#define SU_PATH "/system/bin/su"
#define SH_PATH "/system/bin/sh"

DEFINE_STATIC_KEY_TRUE(ksu_su_compat_enabled);

static int su_compat_feature_get(u64 *value)
{
    *value = static_branch_likely(&ksu_su_compat_enabled) ? 1 : 0;
    return 0;
}

static int su_compat_feature_set(u64 value)
{
    bool enable = value != 0;
    if (enable)
        static_branch_enable(&ksu_su_compat_enabled);
    else
        static_branch_disable(&ksu_su_compat_enabled);
    pr_info("su_compat: set to %d\n", enable);
    return 0;
}

static const struct ksu_feature_handler su_compat_handler = {
    .feature_id = KSU_FEATURE_SU_COMPAT,
    .name = "su_compat",
    .get_handler = su_compat_feature_get,
    .set_handler = su_compat_feature_set,
};

static void __user *userspace_stack_buffer(const void *d, size_t len)
{
    // To avoid having to mmap a page in userspace, just write below the stack
    // pointer.
    char __user *p = (void __user *)current_user_stack_pointer() - len;

    return copy_to_user(p, d, len) ? NULL : p;
}

static char __user *sh_user_path(void)
{
    static const char sh_path[] = "/system/bin/sh";

    return userspace_stack_buffer(sh_path, sizeof(sh_path));
}

static char __user *ksud_user_path(void)
{
    static const char ksud_path[] = KSUD_PATH;

    return userspace_stack_buffer(ksud_path, sizeof(ksud_path));
}

#ifdef CONFIG_KSU_MANUAL_HOOK
/* Source-level hooks operate on kernel-owned filenames, not user pointers. */
static bool sucompat_allowed(void)
{
    return static_branch_likely(&ksu_su_compat_enabled) &&
           ksu_is_allow_uid_for_current(current_uid().val) &&
           !current_chrooted();
}

int ksu_handle_faccessat(int *dfd, struct filename **filename, int *mode,
                       int *unused_flags)
{
    if (filename && !IS_ERR_OR_NULL(*filename) && sucompat_allowed() &&
        !strcmp((*filename)->name, SU_PATH))
        memcpy((char *)(*filename)->name, SH_PATH, sizeof(SH_PATH));
    return 0;
}

int ksu_handle_stat(int *dfd, struct filename **filename, int *flags)
{
    return ksu_handle_faccessat(dfd, filename, NULL, flags);
}

int ksu_handle_execveat_sucompat(int *fd, struct filename **filename,
                               void *argv, void *envp, int *flags)
{
    struct filename *replacement;
    int ret;

    if (!filename || IS_ERR_OR_NULL(*filename))
        return -EINVAL;

    /* init's children must enter the KSU domain before running the real ksud. */
    if (current->pid != 1 && is_init(current_cred())) {
        if (!strcmp((*filename)->name, KSUD_PATH)) {
            escape_to_root_for_init();
            return -EINVAL; /* not a user SU session */
        }
#ifdef CONFIG_KSU_SUSFS
        if (!strstr((*filename)->name, "/app_process") &&
            !strstr((*filename)->name, "/adbd") &&
            !strstr((*filename)->name, "/stub_zygote"))
            susfs_set_current_proc_no_su();
#endif
    }

    if (!sucompat_allowed() || strcmp((*filename)->name, SU_PATH))
        return -EINVAL;

    /* getname_kernel gives exec its own correctly-sized, refcounted filename. */
    replacement = getname_kernel(KSUD_PATH);
    if (IS_ERR(replacement))
        return PTR_ERR(replacement);
    ret = escape_with_root_profile();
    if (ret) {
        putname(replacement);
        return ret;
    }
    putname(*filename);
    *filename = replacement;
#ifdef CONFIG_KSU_SUSFS
    susfs_clear_current_proc_no_su();
#endif
#if __SULOG_GATE
    ksu_sulog_report_su_attempt(current_uid().val, NULL, SU_PATH, true);
#endif
    return 0;
}

int ksu_handle_post_execveat_sucompat(int *fd, struct filename **filename,
                                    void *argv, void *envp, int *flags,
                                    int *retval)
{
    /* Caller invokes this only for a paired, successfully prepared SU exec. */
    if (retval && *retval >= 0) {
        int session_fd = ksu_install_fd();
        if (session_fd < 0)
            pr_warn("SU exec driver fd installation failed: %d\n", session_fd);
    }
    return 0;
}

int ksu_handle_execveat(int *fd, struct filename **filename, void *argv,
                       void *envp, int *flags)
{
    ksu_handle_execveat_ksud(fd, filename, argv, envp, flags);
    return ksu_handle_execveat_sucompat(fd, filename, argv, envp, flags);
}

#else
int ksu_handle_faccessat(int *dfd, const char __user **filename_user, int *mode,
                         int *__unused_flags)
{
    const char su[] = SU_PATH;

    if (!ksu_is_allow_uid_for_current(current_uid().val)) {
        return 0;
    }

    char path[sizeof(su) + 1];
    memset(path, 0, sizeof(path));
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (unlikely(!memcmp(path, su, sizeof(su)))) {
#if __SULOG_GATE
        ksu_sulog_report_syscall(current_uid().val, NULL, "faccessat", path);
#endif
        pr_info("faccessat su->sh!\n");
        *filename_user = sh_user_path();
    }

    return 0;
}

int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags)
{
    // const char sh[] = SH_PATH;
    const char su[] = SU_PATH;

    if (!ksu_is_allow_uid_for_current(current_uid().val)) {
        return 0;
    }

    if (unlikely(!filename_user)) {
        return 0;
    }

    char path[sizeof(su) + 1];
    memset(path, 0, sizeof(path));
    strncpy_from_user_nofault(path, *filename_user, sizeof(path));

    if (unlikely(!memcmp(path, su, sizeof(su)))) {
#if __SULOG_GATE
        ksu_sulog_report_syscall(current_uid().val, NULL, "newfstatat", path);
#endif
        pr_info("newfstatat su->sh!\n");
        *filename_user = sh_user_path();
    }

    return 0;
}

int ksu_handle_execve_sucompat(const char __user **filename_user,
                   void *__never_use_argv, void *__never_use_envp,
                   int *__never_use_flags)
{
    const char su[] = SU_PATH;
    const char __user *fn;
    char path[sizeof(su) + 1];
    long ret;
    unsigned long addr;

    if (unlikely(!filename_user))
        return 0;

#if __SULOG_GATE
    bool is_allowed = ksu_is_allow_uid_for_current(current_uid().val);
    ksu_sulog_report_syscall(current_uid().val, NULL, "execve", path);
    
    if (!is_allowed)
        return 0;

    ksu_sulog_report_su_attempt(current_uid().val, NULL, path, is_allowed);
#else
    if (!ksu_is_allow_uid_for_current(current_uid().val)) {
        return 0;
    }
#endif

    addr = untagged_addr((unsigned long)*filename_user);
    fn = (const char __user *)addr;
    memset(path, 0, sizeof(path));
    ret = strncpy_from_user_nofault(path, fn, sizeof(path));

    if (ret < 0 && try_set_access_flag(addr)) {
        ret = strncpy_from_user_nofault(path, fn, sizeof(path));
    }

    if (ret < 0 && preempt_count()) {
        /* This is crazy, but we know what we are doing:
         * Temporarily exit atomic context to handle page faults, then restore it */
        pr_info("Access filename failed, try rescue..\n");
        preempt_enable_no_resched_notrace();
        ret = strncpy_from_user(path, fn, sizeof(path));
        preempt_disable_notrace();
    }

    if (ret < 0) {
        pr_warn("Access filename when execve failed: %ld", ret);
        return 0;
    }

    if (likely(memcmp(path, su, sizeof(su))))
        return 0;

    pr_info("sys_execve su found\n");
    *filename_user = ksud_user_path();

    escape_with_root_profile();

    return 0;
}

#endif

// sucompat: permitted process can execute 'su' to gain root access.
void ksu_sucompat_init()
{
    if (ksu_register_feature_handler(&su_compat_handler)) {
        pr_err("Failed to register su_compat feature handler\n");
    }
}

void ksu_sucompat_exit()
{
    ksu_unregister_feature_handler(KSU_FEATURE_SU_COMPAT);
}