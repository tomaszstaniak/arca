/* SPDX-License-Identifier: MIT */
/* Host test of the drag-out rules in src/core/plan.c: the common folders of
 * the entries to be extracted are removed, and names that meet afterwards are
 * numbered.
 *   cc -I tests/host/shim -o /tmp/ph tests/host/plan_host.c && /tmp/ph */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/core/plan.c"

char *ua_strdup(const char *s) { return s ? strdup(s) : NULL; }
/* Only ua_plan_make maps names; these tests start from targets. */
char *ua_map_component(const char *c, int kind, ULONG *flags)
{
    (void)kind, (void)flags;
    return strdup(c);
}

static int fails;

/* names: targets, '!' first = not extracted; want: the result, "" for excluded */
static void check(const char *what, const char **names, int n, const char **want, ULONG coll)
{
    ua_list l;
    ua_plan p;
    int i, bad = 0;
    memset(&l, 0, sizeof(l));
    memset(&p, 0, sizeof(p));
    l.v = calloc(n, sizeof(ua_entry));
    l.n = n;
    for (i = 0; i < n; i++) {
        const char *s = names[i];
        size_t k = strlen(s);
        l.v[i].excluded = s[0] == '!';
        if (l.v[i].excluded)
            s++, k--;
        l.v[i].type = k && s[k - 1] == '/' ? UA_ET_DIR : UA_ET_FILE;
        l.v[i].target = strdup(s);
        if (l.v[i].type == UA_ET_DIR)
            l.v[i].target[k - 1] = 0;
    }
    if (ua_plan_strip_common(&l) || ua_plan_unique(&l, &p))
        bad = 1;
    for (i = 0; i < n && !bad; i++)
        if (!l.v[i].excluded && strcmp(l.v[i].target, want[i]))
            bad = 1;
    if (p.collisions != coll)
        bad = 1;
    printf("%s %s:", bad ? "FAIL" : "ok  ", what);
    for (i = 0; i < n; i++)
        printf(" %s", l.v[i].excluded ? "-" : l.v[i].target);
    printf(" (collisions %lu)\n", (unsigned long)p.collisions);
    fails += bad;
    for (i = 0; i < n; i++)
        free(l.v[i].target);
    free(l.v);
}

#define N(a) (int)(sizeof(a) / sizeof(a[0]))
int main(void)
{
    const char *one[] = {"dir/sub/deep.txt"}, *one_w[] = {"deep.txt"};
    const char *two[] = {"a/x.txt", "b/x.txt"}, *two_w[] = {"a/x.txt", "b/x.txt"};
    const char *same[] = {"a/b/x.txt", "a/b/c/y.txt"}, *same_w[] = {"x.txt", "c/y.txt"};
    const char *folder[] = {"docs/", "docs/one.txt", "docs/sub/", "docs/sub/two.txt"},
               *folder_w[] = {"docs", "docs/one.txt", "docs/sub", "docs/sub/two.txt"};
    const char *deep[] = {"!top.txt", "p/docs/", "p/docs/one.txt"},
               *deep_w[] = {"", "docs", "docs/one.txt"};
    const char *all[] = {"big/d000/e0.txt", "big/d000/e1.txt", "big/d001/e2.txt"},
               *all_w[] = {"d000/e0.txt", "d000/e1.txt", "d001/e2.txt"};
    const char *part[] = {"ab/x.txt", "a/y.txt"}, *part_w[] = {"ab/x.txt", "a/y.txt"};
    const char *top[] = {"top.txt", "a/x.txt"}, *top_w[] = {"top.txt", "a/x.txt"};
    const char *meet[] = {"a/x.txt", "!b/", "a/X.txt"}, *meet_w[] = {"x.txt", "", "X 2.txt"};
    check("one file", one, N(one), one_w, 0);
    check("two folders", two, N(two), two_w, 0);
    check("shared parents", same, N(same), same_w, 0);
    check("selected folder", folder, N(folder), folder_w, 0);
    check("folder deeper, other excluded", deep, N(deep), deep_w, 0);
    check("everything under one top", all, N(all), all_w, 0);
    check("prefix of a name is not a folder", part, N(part), part_w, 0);
    check("one at the top", top, N(top), top_w, 0);
    check("case-only collision", meet, N(meet), meet_w, 1);
    return fails != 0;
}
