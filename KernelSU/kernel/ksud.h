#ifndef __KSU_H_KSUD
#define __KSU_H_KSUD

#include <linux/fs.h>
#include <linux/jump_label.h>

struct user_arg_ptr;
int ksu_handle_execveat_ksud(int *, struct filename **, struct user_arg_ptr *,
                            struct user_arg_ptr *, int *);
void ksu_handle_sys_read(unsigned int, char __user **, size_t *);
void ksu_handle_vfs_fstat(int, loff_t *);
extern struct static_key_true ksu_is_init_rc_hook_enabled;
extern struct static_key_true ksu_is_input_hook_enabled;

#define KSUD_PATH "/data/adb/ksud"

void ksu_ksud_init();
void ksu_ksud_exit();

void on_post_fs_data(void);
void on_module_mounted(void);
void on_boot_completed(void);

bool ksu_is_safe_mode(void);

int nuke_ext4_sysfs(const char* mnt);

extern u32 ksu_file_sid;
extern bool ksu_module_mounted;
extern bool ksu_boot_completed;

#endif
