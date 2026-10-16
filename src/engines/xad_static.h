/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* XAD linked statically; see xad_static.c. LGPL-2.1-or-later. */
#ifndef XAD_STATIC_H
#define XAD_STATIC_H

struct xadMasterBase;

/* Returns the program's private XAD base, or NULL. Safe to call repeatedly.
 * Not shared between processes: each extraction worker uses the one base
 * from a single task at a time. */
struct xadMasterBase *xad_static_open(void);
void xad_static_close(void);

#endif
