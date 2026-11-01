#ifndef __KSU_H_SELINUX
#define __KSU_H_SELINUX

#include "linux/types.h"
#include "linux/version.h"
#include "linux/cred.h"

// TODO: rename to "ksu"
#define KERNEL_SU_DOMAIN "su"
#define KERNEL_SU_FILE "ksu_file"

#define KERNEL_SU_CONTEXT "u:r:" KERNEL_SU_DOMAIN ":s0"
#define KSU_FILE_CONTEXT "u:object_r:" KERNEL_SU_FILE ":s0"

void setup_selinux(const char *, struct cred *);

void setenforce(bool);

bool getenforce();

bool is_task_ksu_domain(const struct cred* cred);

bool is_ksu_domain();

bool is_zygote(const struct cred* cred);

bool is_init(const struct cred* cred);

void apply_kernelsu_rules();

u32 ksu_get_ksu_file_sid();

int handle_sepolicy(unsigned long arg3, void __user *arg4);

void setup_ksu_cred();

#ifdef CONFIG_KSU_SUSFS
bool susfs_is_sid_equal(const struct cred *cred, u32 sid);
u32 susfs_get_sid_from_name(const char *secctx_name);
u32 susfs_get_current_sid(void);
void susfs_set_batch_sid(void);
bool susfs_is_current_zygote_domain(void);
bool susfs_is_current_zygote_next_domain(void);
bool susfs_is_current_ksu_domain(void);
bool susfs_is_current_init_domain(void);
extern u32 susfs_ksu_sid, susfs_init_sid, susfs_zygote_sid;
extern u32 susfs_zygote_next_sid, susfs_priv_app_sid;
#endif

#endif
