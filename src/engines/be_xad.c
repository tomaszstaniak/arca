/* SPDX-License-Identifier: MIT */
/* XAD engine: Amiga and legacy formats, read only. Uses the program's own
 * static XAD (xad_static.c), never a system xadmaster.library. Data goes out
 * through an output hook into the AROS file layer, like every other engine. */
#include <stdio.h>
#include <string.h>
#include <aros/asmcall.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/xadmaster.h>
#include "../core/ua.h"
#include "xad_static.h"

struct xadMasterBase *xadMasterBase;

static ULONG xd_caps(ua_format f)
{
    switch (f) {
    case UA_FMT_LHA: case UA_FMT_LZX:
        return UA_CAP_LIST | UA_CAP_EXTRACT | UA_CAP_AMIGA_PROT | UA_CAP_COMMENTS;
    case UA_FMT_DMS:
        return UA_CAP_LIST | UA_CAP_DISKIMAGE;
    case UA_FMT_ZIP:
        /* XAD reads zip local headers, so it can also read a zip whose central
         * directory is gone. Its names are ISO-8859-1 only. */
        return UA_CAP_LIST | UA_CAP_EXTRACT | UA_CAP_RECOVERY;
    case UA_FMT_TAR: case UA_FMT_GZIP: case UA_FMT_BZIP2:
    case UA_FMT_UNKNOWN:
        return UA_CAP_LIST | UA_CAP_EXTRACT | UA_CAP_AMIGA_PROT | UA_CAP_COMMENTS;
    default:
        return 0;
    }
}

static struct xadArchiveInfo *xd_open(const char *path, int *unknown, char *err, int errlen)
{
    struct xadArchiveInfo *ai;
    LONG e;

    if (!xadMasterBase && !(xadMasterBase = xad_static_open())) {
        snprintf(err, errlen, "XAD initialisation failed");
        return NULL;
    }
    ai = xadAllocObjectA(XADOBJ_ARCHIVEINFO, NULL);
    if (!ai) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    e = xadGetInfo(ai, XAD_INFILENAME, (IPTR)path, TAG_DONE);
    if (e) {
        if (unknown)
            *unknown = e == XADERR_FILETYPE;
        snprintf(err, errlen, "%s", xadGetErrorText(e));
        xadFreeObjectA(ai, NULL);
        return NULL;
    }
    return ai;
}

static void xd_close(struct xadArchiveInfo *ai)
{
    xadFreeInfo(ai);
    xadFreeObjectA(ai, NULL);
}

static int xd_list(const char *path, ua_list *out, char *err, int errlen)
{
    int unknown = 0;
    struct xadArchiveInfo *ai;
    struct xadFileInfo *fi;

    /* Volume sets go through libarchive; XAD is given single files only. */
    if (out->volumes) {
        snprintf(err, errlen, "multi-volume sets are not read with XAD");
        return UA_LIST_UNKNOWN;
    }
    ai = xd_open(path, &unknown, err, errlen);

    if (!ai)
        return unknown ? UA_LIST_UNKNOWN : UA_LIST_FAILED;
    if (!ai->xai_FileInfo && ai->xai_DiskInfo)
        out->disk_image = 1;
    /* XAD reads the whole archive in xadGetInfo above; only the copy into
     * our list can be interrupted here. */
    for (fi = ai->xai_FileInfo; fi; fi = fi->xfi_Next) {
        ua_entry *e = ua_list_add(out);
        if (!e)
            break;
        if (ua_list_break(out)) {
            xd_close(ai);
            return UA_LIST_CANCELLED;
        }
        e->name_src = ua_strdup(fi->xfi_FileName ? (char *)fi->xfi_FileName : "");
        e->name_kind = UA_NAME_HOST;      /* XAD hands out ISO-8859-1 */
        if (fi->xfi_Flags & XADFIF_DIRECTORY)
            e->type = UA_ET_DIR;
        else if (fi->xfi_Flags & XADFIF_LINK)
            e->type = UA_ET_LINK;
        else
            e->type = UA_ET_FILE;
        e->size = fi->xfi_Size;
        e->size_valid = 1;
        e->encrypted = (fi->xfi_Flags & XADFIF_CRYPTED) != 0;
        e->prot = fi->xfi_Protection;    /* same layout as FIB protection */
        e->prot_valid = 1;
        if (fi->xfi_Comment && *fi->xfi_Comment)
            e->comment = ua_strdup((char *)fi->xfi_Comment);
        if (!(fi->xfi_Flags & XADFIF_NODATE) &&
            !xadConvertDates(XAD_DATEXADDATE, (IPTR)&fi->xfi_Date,
                             XAD_GETDATEDATESTAMP, (IPTR)&e->date, TAG_DONE))
            e->date_valid = 1;
    }
    /* XAD reports damage it met while scanning in xai_Flags, not as an error. */
    if (ai->xai_Flags & XADAIF_FILECORRUPT) {
        snprintf(err, errlen, "archive is damaged");
        xd_close(ai);
        return out->n ? UA_LIST_PARTIAL : UA_LIST_FAILED;
    }
    xd_close(ai);
    return UA_LIST_OK;
}

struct outctx {
    ua_sink *s;
    int cancelled;
};

AROS_UFH3(static ULONG, outhook,
          AROS_UFHA(struct Hook *, h, A0),
          AROS_UFHA(struct xadArchiveInfo *, ai, A2),
          AROS_UFHA(struct xadHookParam *, p, A1))
{
    AROS_USERFUNC_INIT
    struct outctx *c = h->h_Data;
    (void)ai;
    switch (p->xhp_Command) {
    case XADHC_INIT:
        p->xhp_DataPos = 0;
        return 0;
    case XADHC_WRITE:
        if (c->s->data(c->s, p->xhp_BufferPtr, (ULONG)p->xhp_BufferSize)) {
            c->cancelled = 1;
            return XADERR_BREAK;
        }
        p->xhp_DataPos += p->xhp_BufferSize;
        return 0;
    case XADHC_SEEK:
        if ((xadSignSize)p->xhp_CommandData < 0)
            return XADERR_NOTSUPPORTED;
        if (p->xhp_CommandData && c->s->seek(c->s, (UQUAD)p->xhp_CommandData)) {
            c->cancelled = 1;
            return XADERR_BREAK;
        }
        p->xhp_DataPos += p->xhp_CommandData;
        return 0;
    case XADHC_FREE:
    case XADHC_ABORT:
        return 0;   /* the file layer owns the file and its cleanup */
    default:
        return XADERR_NOTSUPPORTED;
    }
    AROS_USERFUNC_EXIT
}

static int xd_extract(const char *path, ua_list *l, ua_sink *s, char *err, int errlen)
{
    struct xadArchiveInfo *ai = xd_open(path, NULL, err, errlen);
    struct xadFileInfo *fi;
    struct outctx c = { s, 0 };
    struct Hook hook;
    ULONG i = 0;
    int result = 0;

    if (!ai)
        return -1;
    memset(&hook, 0, sizeof(hook));
    hook.h_Entry = (HOOKFUNC)outhook;
    hook.h_Data = &c;
    for (fi = ai->xai_FileInfo; fi && i < l->n; fi = fi->xfi_Next) {
        ua_entry *e = &l->v[i++];
        ua_wdecision d;
        LONG xe;
        ua_etype t = (fi->xfi_Flags & XADFIF_DIRECTORY) ? UA_ET_DIR :
                     (fi->xfi_Flags & XADFIF_LINK) ? UA_ET_LINK : UA_ET_FILE;
        if (!ua_entry_matches(e, fi->xfi_FileName ? (char *)fi->xfi_FileName : "", t, 1,
                              (UQUAD)fi->xfi_Size)) {
            snprintf(err, errlen, "%s (entry %lu is not \"%s\")", UA_CHANGED_MSG,
                     (unsigned long)i, e->name_src ? e->name_src : "");
            result = -1;
            break;
        }
        d = s->begin(s, e);
        if (d == UA_W_CANCEL) {
            result = 1;
            break;
        }
        if (d == UA_W_SKIP)
            continue;
        xe = xadFileUnArc(ai, XAD_ENTRYNUMBER, fi->xfi_EntryNumber,
                          XAD_OUTHOOK, (IPTR)&hook, TAG_DONE);
        if (c.cancelled) {
            s->end(s, e, 0, "cancelled");
            result = 1;
            break;
        }
        s->end(s, e, xe == 0, xe ? (const char *)xadGetErrorText(xe) : NULL);
    }
    if (result == 0 && (i < l->n || (fi && !l->partial))) {
        snprintf(err, errlen, "%s (%s entries than were listed)", UA_CHANGED_MSG,
                 i < l->n ? "fewer" : "more");
        result = -1;
    }
    xd_close(ai);
    return result;
}

const ua_engine ua_engine_xad = { "xad", xd_caps, xd_list, xd_extract };
