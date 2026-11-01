#ifndef __KSU_H_SUCOMPAT
#define __KSU_H_SUCOMPAT
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/jump_label.h>

extern struct static_key_true ksu_su_compat_enabled;

void ksu_sucompat_init(void);
void ksu_sucompat_exit(void);

#ifdef CONFIG_KSU_MANUAL_HOOK
int ksu_handle_faccessat(int *, struct filename **, int *, int *);
int ksu_handle_stat(int *, struct filename **, int *);
int ksu_handle_execveat_sucompat(int *, struct filename **, void *, void *, int *);
int ksu_handle_post_execveat_sucompat(int *, struct filename **, void *, void *,
                                    int *, int *);
int ksu_handle_execveat(int *, struct filename **, void *, void *, int *);
#else
// Handler functions exported for hook_manager
int ksu_handle_faccessat(int *dfd, const char __user **filename_user,
			 int *mode, int *__unused_flags);
int ksu_handle_stat(int *dfd, const char __user **filename_user, int *flags);
int ksu_handle_execve_sucompat(const char __user **filename_user,
			       void *__never_use_argv, void *__never_use_envp,
			       int *__never_use_flags);

#endif

#endif