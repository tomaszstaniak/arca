/* SPDX-License-Identifier: MIT */
#ifndef UA_PACK_ICON_H
#define UA_PACK_ICON_H

#include <exec/types.h>
#include <intuition/classusr.h>
#include <workbench/workbench.h>

/* First available named system DefIcon, then the built-in/type default.
 * names is NULL or NULL-terminated, without def_ or .info. The caller owns
 * the result and releases it with FreeDiskObject(). Never writes icons. */
struct DiskObject *ua_default_icon(const char *const *names, LONG type);

/* Packing preview in the GUI's LhA, ZIP, 7z, tar.gz order. Each object owns
 * its four icons. Dispose all objects before ua_pack_icon_exit(). */
int ua_pack_icon_init(void);
void ua_pack_icon_exit(void);
Object *ua_pack_icon_new(void);
void ua_pack_icon_set(Object *obj, int format);

#endif
