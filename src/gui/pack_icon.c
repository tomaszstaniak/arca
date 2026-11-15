/* SPDX-License-Identifier: MIT */
/* The file being packed does not exist yet: request named system DefIcons
 * rather than asking icon.library to identify an imaginary file. */
#include <proto/exec.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <proto/utility.h>
#include <libraries/mui.h>
#include <workbench/icon.h>
#include <clib/alib_protos.h>
#include <aros/asmcall.h>
#include "pack_icon.h"

#define UA_ICON_FORMAT (TAG_USER | 0x55410001)
#define NFORMATS 4
#define PREVIEW_MIN 112
#define ICON_SIZE 96

struct preview_data {
    struct DiskObject *icons[NFORMATS];
    BOOL laid[NFORMATS];
    int format;
};
static struct MUI_CustomClass *preview_class;

struct DiskObject *ua_default_icon(const char *const *names, LONG type)
{
    struct DiskObject *icon;
    if (names)
        for (; *names; names++) {
            icon = GetIconTags(NULL, ICONGETA_GetDefaultName, (IPTR)*names,
                               ICONGETA_RemapIcon, FALSE, TAG_DONE);
            if (icon)
                return icon;
        }
    return GetIconTags(NULL, ICONGETA_GetDefaultType, type,
                       ICONGETA_RemapIcon, FALSE, TAG_DONE);
}

static void load_icons(struct preview_data *d)
{
    static const char *const names[NFORMATS][6] = {
        { "LHA", "LhArc", "Archive", NULL },
        { "ZIP", "Archive", NULL },
        { "7z", "7zip", "7-zip", "Archive", NULL },
        { "TGZ", "TAR.GZ", "GZIP", "GZ", "Archive", NULL }
    };
    int i;
    for (i = 0; i < NFORMATS; i++) {
        if (d->icons[i]) continue;
        d->icons[i] = ua_default_icon(names[i], WBPROJECT);
        if (d->icons[i]) {
            /* Only this preview is enlarged; do not change global icon settings. */
            struct TagItem tags[] = {
                { ICONCTRLA_SetScaleBox, PACK_ICON_SCALEBOX(ICON_SIZE, ICON_SIZE) },
                { TAG_DONE, 0 }
            };
            IconControlA(d->icons[i], tags);
        }
    }
}

static struct Rectangle icon_rectangle(struct DiskObject *icon)
{
    struct Rectangle rect = { 0, 0, -1, -1 };
    struct TagItem tags[] = {
        { ICONDRAWA_Borderless, TRUE },
        { ICONDRAWA_Frameless, TRUE },
        { TAG_DONE, 0 }
    };
    if (icon) GetIconRectangleA(NULL, icon, NULL, &rect, tags);
    return rect;
}

/* Screen-specific objects live only as long as the setup. FreeDiskObject
 * releases their layout as well; never keep an unmapped scaled object for a
 * subsequent setup. Reloading also picks up icon-set changes between opens. */
static void free_icons(struct preview_data *d)
{
    int i;
    for (i = 0; i < NFORMATS; i++) {
        if (d->icons[i]) FreeDiskObject(d->icons[i]);
        d->icons[i] = NULL;
        d->laid[i] = FALSE;
    }
}

/* All formats reserve the same space. Different icon sets and differently
 * sized format icons must not resize the window when the choice changes. */
static void preview_size(struct preview_data *d, int *w, int *h)
{
    int i;
    *w = *h = PREVIEW_MIN;
    for (i = 0; i < NFORMATS; i++)
        if (d->icons[i]) {
            struct Rectangle rect = icon_rectangle(d->icons[i]);
            int iw = rect.MaxX - rect.MinX + 1, ih = rect.MaxY - rect.MinY + 1;
            if (iw > *w) *w = iw;
            if (ih > *h) *h = ih;
        }
}

BOOPSI_DISPATCHER(IPTR, preview_dispatcher, cl, obj, msg)
{
    struct preview_data *d;
    int i;
    switch (msg->MethodID) {
    case OM_NEW: {
        Object *o = (Object *)DoSuperMethodA(cl, obj, msg);
        if (o) {
            d = INST_DATA(cl, o);
            load_icons(d);
        }
        return (IPTR)o;
    }
    case OM_DISPOSE:
        d = INST_DATA(cl, obj);
        free_icons(d);
        break;
    case OM_SET: {
        struct TagItem *tag;
        struct TagItem *state = ((struct opSet *)msg)->ops_AttrList;
        IPTR result = DoSuperMethodA(cl, obj, msg);
        d = INST_DATA(cl, obj);
        while ((tag = NextTagItem(&state)))
            if (tag->ti_Tag == UA_ICON_FORMAT && tag->ti_Data < NFORMATS &&
                d->format != (int)tag->ti_Data) {
                d->format = (int)tag->ti_Data;
                MUI_Redraw(obj, MADF_DRAWOBJECT);
            }
        return result;
    }
    case MUIM_Setup:
        if (!DoSuperMethodA(cl, obj, msg)) return FALSE;
        d = INST_DATA(cl, obj);
        load_icons(d);
        for (i = 0; i < NFORMATS; i++)
            if (d->icons[i]) d->laid[i] = LayoutIconA(d->icons[i], _screen(obj), NULL);
        return TRUE;
    case MUIM_Cleanup:
        d = INST_DATA(cl, obj);
        free_icons(d);
        break;
    case MUIM_AskMinMax: {
        IPTR result = DoSuperMethodA(cl, obj, msg);
        struct MUI_MinMax *mm = ((struct MUIP_AskMinMax *)msg)->MinMaxInfo;
        int w, h;
        d = INST_DATA(cl, obj);
        preview_size(d, &w, &h);
        mm->MinWidth += w; mm->DefWidth += w; mm->MaxWidth += w;
        mm->MinHeight += h; mm->DefHeight += h; mm->MaxHeight += h;
        return result;
    }
    case MUIM_Draw: {
        IPTR result = DoSuperMethodA(cl, obj, msg);
        struct DiskObject *icon;
        d = INST_DATA(cl, obj);
        icon = d->icons[d->format];
        if (icon && d->laid[d->format] &&
            (((struct MUIP_Draw *)msg)->flags & MADF_DRAWOBJECT)) {
            struct TagItem tags[] = {
                { ICONDRAWA_Borderless, TRUE },
                { ICONDRAWA_Frameless, TRUE },
                { ICONDRAWA_EraseBackground, FALSE },
                { ICONDRAWA_DrawInfo, (IPTR)_dri(obj) },
                { TAG_DONE, 0 }
            };
            struct Rectangle rect = icon_rectangle(icon);
            int w = rect.MaxX - rect.MinX + 1, h = rect.MaxY - rect.MinY + 1;
            DrawIconStateA(_rp(obj), icon, NULL,
                _mleft(obj) + (_mwidth(obj) - w) / 2 - rect.MinX,
                _mtop(obj) + (_mheight(obj) - h) / 2 - rect.MinY,
                IDS_NORMAL, tags);
        }
        return result;
    }
    }
    return DoSuperMethodA(cl, obj, msg);
}
BOOPSI_DISPATCHER_END

int ua_pack_icon_init(void)
{
    if (!preview_class)
        preview_class = MUI_CreateCustomClass(NULL, MUIC_Area, NULL,
            sizeof(struct preview_data), (APTR)preview_dispatcher);
    return preview_class != NULL;
}

void ua_pack_icon_exit(void)
{
    if (preview_class) {
        MUI_DeleteCustomClass(preview_class);
        preview_class = NULL;
    }
}

Object *ua_pack_icon_new(void)
{
    if (!preview_class) return NULL;
    return NewObject(preview_class->mcc_Class, NULL, MUIA_FillArea, TRUE, TAG_DONE);
}

void ua_pack_icon_set(Object *obj, int format)
{
    if (obj && format >= 0 && format < NFORMATS)
        SetAttrs(obj, UA_ICON_FORMAT, format, TAG_DONE);
}
