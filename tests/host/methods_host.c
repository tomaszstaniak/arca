/* SPDX-License-Identifier: MIT */
/* Host test of src/core/methods.c (with bigfile.c), on the DOS shim of
 * tests/host/shim. Driven by tests/host/test_methods.py. */
#include <stdio.h>
#include <string.h>
#include "../../src/core/bigfile.c"
#include "../../src/core/methods.c"

int main(int argc, char **argv)
{
    int i;
    if (argc == 3 && !strcmp(argv[1], "stamp")) {
        ua_stamp st;
        int ok = ua_stamp_file(argv[2], &st);
        printf("ok=%d size=%llu crc=%08lx/%08lx day=%ld min=%ld tick=%ld\n", ok,
               (unsigned long long)st.size, (unsigned long)st.crc_head, (unsigned long)st.crc_tail,
               (long)st.date.ds_Days, (long)st.date.ds_Minute, (long)st.date.ds_Tick);
        return 0;
    }
    for (i = 2; i < argc; i++) {
        ua_needs n;
        int ok = ua_needs_read(argv[i], !strcmp(argv[1], "7z") ? UA_FMT_7Z : UA_FMT_ZIP, &n);
        printf("%s: ok=%d list_enc=%d data_enc=%d multi=%d unsupported='%s' seeks=%ld\n", argv[i],
               ok, n.list_encrypted, n.data_encrypted, n.multi_disk, n.unsupported, shim_seek_calls);
    }
    return 0;
}
