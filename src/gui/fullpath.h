/* SPDX-License-Identifier: MIT */
/* fullpath: a name made absolute in the process that owns its current drawer
 * and program drawer, before another process (the worker, which has neither)
 * or the settings keep it. */
#ifndef UA_FULLPATH_H
#define UA_FULLPATH_H

#include <stddef.h>

/* 1 with the absolute name in out; 0 when it cannot be made, with the reason
 * in fp_why. The name as given must then not be used. */
int full_path(const char *in, char *out, size_t n);

extern char fp_why[120];
/* Tests only (UnarcTestNoFullPath): every name that needs resolving fails. */
extern int fp_test_fail;

#endif
