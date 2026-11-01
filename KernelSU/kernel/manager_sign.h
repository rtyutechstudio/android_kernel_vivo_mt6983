#ifndef MANAGER_SIGN_H
#define MANAGER_SIGN_H

// ShirkNeko/SukiSU
#define EXPECTED_SIZE_SHIRKNEKO   0x35c
#define EXPECTED_HASH_SHIRKNEKO   "947ae944f3de4ed4c21a7e4f7953ecf351bfa2b36239da37a34111ad29993eef"

/* tiann/KernelSU official Kbuild trust anchor (not a generated placeholder). */
#define EXPECTED_SIZE_KERNELSU 0x033b
#define EXPECTED_HASH_KERNELSU "c371061b19d8c7d7d6133c6a9bafe198fa944e50c1b31c9d8daa8d7f1fc2d2d6"

/* The native second/dynamic manager defaults to the official KernelSU cert. */
#define EXPECTED_SIZE_OTHER EXPECTED_SIZE_KERNELSU
#define EXPECTED_HASH_OTHER EXPECTED_HASH_KERNELSU

typedef struct {
    unsigned size;
    const char *sha256;
} apk_sign_key_t;

#endif /* MANAGER_SIGN_H */
