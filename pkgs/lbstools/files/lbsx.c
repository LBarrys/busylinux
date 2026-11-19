/* lbsx - extract zip, 7z, rar and tar archives (extraction only)
 *
 * Usage:   lbsx [-v] [-C dir] [-p password] archive...
 *            -C dir       extract into dir (created if missing); default: current dir
 *            -v           print each file as it is extracted
 *            -p password  password for encrypted zips; without it lbsx asks on the
 *                         terminal (hidden) when one is needed. -p is visible to
 *                         other users in `ps`, so prefer the prompt.
 *
 * Formats: zip, 7z, rar (RAR4 and RAR5), tar - including compressed tars
 *          (.tar.gz .tgz .tar.bz2 .tar.xz .tar.zst .tar.lz4 ...).
 *          Detected from the contents, not the file name.
 * Safety:  entries can't escape the destination: "../" paths, absolute paths
 *          and paths through symlinks are refused. Existing files are replaced.
 *          Encrypted zips (ZipCrypto and AES) are supported; encrypted 7z and rar
 *          are refused, since libarchive can't decrypt them.
 *          A file whose data turns out truncated/corrupt is removed, not left half-written.
 *
 * Build:   cc -O2 -s -o lbsx lbsx.c -larchive
 *          (Alpine: apk add gcc musl-dev libarchive-dev; at runtime only libarchive)
 */
#include <errno.h>
#include <fcntl.h>
#include <langinfo.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <archive.h>
#include <archive_entry.h>

static int verbose;
static const char *password;            /* from -p */

/* ---- password prompt: on the terminal, typing hidden, up to 3 tries per archive */
static int tty = -1, tries;
static struct termios saved;
static enum { PW_OK, PW_NOTTY, PW_WRONG, PW_GAVEUP } pw_state;   /* why no password came */

static void restore_tty(int sig)        /* Ctrl-C at the prompt: don't leave echo off */
{
    tcsetattr(tty, TCSAFLUSH, &saved);
    if (write(tty, "\n", 1)) {}
    _exit(128 + sig);
}

static const char *ask(struct archive *a, void *data)
{
    static char buf[256];
    const char *path = data;
    struct termios t;
    struct sigaction sa = { .sa_handler = restore_tty }, oi, ot;
    ssize_t n = 0;
    (void)a;

    if (tries++ >= 3) { pw_state = PW_WRONG; return NULL; }
    if ((tty = open("/dev/tty", O_RDWR | O_CLOEXEC)) < 0) { pw_state = PW_NOTTY; return NULL; }
    tcgetattr(tty, &saved);
    t = saved;
    t.c_lflag &= ~ECHO;
    sigaction(SIGINT, &sa, &oi);
    sigaction(SIGTERM, &sa, &ot);
    dprintf(tty, tries > 1 ? "Wrong password. Password for %s: " : "Password for %s: ", path);
    tcsetattr(tty, TCSAFLUSH, &t);
    while (n < (ssize_t)sizeof buf - 1) {
        ssize_t k = read(tty, buf + n, 1);
        if (k <= 0 || buf[n] == '\n') break;
        n++;
    }
    buf[n] = 0;
    tcsetattr(tty, TCSAFLUSH, &saved);
    if (write(tty, "\n", 1)) {}
    sigaction(SIGINT, &oi, NULL);
    sigaction(SIGTERM, &ot, NULL);
    close(tty);
    if (!n) pw_state = PW_GAVEUP;       /* empty answer: give up */
    return n ? buf : NULL;
}

static void usage(int code)
{
    fputs("usage: lbsx [-v] [-C dir] [-p password] archive...\n"
          "  -C dir       extract into dir (created if missing)\n"
          "  -v           list files as they are extracted\n"
          "  -p password  password for encrypted zips (default: ask when needed)\n", code ? stderr : stdout);
    exit(code);
}

static int mkdir_p(char *p)
{
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, 0777) && errno != EEXIST) return -1;
            *s = '/';
        }
    return mkdir(p, 0777) && errno != EEXIST ? -1 : 0;
}

static int copy_data(struct archive *in, struct archive *out)
{
    const void *buf;
    size_t len;
    la_int64_t off;
    for (;;) {
        int r = archive_read_data_block(in, &buf, &len, &off);
        if (r == ARCHIVE_EOF) return ARCHIVE_OK;
        if (r < ARCHIVE_WARN) return r;
        if (archive_write_data_block(out, buf, len, off) < ARCHIVE_WARN) return ARCHIVE_FATAL;
    }
}

/* the clearest description of why reading failed */
static const char *why(struct archive *a, int at_start)
{
    const char *msg = archive_error_string(a);
    int e = archive_errno(a);
    if (msg && strstr(msg, "ncrypted")) return "encrypted 7z/rar archives are not supported (only zip)";
    if (at_start && e == EILSEQ) return "not a zip, 7z, rar or tar archive";   /* libarchive's "bad format" */
    if (at_start && e > 0 && e != EINVAL) return strerror(e);                 /* e.g. no such file */
    return msg ? msg : "unknown error";
}

/* Extract one archive into the current directory; returns the number of errors.
 * The archive is opened before we chdir, so relative archive paths still work. */
static int extract(const char *path, int destfd, int homefd)
{
    struct archive *in = archive_read_new(), *out = archive_write_disk_new();
    struct archive_entry *e;
    int errors = 0, entries = 0, r;

    tries = 0;                                      /* each archive gets its own 3 tries */
    pw_state = PW_OK;

    archive_read_support_format_zip(in);
    archive_read_support_format_7zip(in);
    archive_read_support_format_rar(in);
    archive_read_support_format_rar5(in);
    archive_read_support_format_tar(in);
    archive_read_support_filter_all(in);            /* .gz .bz2 .xz .zst ... around tar */
    if (password) archive_read_add_passphrase(in, password);    /* -p: use only that */
    else archive_read_set_passphrase_callback(in, (void *)path, ask);

    archive_write_disk_set_options(out,
        ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM |
        ARCHIVE_EXTRACT_SECURE_NODOTDOT | ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS |
        ARCHIVE_EXTRACT_SECURE_SYMLINKS);
    archive_write_disk_set_standard_lookup(out);

    if (archive_read_open_filename(in, path, 1 << 16) != ARCHIVE_OK) {
        fprintf(stderr, "lbsx: %s: %s\n", path, why(in, 1));
        errors = 1;
        goto done;
    }
    if (fchdir(destfd)) {
        fprintf(stderr, "lbsx: cannot enter destination: %s\n", strerror(errno));
        errors = 1;
        goto done;
    }
    while ((r = archive_read_next_header(in, &e)) != ARCHIVE_EOF) {
        if (r == ARCHIVE_FATAL) {                   /* broken or unsupported archive */
            fprintf(stderr, "lbsx: %s: %s\n", path, why(in, !entries));
            errors++;
            break;
        }
        const char *name = archive_entry_pathname(e);
        if (!name) name = archive_entry_pathname_utf8(e);
        if (!name) name = "?";
        int zip = (archive_format(in) & ARCHIVE_FORMAT_BASE_MASK) == ARCHIVE_FORMAT_ZIP;
        entries++;
        /* libarchive can't decrypt a symlink's target in an encrypted zip: AES
         * entries fail to read, ZipCrypto ones would come out as garbage links */
        if (r == ARCHIVE_FAILED || (zip && archive_entry_is_encrypted(e) &&
                                    archive_entry_filetype(e) == AE_IFLNK)) {
            const char *msg = archive_error_string(in);
            if (r != ARCHIVE_FAILED || (msg && strstr(msg, "link entry")))
                msg = "symlink in an encrypted zip can't be restored, skipped";
            fprintf(stderr, "lbsx: %s: %s: %s\n", path, name, msg ? msg : "unreadable entry, skipped");
            errors++;
            continue;                               /* the rest of the archive is fine */
        }
        if (archive_entry_is_encrypted(e) && !zip) {   /* stop before creating empty files */
            fprintf(stderr, "lbsx: %s: encrypted 7z/rar archives are not supported (only zip)\n", path);
            errors++;
            break;
        }
        if (verbose) puts(name);
        if (archive_write_header(out, e) < ARCHIVE_WARN) {   /* e.g. unsafe path: skip it */
            fprintf(stderr, "lbsx: %s: %s: %s\n", path, name, archive_error_string(out));
            errors++;
            continue;
        }
        if (archive_entry_size(e) > 0 && copy_data(in, out) < ARCHIVE_WARN) {
            const char *msg = archive_error_string(in);
            if (msg && (strstr(msg, "Incorrect passphrase") || strstr(msg, "Passphrase required")))
                msg = pw_state == PW_NOTTY  ? "password required: no terminal to ask on, use -p"
                    : pw_state == PW_GAVEUP ? "no password given"
                    : pw_state == PW_WRONG  ? "wrong password (3 tries)"
                    : "wrong password";
            if (!msg) msg = archive_error_string(out);
            fprintf(stderr, "lbsx: %s: %s: %s\n", path, name, msg ? msg : "read error");
            errors++;
            archive_write_finish_entry(out);
            if (archive_entry_filetype(e) == AE_IFREG) unlink(name);  /* no half-written files */
            break;                                  /* truncated/corrupt: nothing more to read */
        }
        if (archive_write_finish_entry(out) < ARCHIVE_WARN) {
            fprintf(stderr, "lbsx: %s: %s: %s\n", path, name, archive_error_string(out));
            errors++;
        }
    }
done:
    /* Freeing the writer applies its deferred fixes (directory times and
     * read-only modes); their paths are relative to the destination, so this
     * must happen before we leave it. */
    archive_write_free(out);
    archive_read_free(in);
    if (fchdir(homefd)) {}
    return errors;
}

int main(int argc, char **argv)
{
    const char *dest = ".";
    int i, errors = 0;

    /* libarchive converts entry names to the locale's charset; in the plain "C"
     * locale non-ASCII names (very common in zip/7z/rar) can't be converted. */
    setlocale(LC_CTYPE, "");
    if (strcmp(nl_langinfo(CODESET), "UTF-8") && !setlocale(LC_CTYPE, "C.UTF-8"))
        setlocale(LC_CTYPE, "en_US.UTF-8");

    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage(0);
        else if (!strcmp(a, "-v")) verbose = 1;
        else if (!strcmp(a, "-C") && i + 1 < argc) dest = argv[++i];
        else if (!strcmp(a, "-p") && i + 1 < argc) password = argv[++i];
        else usage(1);
    }
    if (i >= argc) usage(1);

    char d[4096];
    snprintf(d, sizeof d, "%s", dest);
    if (mkdir_p(d)) { fprintf(stderr, "lbsx: %s: %s\n", dest, strerror(errno)); return 1; }
    int destfd = open(dest, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    int homefd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (destfd < 0 || homefd < 0) { fprintf(stderr, "lbsx: %s: %s\n", dest, strerror(errno)); return 1; }

    for (; i < argc; i++) errors += extract(argv[i], destfd, homefd);
    return errors ? 1 : 0;
}
