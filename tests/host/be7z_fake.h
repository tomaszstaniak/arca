/* SPDX-License-Identifier: MIT */
/* Stand-in for the unarc-7z process in src/engines/be_7z.c, for
 * tests/host/be7z_host.c: the program's output is a byte string given by the
 * test. A line "~BREAK" in it marks where a Ctrl-C arrives: from there the
 * reader behaves as after a real one (cancelled set, the program told), and
 * the bytes after the marker are what the program writes afterwards.
 * child_end clears the structure as the real one does. */
struct child {
    UBYTE *buf;
    LONG len, pos;
    int eof, cancelled;
    char idxfile[80];
    const char *out;          /* the program's output */
    size_t outlen, given;
};

static void *FindTask(const char *n) { (void)n; return (void *)0x1234; }

static int have_prog = 1, prog_ok = 1;
static char prog_why[200];
static void find_prog_now(void) {}
static int find_prog(void) { return have_prog; }
static void check_version(void) {}
void ua_7z_init(void) {}

static const char *fake_out;
static size_t fake_len;
static int fake_pokes, fake_starts;
static char fake_args[256];

static void poke(child *c)
{
    (void)c;
    fake_pokes++;
}

static int child_start(child *c, const char *verb, const char *archive, const char *which,
                       const char *password, char *err, int errlen)
{
    (void)archive; (void)err; (void)errlen;
    memset(c, 0, sizeof(*c));
    c->buf = malloc(BUFSZ);
    c->out = fake_out;
    c->outlen = fake_len;
    fake_starts++;
    snprintf(fake_args, sizeof(fake_args), "%s %s %s", password ? "-p" : "", verb,
             which ? which : "");
    return 0;
}

static LONG child_read(child *c, void *buf, LONG len)
{
    const char *p = c->out + c->given, *mark;
    size_t left = c->outlen - c->given, n;
    if (!left)
        return 0;
    mark = memmem(p, left, "~BREAK\n", 7);
    if (mark == p) {
        /* Ctrl-C now: as in the real child_read. */
        c->given += 7;
        if (!c->cancelled) {
            c->cancelled = 1;
            poke(c);
        }
        p += 7;
        left -= 7;
        mark = memmem(p, left, "~BREAK\n", 7);
        if (!left)
            return 0;
    }
    n = mark ? (size_t)(mark - p) : left;
    if (n > (size_t)len)
        n = len;
    if (n > 7)
        n = n / 2;            /* short reads, as a pipe gives them */
    memcpy(buf, p, n);
    c->given += n;
    return (LONG)n;
}

static int child_end(child *c)
{
    int cancelled;
    c->pos = c->len = 0;
    while (fill(c))
        c->pos = c->len = 0;
    cancelled = c->cancelled;
    free(c->buf);
    memset(c, 0, sizeof(*c));
    return cancelled;
}
