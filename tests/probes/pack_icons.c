/* SPDX-License-Identifier: MIT */
/* Native regression: named DefIcons, missing-name fallback and repeated preview
 * switches across window cleanup/setup. No changes to existing user icons. */
#include <stdio.h>
#include <string.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/icon.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <workbench/icon.h>
#include <libraries/mui.h>
#include <clib/alib_protos.h>
#include "../../src/gui/pack_icon.h"

static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)

int main(void)
{
    char key[80], path[120], info[128];
    const char *names[3];
    struct DiskObject *fixture, *icon, *expected;
    Object *app = NULL, *win = NULL, *preview;
    int round, f;
    snprintf(key, sizeof(key), "ArcaIconTest-%p", FindTask(NULL));
    snprintf(path, sizeof(path), "ENV:Sys/def_%s", key);
    snprintf(info, sizeof(info), "%s.info", path);
    fixture = GetDefDiskObject(WBPROJECT);
    CHECK(fixture != NULL);
    if (fixture) {
        STRPTR oldtool = fixture->do_DefaultTool;
        fixture->do_DefaultTool = (STRPTR)"ARCA_ICON_FIXTURE";
        CHECK(PutDiskObject(path, fixture));
        fixture->do_DefaultTool = oldtool;
        FreeDiskObject(fixture);
        names[0] = "ArcaIconTest-missing";
        names[1] = key;
        names[2] = NULL;
        icon = ua_default_icon(names, WBPROJECT);
        CHECK(icon && icon->do_DefaultTool && !strcmp(icon->do_DefaultTool, "ARCA_ICON_FIXTURE"));
        if (icon) FreeDiskObject(icon);
        names[0] = key;
        names[1] = "Archive";
        icon = ua_default_icon(names, WBPROJECT);
        CHECK(icon && icon->do_DefaultTool && !strcmp(icon->do_DefaultTool, "ARCA_ICON_FIXTURE"));
        if (icon) FreeDiskObject(icon);
        CHECK(DeleteFile(info));
    }
    names[0] = "ArcaIconTest-missing";
    names[1] = NULL;
    icon = ua_default_icon(names, WBDRAWER);
    expected = GetIconTags(NULL, ICONGETA_GetDefaultType, WBDRAWER,
                           ICONGETA_RemapIcon, FALSE, TAG_DONE);
    CHECK(icon && expected);
    if (icon && expected) {
        /* A theme's def_Drawer may itself carry project/zero metadata.
         * Compare with the configured system default, not an assumed type. */
        CHECK(icon->do_Type == expected->do_Type);
        CHECK(icon->do_Gadget.Width == expected->do_Gadget.Width);
        CHECK(icon->do_Gadget.Height == expected->do_Gadget.Height);
    }
    if (expected) FreeDiskObject(expected);
    if (icon) FreeDiskObject(icon);
    CHECK(ua_pack_icon_init());
    preview = ua_pack_icon_new();
    CHECK(preview != NULL);
    if (preview) {
        app = ApplicationObject,
            MUIA_Application_Title, (IPTR)"Arca icon regression",
            SubWindow, (IPTR)(win = WindowObject,
                MUIA_Window_Title, (IPTR)"Arca icon regression",
                WindowContents, (IPTR)(VGroup, Child, (IPTR)preview, End),
            End),
        End;
        CHECK(app != NULL);
        if (!app) MUI_DisposeObject(preview);
    }
    if (app) {
        for (round = 0; round < 3; round++) {
            struct Window *iw = NULL;
            IPTR opened = 0;
            WORD width = 0, height = 0;
            SetAttrs(win, MUIA_Window_Open, TRUE, TAG_DONE);
            GetAttr(MUIA_Window_Open, win, &opened);
            CHECK(opened);
            GetAttr(MUIA_Window_Window, win, (IPTR *)&iw);
            if (iw) { width = iw->Width; height = iw->Height; }
            for (f = 0; f < 40; f++) {
                ULONG sigs = 0;
                ua_pack_icon_set(preview, f % 4);
                DoMethod(app, MUIM_Application_NewInput, (IPTR)&sigs);
                if (iw) CHECK(iw->Width == width && iw->Height == height);
                Delay(1);
            }
            /* Out-of-range choices must be safe as well. */
            ua_pack_icon_set(preview, -1);
            ua_pack_icon_set(preview, 4);
            SetAttrs(win, MUIA_Window_Open, FALSE, TAG_DONE);
        }
        MUI_DisposeObject(app);
    }
    ua_pack_icon_exit();
    printf("pack-icons: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 20 : 0;
}
