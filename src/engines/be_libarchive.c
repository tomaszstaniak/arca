/* SPDX-License-Identifier: MIT */
/* libarchive engine: modern formats. Only the archive read API is used; files
 * reach the disk through the AROS file layer, never archive_write_disk. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/locale.h>
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include "../core/ua.h"

/* libarchive's "unrecognized format" errno, as archive_platform.h (private)
 * picks it. */
#ifdef EFTYPE
#define LA_ERRNO_FILE_FORMAT EFTYPE
#else
#define LA_ERRNO_FILE_FORMAT EILSEQ
#endif

static ULONG la_caps(ua_format f)
{
    switch (f) {
    case UA_FMT_ZIP:
        return UA_CAP_LIST | UA_CAP_EXTRACT | UA_CAP_UTF8_NAMES;
    case UA_FMT_7Z: case UA_FMT_TAR: case UA_FMT_GZIP: case UA_FMT_BZIP2:
    case UA_FMT_XZ: case UA_FMT_RAR: case UA_FMT_ISO9660: case UA_FMT_LHA:
    case UA_FMT_UNKNOWN:
        return UA_CAP_LIST | UA_CAP_EXTRACT;
    default:
        return 0;
    }
}

/* stream: the zip reader that walks local headers (archive_read_support_
 * format_zip_streamable) instead of the central directory. Used only for
 * recovery: it cannot see entries that the central directory alone describes. */
static struct archive *la_open(const char *path, char **vols, int stream, int *unknown,
                               char *err, int errlen)
{
    struct archive *a = archive_read_new();
    if (!a) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    /* An explicit list: the _all() variants also pull in external-program
     * filters, which need fork() and are of no use here. */
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_xz(a);
    archive_read_support_filter_lzma(a);
    archive_read_support_filter_compress(a);
    if (stream)
        archive_read_support_format_zip_streamable(a);
    else
        archive_read_support_format_zip(a);
    archive_read_support_format_7zip(a);
    archive_read_support_format_tar(a);
    archive_read_support_format_lha(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    archive_read_support_format_iso9660(a);
    archive_read_support_format_cab(a);
    archive_read_support_format_cpio(a);
    archive_read_support_format_ar(a);
    /* A multi-volume set is read as one stream across its files, in the
     * order volumes.c established. */
    if ((vols ? archive_read_open_filenames(a, (const char **)vols, 65536)
              : archive_read_open_filename(a, path, 65536)) != ARCHIVE_OK) {
        if (unknown)
            *unknown = archive_errno(a) == LA_ERRNO_FILE_FORMAT;
        snprintf(err, errlen, "%s", archive_error_string(a) ? archive_error_string(a)
                                                            : "not recognised");
        archive_read_free(a);
        return NULL;
    }
    return a;
}

static ULONG mode_to_prot(mode_t m)
{
    ULONG p = 0;     /* RWED bits are active low */
    if (!(m & S_IRUSR)) p |= FIBF_READ;
    if (!(m & S_IWUSR)) p |= FIBF_WRITE | FIBF_DELETE;
    if (!(m & S_IXUSR)) p |= FIBF_EXECUTE;
    if (m & S_IRGRP) p |= FIBF_GRP_READ;
    if (m & S_IWGRP) p |= FIBF_GRP_WRITE | FIBF_GRP_DELETE;
    if (m & S_IXGRP) p |= FIBF_GRP_EXECUTE;
    if (m & S_IROTH) p |= FIBF_OTR_READ;
    if (m & S_IWOTH) p |= FIBF_OTR_WRITE | FIBF_OTR_DELETE;
    if (m & S_IXOTH) p |= FIBF_OTR_EXECUTE;
    return p;
}

/* The name of a header as the listing takes it; extraction compares with it. */
static const char *la_name(struct archive_entry *ae)
{
    const char *u = archive_entry_pathname_utf8(ae);
    if (!u)
        u = archive_entry_pathname(ae);
    return u ? u : "";
}

static ua_etype la_type(struct archive_entry *ae)
{
    switch (archive_entry_filetype(ae)) {
    case AE_IFREG: return UA_ET_FILE;
    case AE_IFDIR: return UA_ET_DIR;
    case AE_IFLNK: return UA_ET_LINK;
    default: return UA_ET_OTHER;
    }
}

static int la_list_mode(const char *path, int stream, ua_list *out, char *err, int errlen)
{
    int unknown = 0, have_needs = 0, any_enc = 0;
    struct archive *a;
    struct archive_entry *ae;
    ua_needs nd;
    int r;

    /* 7z and ZIP: what the archive needs, from its index, before reading it.
     * An encrypted list cannot be read here at all. (A set's index is in its
     * last volume; sets go to 7-Zip first anyway.) */
    if (!stream && !out->volumes) {
        ua_format f;
        LONG ioerr;
        if (ua_sniff_file(path, &f, &ioerr) == 0 && (f == UA_FMT_7Z || f == UA_FMT_ZIP))
            have_needs = ua_needs_read(path, f, &nd);
        if (have_needs && nd.list_encrypted) {
            ua_needs_free(&nd);
            snprintf(err, errlen, "the entry list is encrypted, which this engine cannot read");
            return UA_LIST_UNSUPPORTED;
        }
    }
    a = la_open(path, out->volumes, stream, &unknown, err, errlen);
    if (!a) {
        if (have_needs)
            ua_needs_free(&nd);
        return unknown ? UA_LIST_UNKNOWN : UA_LIST_FAILED;
    }
    while ((r = archive_read_next_header(a, &ae)) == ARCHIVE_OK || r == ARCHIVE_WARN) {
        ua_entry *e = ua_list_add(out);
        const char *u = la_name(ae);
        if (!e)
            break;
        /* The program sets libarchive's "current charset" to UTF-8 (compat.c),
         * so names arrive unconverted. Bytes that are not valid UTF-8 come from
         * an archive without charset information: for ZIP that is CP437 by
         * convention, for anything else we only know it is 8-bit. */
        e->name_src = ua_strdup(u);
        if (ua_list_break(out)) {
            archive_read_free(a);
            if (have_needs)
                ua_needs_free(&nd);
            return UA_LIST_CANCELLED;
        }
        if (ua_utf8_valid(u))
            e->name_kind = UA_NAME_UTF8;
        else if (archive_format(a) == ARCHIVE_FORMAT_ZIP)
            e->name_kind = UA_NAME_CP437;
        else
            e->name_kind = UA_NAME_GUESSED;
        e->type = la_type(ae);
        e->size_valid = archive_entry_size_is_set(ae) != 0;
        e->size = e->size_valid ? (UQUAD)archive_entry_size(ae) : 0;
        e->encrypted = archive_entry_is_encrypted(ae);
        any_enc |= e->encrypted;
        if (archive_entry_perm_is_set(ae) && e->type == UA_ET_FILE) {
            e->prot = mode_to_prot(archive_entry_perm(ae));
            e->prot_valid = 1;
        }
        if (archive_entry_mtime_is_set(ae)) {
            /* libarchive gives UTC where the archive stores UTC. Where it
             * stores local time (RAR 1.5-4, CAB, a ZIP entry with only the DOS
             * field) it converts with mktime(), which on AROS ignores the
             * time zone, so the value is the stored local time as it is. */
            int af = archive_format(a), local =
                af == ARCHIVE_FORMAT_RAR || af == ARCHIVE_FORMAT_CAB ||
                (af == ARCHIVE_FORMAT_ZIP && have_needs && ua_needs_dos_time(&nd, u));
            ua_unix_to_datestamp(archive_entry_mtime(ae), !local, &e->date);
            e->date_valid = 1;
        }
        archive_read_data_skip(a);
    }
    if (r != ARCHIVE_EOF) {
        snprintf(err, errlen, "%s", archive_error_string(a) ? archive_error_string(a)
                                                            : "damaged archive");
        archive_read_free(a);
        if (have_needs)
            ua_needs_free(&nd);
        /* A partial listing is still useful to show, but the caller must know. */
        return out->n ? UA_LIST_PARTIAL : UA_LIST_FAILED;
    }
    archive_read_free(a);
    if (have_needs) {
        int unsup = nd.unsupported[0] != 0, enc = nd.data_encrypted;
        char what[80];
        snprintf(what, sizeof(what), "%s", nd.unsupported);
        ua_needs_free(&nd);
        nd.data_encrypted = enc;
        if (unsup)
            snprintf(nd.unsupported, sizeof(nd.unsupported), "%s", what);
    }
    /* Read, but not all of it can be extracted here: another engine may.
     * The list stays, for when none does. */
    if (have_needs && nd.unsupported[0]) {
        snprintf(err, errlen, "the archive uses %s, which this engine cannot decode",
                 nd.unsupported);
        return UA_LIST_UNSUPPORTED;
    }
    if (any_enc || (have_needs && nd.data_encrypted)) {
        snprintf(err, errlen, "the archive has encrypted entries, which this engine cannot decrypt");
        return UA_LIST_UNSUPPORTED;
    }
    return UA_LIST_OK;
}

static int la_extract_mode(const char *path, int stream, ua_list *l, ua_sink *s,
                           char *err, int errlen)
{
    struct archive *a = la_open(path, l->volumes, stream, NULL, err, errlen);
    struct archive_entry *ae;
    ULONG i = 0;
    int r = ARCHIVE_EOF, result = 0;

    if (!a)
        return -1;
    while (i < l->n && ((r = archive_read_next_header(a, &ae)) == ARCHIVE_OK || r == ARCHIVE_WARN)) {
        ua_entry *e = &l->v[i++];
        ua_wdecision d;
        if (!ua_entry_matches(e, la_name(ae), la_type(ae), archive_entry_size_is_set(ae) != 0,
                              (UQUAD)archive_entry_size(ae))) {
            snprintf(err, errlen, "%s (entry %lu is not \"%s\")", UA_CHANGED_MSG,
                     (unsigned long)i, e->name_src ? e->name_src : "");
            result = -1;
            break;
        }
        d = s->begin(s, e);
        const void *buf;
        size_t size;
        la_int64_t off;
        UQUAD at = 0;
        int rr, stop = 0;

        if (d == UA_W_CANCEL) {
            result = 1;
            break;
        }
        if (d == UA_W_SKIP) {
            archive_read_data_skip(a);
            continue;
        }
        while ((rr = archive_read_data_block(a, &buf, &size, &off)) == ARCHIVE_OK) {
            if ((UQUAD)off > at && s->seek(s, (UQUAD)off - at)) {
                stop = 1;
                break;
            }
            at = (UQUAD)off;
            if (size && s->data(s, buf, (ULONG)size)) {
                stop = 1;
                break;
            }
            at += size;
        }
        if (stop) {
            s->end(s, e, 0, "cancelled");
            result = 1;
            break;
        }
        if (rr != ARCHIVE_EOF) {
            s->end(s, e, 0, archive_error_string(a));
            if (rr == ARCHIVE_FATAL) {
                snprintf(err, errlen, "%s", archive_error_string(a));
                result = -1;
                break;
            }
            continue;
        }
        if (e->size > at && s->seek(s, e->size - at)) {   /* trailing sparse hole */
            s->end(s, e, 0, "cancelled");
            result = 1;
            break;
        }
        s->end(s, e, 1, NULL);
    }
    /* The walk must have met every listed entry, and the archive must end
     * where the listing said it did. */
    if (result == 0 && i < l->n) {
        if (r == ARCHIVE_EOF)
            snprintf(err, errlen, "%s (it ends after %lu of %lu entries)", UA_CHANGED_MSG,
                     (unsigned long)i, (unsigned long)l->n);
        else
            snprintf(err, errlen, "%s", archive_error_string(a) ? archive_error_string(a)
                                                                 : "damaged archive");
        result = -1;
    } else if (result == 0 && l->n && !l->partial &&
               (r = archive_read_next_header(a, &ae)) != ARCHIVE_EOF) {
        if (r == ARCHIVE_OK || r == ARCHIVE_WARN)
            snprintf(err, errlen, "%s (it has more entries than were listed)", UA_CHANGED_MSG);
        else
            snprintf(err, errlen, "%s", archive_error_string(a) ? archive_error_string(a)
                                                                 : "damaged archive");
        result = -1;
    }
    archive_read_free(a);
    return result;
}

static ULONG la_stream_caps(ua_format f)
{
    return f == UA_FMT_ZIP ? UA_CAP_LIST | UA_CAP_EXTRACT | UA_CAP_RECOVERY : 0;
}
static int la_list(const char *p, ua_list *o, char *e, int n) { return la_list_mode(p, 0, o, e, n); }
static int la_list_stream(const char *p, ua_list *o, char *e, int n) { return la_list_mode(p, 1, o, e, n); }
static int la_extract(const char *p, ua_list *l, ua_sink *s, char *e, int n)
{
    return la_extract_mode(p, 0, l, s, e, n);
}
static int la_extract_stream(const char *p, ua_list *l, ua_sink *s, char *e, int n)
{
    return la_extract_mode(p, 1, l, s, e, n);
}

const ua_engine ua_engine_libarchive = { "libarchive", la_caps, la_list, la_extract };
const ua_engine ua_engine_libarchive_stream = { "libarchive-stream", la_stream_caps,
                                                la_list_stream, la_extract_stream };
