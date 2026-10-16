# unarc-7z output protocol

`unarc-7z` writes only this protocol to stdout. It is meant to be read by a
program, not by a person. Records are lines of tab-separated fields ending in
`\n` (LF). The only exception is the raw data that follows a `D` record.

```
unarc-7z [-p <file>] list <archive>
unarc-7z [-p <file>] extract <archive> <index>[,<index>...]
unarc-7z [-p <file>] extract <archive> all
unarc-7z [-p <file>] listx <hex>
unarc-7z [-p <file>] extractx <hex> <indices or all>
```

`listx` and `extractx` take the archive path as hex of its bytes, so that a
caller never has to quote a file name. On AROS the path bytes are ISO-8859-1,
as AROS file names are; on a POSIX host they are UTF-8.

`extract` takes one or more indices (in any order, duplicates ignored) or
`all`, and decodes them in one pass: a solid block is decoded once.

`<archive>` is the first volume of a set (`name.part1.rar`, `name.part01.rar`,
`name.rar` with `name.r00`...). The other volumes are looked up in the same
directory, under the names the RAR handlers derive from the first one.

## Password

If the archive asks for a password, while opening (encrypted headers) or while
extracting (encrypted data), one line is read from the `-p` file, or from
stdin without `-p`. aros-unarc passes a `PIPE:` name with `-p`: AROS puts a
process's command line into its input stream, so stdin is not a safe place
for a password there. That happens at most
once per run, and only when a handler asks. A trailing CR/LF is removed. The
line is taken as UTF-8. An empty line or end of file means "no password". The
password is never printed or logged.

A caller that has no password should give an empty stdin (`<NIL:` on AROS,
`</dev/null` on a host). Otherwise the program waits for a line from the
console.

## Records

### A: archive (list only, after a successful open)

```
A <tab> format <tab> volumes <tab> flags
```

- `format`: the 7-Zip handler name: `7z`, `Rar` (RAR 1.5 to 4.x) or `Rar5`.
- `volumes`: the number of files opened, the first one included.
- `flags`: `-`, or a comma-separated list of:
  - `solid`: the archive is solid;
  - `headers-encrypted`: a password was needed to read the entry list;
  - `entries-encrypted`: at least one entry has encrypted data;
  - `multivolume`: more than one volume was opened, or the handler says the
    file is a volume.

### E: entry (list only)

```
E <tab> index <tab> isdir <tab> size <tab> packed <tab> mtime <tab> attrib <tab> encrypted <tab> name
```

- `index`: 0-based. This is the number `extract` takes.
- `isdir`: `1` for a directory, else `0`.
- `size`: unpacked size in bytes, or `-` if unknown.
- `packed`: packed size in bytes, or `-` if unknown. In a solid 7z block, the
  whole block is counted on its first entry and the rest show `-`.
- `mtime`: modification time in seconds since 1970-01-01 UTC, or `-`. RAR 1.5
  to 4.x stores local time, and 7-Zip converts it using the time zone of the
  process. The value is therefore only as correct as that time zone.
- `attrib`: the 7-Zip attribute word in lowercase hex, or `-`. The low 16 bits
  are Windows attributes (`10` = directory). If bit 15 (`8000`) is set, the
  high 16 bits are a Unix `st_mode`. Example: `81a48000` is a regular file with
  mode 0644.
- `encrypted`: `1` if the entry's data is encrypted.
- `name`: the path inside the archive with `/` separators, as hex of its UTF-8
  bytes in lowercase (`612f62` = `a/b`). It is empty for an entry without a
  name.

### D and F: data and result per entry (extract only)

```
D <tab> index <tab> size \n <size raw bytes>
```

`size` is the entry's unpacked size from the archive headers. Exactly `size`
bytes follow the LF. If the decoder produces fewer bytes, the rest is padded
with zero bytes. If it produces more, the excess is dropped. In both cases the
`R` line reports an error, so the stream stays readable. If the size is not
known in advance, the data is collected in memory and `size` is the real
length.

No `D` record is written if no data was produced for a non-empty entry, for
example when the password is missing or wrong. A directory gives `D <index> 0`.

Before each wanted entry's `D` and `F` records:

```
H <tab> index <tab> isdir <tab> size <tab> name
```

with the fields of the `E` record of `list`. A caller compares it with the
entry it listed, so that an archive changed between listing and extraction
is noticed before anything is written.

After each wanted entry, with or without a `D` record:

```
F <tab> index <tab> code <tab> message
```

`code` and `message` are those of the table below, for this entry alone.
Entries come in archive order, not in the order they were asked for.

### R: result (always the last line)

```
R <tab> code <tab> message
```

| code | meaning |
|---|---|
| 0 | ok |
| 1 | needs a password: one was asked for, and stdin gave none |
| 2 | wrong password (see below) |
| 3 | data error: CRC or data error, broken or truncated headers, unexpected end, file is not an archive of the detected type |
| 4 | missing volume: a volume that the set needs is not there |
| 5 | unsupported: format not in this build, unsupported method or feature |
| 8 | cancelled: Ctrl-C (AROS) while opening, listing or extracting |
| 9 | other: cannot open the file, bad arguments, out of memory, internal error |

`message` is short English text on one line, with no tabs. Programs should
use the code. With code 4, the message names the missing volume, for example
`Missing volume : gap.part3.rar`.

What "wrong password" is based on:

- Encrypted headers, open fails after a password was given: 7-Zip cannot tell
  a wrong password from damaged encrypted headers, and neither can this
  program. The message is `cannot open encrypted archive (wrong password?)`.
- RAR5 data: the archive stores a password check value, so the handler
  reports a wrong password directly (`wrong password`).
- 7z and RAR 1.5-4.x data: these formats have no password check. A data or
  CRC error in an encrypted entry is reported as code 2,
  `data error in encrypted entry (wrong password?)`. 7-Zip does the same.

For `extract`, `R` is 0 when every entry's `F` was 0, otherwise the code and
message of the first entry that failed (1 when a password was needed and none
was given; 8 when cancelled, after which no more `F` records come).

For `list`, a non-zero code after `E` records means the listing may be
incomplete. Example: code 4 when a later volume is missing. The entries found
up to that point are still printed.

The process exit status is 0 when the code is 0, 10 for any other code, and
20 for a usage error. Use the `R` line, not the exit status.

## Example

```
$ unarc-7z list tree.7z </dev/null
A	7z	1	solid
E	0	1	0	0	1792095884	41ed8010	0	416d696761
...
R	0	ok
$ printf 'secret\n' | unarc-7z extract hdr.7z 1 | od -c | head -2
0000000   D  \t   1  \t   2   2  \n   h   e   l   l   o ...
```
