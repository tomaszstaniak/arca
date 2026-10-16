/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* XAD linked into the program instead of opened as xadmaster.library.
 *
 * XAD's own code calls its library functions through the library base's jump
 * table (clients do xadAllocVec(XADM ...) via the base), so a static build
 * still needs a base with a vector table in front of it. This file builds one,
 * fills it with the functions compiled from the contrib sources, and runs the
 * same initialisation as the library's Init(), minus the scan of LIBS:XAD for
 * external clients: a static build carries only the built-in clients.
 *
 * Licence: LGPL-2.1-or-later, like the XAD sources it initialises. */
#include <stdlib.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include "functions.h"
#include "version.h"
#include "xad_static.h"

#define NVEC 32   /* LVOs 5..31 are used; 0..4 are the library vectors */

/* The compiled library functions (AROS_LH names: xadmaster_<lvo>_<name>). */
#define F(n, name) extern void xadmaster_##n##_##name(void);
F(5, xadAllocObjectA)  F(6, xadFreeObjectA)   F(7, xadRecogFileA)
F(8, xadGetInfoA)      F(9, xadFreeInfo)      F(10, xadFileUnArcA)
F(11, xadDiskUnArcA)   F(12, xadGetErrorText) F(13, xadGetClientInfo)
F(14, xadHookAccess)   F(15, xadConvertDatesA) F(16, xadCalcCRC16)
F(17, xadCalcCRC32)    F(18, xadAllocVec)     F(19, xadCopyMem)
F(20, xadHookTagAccessA) F(21, xadConvertProtectionA) F(22, xadGetDiskInfoA)
F(23, xadGetHookAccessA) F(24, xadFreeHookAccessA) F(25, xadAddFileEntryA)
F(26, xadAddDiskEntryA) F(27, xadGetFilenameA) F(28, xadConvertNameA)
F(29, xadGetDefaultNameA) F(30, xadGetSystemInfo) F(31, xadDiskFileUnArcA)
#undef F

static void *const vectors[NVEC] = {
    [5] = xadmaster_5_xadAllocObjectA,   [6] = xadmaster_6_xadFreeObjectA,
    [7] = xadmaster_7_xadRecogFileA,     [8] = xadmaster_8_xadGetInfoA,
    [9] = xadmaster_9_xadFreeInfo,       [10] = xadmaster_10_xadFileUnArcA,
    [11] = xadmaster_11_xadDiskUnArcA,   [12] = xadmaster_12_xadGetErrorText,
    [13] = xadmaster_13_xadGetClientInfo, [14] = xadmaster_14_xadHookAccess,
    [15] = xadmaster_15_xadConvertDatesA, [16] = xadmaster_16_xadCalcCRC16,
    [17] = xadmaster_17_xadCalcCRC32,    [18] = xadmaster_18_xadAllocVec,
    [19] = xadmaster_19_xadCopyMem,      [20] = xadmaster_20_xadHookTagAccessA,
    [21] = xadmaster_21_xadConvertProtectionA, [22] = xadmaster_22_xadGetDiskInfoA,
    [23] = xadmaster_23_xadGetHookAccessA, [24] = xadmaster_24_xadFreeHookAccessA,
    [25] = xadmaster_25_xadAddFileEntryA, [26] = xadmaster_26_xadAddDiskEntryA,
    [27] = xadmaster_27_xadGetFilenameA, [28] = xadmaster_28_xadConvertNameA,
    [29] = xadmaster_29_xadGetDefaultNameA, [30] = xadmaster_30_xadGetSystemInfo,
    [31] = xadmaster_31_xadDiskFileUnArcA,
};

static void *block;
static struct xadMasterBaseP *base;

struct xadMasterBase *xad_static_open(void)
{
    const struct xadClient *cl;
    xadSize minsize = 0;
    void **vec;
    int i;

    if (base)
        return (struct xadMasterBase *)base;
    block = calloc(1, NVEC * sizeof(void *) + sizeof(struct xadMasterBaseP));
    if (!block)
        return NULL;
    vec = block;
    /* Library base follows the table; LVO n is the n-th pointer below it. */
    base = (struct xadMasterBaseP *)(vec + NVEC);
    for (i = 1; i < NVEC; i++)
        vec[NVEC - i] = vectors[i];

    base->xmb_LibNode.lib_Node.ln_Name = "xadmaster.library (static)";
    base->xmb_LibNode.lib_Version = XADMASTERVERSION;
    base->xmb_LibNode.lib_Revision = XADMASTERREVISION;
    base->xmb_LibNode.lib_NegSize = NVEC * sizeof(void *);
    base->xmb_LibNode.lib_PosSize = sizeof(struct xadMasterBaseP);
    base->xmb_DefaultName = "unnamed.dat";
    base->xmb_InHookFH.h_Entry = (HOOKFUNC)InHookFH;
    base->xmb_OutHookFH.h_Entry = (HOOKFUNC)OutHookFH;
    base->xmb_InHookMem.h_Entry = (HOOKFUNC)InHookMem;
    base->xmb_OutHookMem.h_Entry = (HOOKFUNC)OutHookMem;
    base->xmb_InHookStream.h_Entry = (HOOKFUNC)InHookStream;
    base->xmb_OutHookStream.h_Entry = (HOOKFUNC)OutHookStream;
    base->xmb_InHookDisk.h_Entry = (HOOKFUNC)InHookDisk;
    base->xmb_OutHookDisk.h_Entry = (HOOKFUNC)OutHookDisk;
    base->xmb_InHookSplitted.h_Entry = (HOOKFUNC)InHookSplitted;
    base->xmb_InHookDiskArc.h_Entry = (HOOKFUNC)InHookDiskArc;
    base->xmb_System.xsi_Version = XADMASTERVERSION;
    base->xmb_System.xsi_Revision = XADMASTERREVISION;

    base->xmb_DOSBase = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 37);
    base->xmb_UtilityBase = (struct UtilityBase *)OpenLibrary((CONST_STRPTR)"utility.library", 37);
    if (!base->xmb_DOSBase || !base->xmb_UtilityBase) {
        xad_static_close();
        return NULL;
    }
    xadAddClients(base, RealFirstClient, 0);
    for (cl = base->xmb_FirstClient; cl; cl = cl->xc_Next)
        if (cl->xc_RecogSize > minsize)
            minsize = cl->xc_RecogSize;
    base->xmb_RecogSize = minsize;
    base->xmb_System.xsi_RecogSize = minsize;
    MakeCRC16(base->xmb_CRCTable1, XADCRC16_ID1);
    MakeCRC32(base->xmb_CRCTable2, XADCRC32_ID1);
    return (struct xadMasterBase *)base;
}

void xad_static_close(void)
{
    if (!base)
        return;
    xadFreeClients(base);
    if (base->xmb_UtilityBase)
        CloseLibrary((struct Library *)base->xmb_UtilityBase);
    if (base->xmb_DOSBase)
        CloseLibrary((struct Library *)base->xmb_DOSBase);
    free(block);
    block = NULL;
    base = NULL;
}
