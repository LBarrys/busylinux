/* file-manager.c - a tiny terminal file manager (started as a C port of fff-ash)
 * Plain POSIX C: termios + ANSI escapes, no curses, no dependencies.
 *
 * Build:      cc -O2 -s -o file-manager file-manager.c           (Alpine: apk add gcc musl-dev)
 * cd-on-exit: f() { file-manager "$@"; cd "$(cat "${XDG_CACHE_HOME:-$HOME/.cache}/file-manager/.last_dir")"; }
 * Env:        FILE_MANAGER_HIDDEN=0    start with hidden files hidden (default: shown)
 *             FILE_MANAGER_PREVIEW=0   start with the preview pane off
 *             FILE_MANAGER_OPENER=cmd  open files with cmd instead of xdg-open/$EDITOR
 */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#ifdef __linux__
#include <sys/inotify.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

enum { K_UP = 256, K_DOWN, K_SUP, K_SDOWN, K_LEFT, K_RIGHT, K_HOME, K_END, PGUP, PGDN, ESCKEY };  /* pseudo keys */

/* Colours: the terminal's own 16-colour palette, so file-manager follows your theme.
 * BASE resets to the terminal's default foreground and background. */
#define BG     "49"                 /* default background */
#define FG     "39"                 /* default foreground */
#define RED    "31"
#define GREEN  "32"
#define YELLOW "33"
#define BLUE   "34"
#define MAGENTA "35"
#define CYAN   "36"
#define BASE   "\033[0m"
#define DIM    "\033[0;2m"
enum { SP_TTY, SP_DETACH };             /* how spawn() wires the child */
enum { ED_COMPLETE = 1, ED_LIVE = 2 };  /* line editor flags */

/* type: d dir, L link->dir, l link, b broken link, x executable, f file.
 * Stat data is filled lazily (st=1) so big directories list fast. */
typedef struct {
    char *name;
    char type, st;
    off_t size;
    time_t mtime;
    mode_t mode;
} Ent;

typedef struct { char *dir, *name; } Mem;

static const char OP_COPY[] = "copy", OP_MOVE[] = "move", OP_DEL[] = "delete";
static const char *SORTS[] = { "name", "size", "time" };

static Ent *all, **view;                /* all entries / visible after filter */
static char *names;                     /* every name of the listing, back to back */
static size_t nlen, ncap;
static int nall, all_cap, total, view_cap;
static int pos, top, lines = 24, cols = 80, rows = 23, listw = 80, pv;
static int drawn_top = -1, drawn_pos = -1, full = 1, dirty[16], ndirty;
static int show_hidden, show_details = 1, show_preview, sort_mode, utf8;
static int editing, prompted;
static int pscroll, pdirty;             /* preview scroll offset (lines) */
static char pv_key[PATH_MAX];           /* entry the scroll offset belongs to */
static const unsigned char *ahead;      /* typed-ahead bytes for a prompt */
static int nahead;
static char cwd[PATH_MAX], prev_dir[PATH_MAX], msg[PATH_MAX + 64];
static char filter[256], reply[PATH_MAX];
static char **mtab;                     /* marks: hash set of full paths */
static size_t mcap, mused;
static int nmarks;
static const char *mark_op;
static Mem *mem;                        /* cursor memory per directory */
static int nmem, mem_cap;
static int tty = -1, ino = -1, wd = -1;
static struct timespec dir_mt;          /* mtime fallback when no inotify */
static long pending_at;                 /* debounced relist deadline (ms) */
static struct termios orig;
static volatile sig_atomic_t resized, conted, cancel;

static void quit(void);
static void refresh(void);

/* ---------------------------------------------------------------- output */

static char ob[16384];
static size_t ol;

static void flush(void)
{
    size_t o = 0;
    while (o < ol) {
        ssize_t w = write(tty, ob + o, ol - o);
        if (w < 0 && errno == EINTR) continue;
        if (w < 0) break;
        o += w;
    }
    ol = 0;
}

static void out(const char *s, size_t n)
{
    while (n) {
        size_t k = sizeof ob - ol;
        if (!k) { flush(); continue; }
        if (k > n) k = n;
        memcpy(ob + ol, s, k);
        ol += k; s += k; n -= k;
    }
}

static void outs(const char *s) { out(s, strlen(s)); }

static void outf(const char *f, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(b, sizeof b, f, ap);
    va_end(ap);
    if (n > 0) out(b, n < (int)sizeof b ? (size_t)n : sizeof b - 1);
}

static void pad(int n) { while (n-- > 0) out(" ", 1); }

/* display width of one character at s; -1 for control/invalid (shown as '?') */
static int charw(const char *s, int *len)
{
    wchar_t wc;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    size_t n = mbrtowc(&wc, s, MB_CUR_MAX, &st);
    if (n == 0 || n > (size_t)MB_CUR_MAX) { *len = 1; return -1; }
    *len = (int)n;
    int w = wcwidth(wc);
    return w < 0 ? -1 : w;
}

static int strw(const char *s, size_t n)
{
    int w = 0, len, c;
    for (const char *e = s + n; s < e && *s; s += len)
        w += (c = charw(s, &len)) < 0 ? 1 : c;
    return w;
}

/* print s in at most maxw columns, control chars as '?', '…' when cut */
static int put_w(const char *s, int maxw, int ell)
{
    int tot = 0, used = 0, len, w;
    if (maxw <= 0) return 0;
    for (const char *p = s; *p && tot <= maxw; p += len)
        tot += (w = charw(p, &len)) < 0 ? 1 : w;
    int cut = tot > maxw, lim = cut && ell ? maxw - 1 : maxw;
    for (const char *p = s; *p; p += len) {
        w = charw(p, &len);
        if (used + (w < 0 ? 1 : w) > lim) break;
        if (w < 0) out("?", 1); else out(p, len);
        used += w < 0 ? 1 : w;
    }
    if (cut && ell) { outs(utf8 ? "\xe2\x80\xa6" : "~"); used++; }
    return used;
}

/* ---------------------------------------------------------------- terminal */

static void tty_raw(int isig)
{
    struct termios t = orig;
    t.c_lflag &= ~(ECHO | ICANON);
    if (!isig) t.c_lflag &= ~ISIG;
    t.c_iflag &= ~ICRNL;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    tcsetattr(tty, TCSANOW, &t);
}

static void setup_terminal(void)
{
    tty_raw(1);
    /* alt screen, no line wrap (long names get clipped), hide cursor */
    outs("\033[?1049h\033[?7l\033[?25l" BASE "\033[2J");
    flush();
}

/* async-signal-safe: usable from signal handlers */
static void restore(void)
{
    static const char s[] = "\033[m\033[?7h\033[?25h\033[?1049l";
    if (write(tty, s, sizeof s - 1)) {}
    tcsetattr(tty, TCSANOW, &orig);
}

static void reset_terminal(void) { flush(); restore(); }

static void on_fatal(int sig) { restore(); _exit(128 + sig); }
static void on_flag(int sig) { if (sig == SIGWINCH) resized = 1; else conted = 1; }
static void on_cancel(int sig) { (void)sig; cancel = 1; }

/* Ctrl-Z: give the terminal back, stop, and set it up again on resume */
static void on_tstp(int sig)
{
    int e = errno;
    sigset_t s;
    struct sigaction dfl = { .sa_handler = SIG_DFL }, me = { .sa_handler = on_tstp };
    (void)sig;
    restore();
    sigaction(SIGTSTP, &dfl, NULL);
    sigemptyset(&s);
    sigaddset(&s, SIGTSTP);
    sigprocmask(SIG_UNBLOCK, &s, NULL);
    raise(SIGTSTP);                     /* stopped here until `fg` */
    sigaction(SIGTSTP, &me, NULL);
    conted = 1;
    errno = e;
}

static void layout(void)
{
    pv = show_preview && cols >= 40;
    listw = pv ? (cols - 1) / 2 : cols;
}

static void get_size(void)
{
    struct winsize w;
    int ok = ioctl(tty, TIOCGWINSZ, &w) == 0;
    lines = ok && w.ws_row ? w.ws_row : 24;
    cols = ok && w.ws_col ? w.ws_col : 80;
    if (lines < 4) lines = 4;
    rows = lines - 2;                   /* list, divider line, status line */
    layout();
}

static long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

/* ---------------------------------------------------------------- helpers */

static void *grow(void *p, int *cap, int n, size_t sz)
{
    if (n < *cap) return p;
    while (n >= *cap) *cap = *cap * 2 + 32;
    if (!(p = realloc(p, *cap * sz))) { restore(); fputs("file-manager: out of memory\n", stderr); exit(1); }
    return p;
}

/* non-empty environment variable or NULL, like ${VAR:-} */
static const char *env(const char *k)
{
    const char *v = getenv(k);
    return v && *v ? v : NULL;
}

/* "$cwd/name" without doubling the slash at / */
static void join(char *buf, const char *name)
{
    snprintf(buf, PATH_MAX, "%s/%s", strcmp(cwd, "/") ? cwd : "", name);
}

static char *pjoin(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char *p = malloc(n);
    if (!p) { restore(); exit(1); }
    snprintf(p, n, "%s/%s", a, b);
    return p;
}

/* resolve `in` against `base` lexically (like cd -L): handles . and .. */
static int normpath(char *res, const char *base, const char *in)
{
    char tmp[PATH_MAX * 2], *o = res;
    const char *p = tmp;
    if (snprintf(tmp, sizeof tmp, "%s/%s", *in == '/' ? "" : base, in) >= (int)sizeof tmp)
        return -1;
    *o = 0;
    while (*p) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t n = p - s;
        if (!n || (n == 1 && *s == '.')) continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            char *q = strrchr(res, '/');
            if (q) *(o = q) = 0;
            continue;
        }
        if ((size_t)(o - res) + n + 2 > PATH_MAX) return -1;
        *o++ = '/';
        memcpy(o, s, n);
        *(o += n) = 0;
    }
    if (!*res) strcpy(res, "/");
    return 0;
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

static int in_path(const char *cmd)
{
    const char *p = getenv("PATH");
    char b[PATH_MAX];
    while (p) {
        const char *e = strchr(p, ':');
        int n = e ? (int)(e - p) : (int)strlen(p);
        snprintf(b, sizeof b, "%.*s/%s", n ? n : 1, n ? p : ".", cmd);
        if (!access(b, X_OK)) return 1;
        p = e ? e + 1 : NULL;
    }
    return 0;
}

/* text = no NUL bytes and valid UTF-8 (a sequence cut at the end is fine) */
static int is_text(const char *b, ssize_t n)
{
    const unsigned char *s = (const unsigned char *)b;
    for (ssize_t i = 0; i < n;) {
        unsigned c = s[i];
        int k = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (!c || k < 0) return 0;
        if (i + k >= n) return 1;
        for (int j = 1; j <= k; j++)
            if ((s[i + j] & 0xC0) != 0x80) return 0;
        i += k + 1;
    }
    return 1;
}

static void human(off_t s, char *o)
{
    const char *u = "BKMGTPE";
    double v = (double)s;
    int i = 0;
    while (v >= 1024 && i < 6) { v /= 1024; i++; }
    if (!i) snprintf(o, 16, "%dB", (int)s);
    else snprintf(o, 16, v < 10 ? "%.1f%c" : "%.0f%c", v, u[i]);
}

/* run a command; returns its exit status (or -1) */
static int spawn(char *const av[], int how)
{
    struct sigaction ign = { .sa_handler = SIG_IGN }, dfl = { .sa_handler = SIG_DFL }, oi, oq, ot;
    int st = 0;
    pid_t p = fork();
    if (p < 0) return -1;
    if (!p) {
        if (how == SP_DETACH) {
            int fd = open("/dev/null", O_RDWR);
            setsid();
            if (fork()) _exit(0);
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
        } else {
            dup2(tty, 0); dup2(tty, 1); dup2(tty, 2);
        }
        execvp(av[0], av);
        _exit(127);
    }
    if (how == SP_TTY) {                /* the child owns the terminal now */
        sigaction(SIGINT, &ign, &oi);
        sigaction(SIGQUIT, &ign, &oq);
        sigaction(SIGTSTP, &dfl, &ot);
    }
    while (waitpid(p, &st, 0) < 0 && errno == EINTR) {}
    if (how == SP_TTY) {
        sigaction(SIGINT, &oi, NULL);
        sigaction(SIGQUIT, &oq, NULL);
        sigaction(SIGTSTP, &ot, NULL);
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

/* ---------------------------------------------------------------- listing */

static int is_dir(char t) { return t == 'd' || t == 'L'; }

static void ent_stat(Ent *e)
{
    struct stat st, t;
    if (e->st) return;
    e->st = 1;
    if (!e->type) e->type = 'f';
    if (lstat(e->name, &st)) return;
    if (S_ISLNK(st.st_mode)) {
        if (stat(e->name, &t)) e->type = 'b';
        else { e->type = S_ISDIR(t.st_mode) ? 'L' : 'l'; st = t; }
    } else {
        e->type = S_ISDIR(st.st_mode) ? 'd' : st.st_mode & 0111 ? 'x' : 'f';
    }
    e->size = st.st_size;
    e->mtime = st.st_mtime;
    e->mode = st.st_mode;
}

static inline int dig(char c) { return (unsigned)(c - '0') < 10; }
static inline int lc(unsigned char c) { return (unsigned)(c - 'A') < 26u ? c + 32 : c; }

/* natural, case-insensitive: file2 < file10 */
static int natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (dig(*a) && dig(*b)) {
            while (*a == '0') a++;
            while (*b == '0') b++;
            const char *x = a, *y = b;
            while (dig(*x)) x++;
            while (dig(*y)) y++;
            if (x - a != y - b) return (int)((x - a) - (y - b));
            int c = strncmp(a, b, x - a);
            if (c) return c;
            a = x; b = y;
        } else {
            int c = lc(*a) - lc(*b);
            if (c) return c;
            a++; b++;
        }
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int cmp(const void *pa, const void *pb)
{
    const Ent *x = pa, *y = pb;
    int dx = is_dir(x->type), dy = is_dir(y->type), c;
    if (dx != dy) return dy - dx;
    if (sort_mode == 1 && x->size != y->size) return x->size < y->size ? 1 : -1;
    if (sort_mode == 2 && x->mtime != y->mtime) return x->mtime < y->mtime ? 1 : -1;
    return (c = natcmp(x->name, y->name)) ? c : strcmp(x->name, y->name);
}

/* substring match, case-insensitive unless the filter has uppercase */
static int match(const char *s, const char *f)
{
    for (const char *p = f; *p; p++)
        if (isupper((unsigned char)*p)) return strstr(s, f) != NULL;
    for (; *s; s++) {
        size_t k = 0;
        while (f[k] && tolower((unsigned char)s[k]) == f[k]) k++;
        if (!f[k]) return 1;
    }
    return 0;
}

static void clamp(void)
{
    if (pos >= total) pos = total - 1;
    if (pos < 0) pos = 0;
}

static void build_view(void)
{
    view = grow(view, &view_cap, nall, sizeof *view);
    total = 0;
    for (int i = 0; i < nall; i++)
        if (!*filter || match(all[i].name, filter)) view[total++] = &all[i];
    clamp();
    full = 1;
}

static int skip_name(const char *n)
{
    return *n == '.' && (!show_hidden || !n[1] || (n[1] == '.' && !n[2]));
}

static void list_dir(void)
{
    DIR *d = opendir(".");
    struct dirent *de;
    struct stat st;

    nall = 0;
    nlen = 0;
    while (d && (de = readdir(d))) {    /* one buffer for all names: no malloc per file */
        if (skip_name(de->d_name)) continue;
        size_t l = strlen(de->d_name) + 1;
        if (nlen + l > ncap) {
            ncap = (ncap + l) * 2;
            if (!(names = realloc(names, ncap))) { restore(); fputs("file-manager: out of memory\n", stderr); exit(1); }
        }
        memcpy(names + nlen, de->d_name, l);
        all = grow(all, &all_cap, nall, sizeof *all);
        Ent *e = &all[nall++];
        memset(e, 0, sizeof *e);
        e->name = (char *)(uintptr_t)nlen;  /* offset until the buffer stops moving */
        nlen += l;
#ifdef DT_DIR
        if (de->d_type == DT_DIR) e->type = 'd';
        else if (de->d_type == DT_REG) e->type = 'f';
#endif
    }
    if (d) closedir(d);
    for (int i = 0; i < nall; i++) {
        all[i].name = names + (uintptr_t)all[i].name;
        if (!all[i].type) ent_stat(&all[i]);           /* links and unknowns need it now */
    }
    if (!stat(".", &st)) dir_mt = st.st_mtim;
    if (sort_mode) for (int i = 0; i < nall; i++) ent_stat(&all[i]);
    qsort(all, nall, sizeof *all, cmp);
    build_view();
}

static int find_name(const char *n);

/* new sort order: re-sort what's loaded instead of reading the directory again */
static void resort(void)
{
    char *c = total ? strdup(view[pos]->name) : NULL;
    if (sort_mode) for (int i = 0; i < nall; i++) ent_stat(&all[i]);
    qsort(all, nall, sizeof *all, cmp);
    build_view();
    if (c) { find_name(c); free(c); }
    clamp();
}

static int find_name(const char *n)
{
    for (int i = 0; i < total; i++)
        if (!strcmp(view[i]->name, n)) { pos = i; return 1; }
    return 0;
}

/* re-list the directory, keeping the cursor on the same entry if possible */
static void relist(void)
{
    char *c = total ? strdup(view[pos]->name) : NULL;
    list_dir();
    if (c) { find_name(c); free(c); }
    clamp();
}

static void set_filter(const char *f)
{
    char *c = total ? strdup(view[pos]->name) : NULL;
    snprintf(filter, sizeof filter, "%s", f);
    build_view();
    if (!(c && find_name(c))) pos = 0;
    free(c);
}

static void remember(void)
{
    if (!total) return;
    for (int i = 0; i < nmem; i++)
        if (!strcmp(mem[i].dir, cwd)) {
            free(mem[i].name);
            mem[i].name = strdup(view[pos]->name);
            return;
        }
    mem = grow(mem, &mem_cap, nmem, sizeof *mem);
    mem[nmem].dir = strdup(cwd);
    mem[nmem++].name = strdup(view[pos]->name);
}

static const char *recall(const char *dir)
{
    for (int i = 0; i < nmem; i++)
        if (!strcmp(mem[i].dir, dir)) return mem[i].name;
    return NULL;
}

static void watch(void)
{
#ifdef __linux__
    if (ino < 0) return;
    if (wd >= 0) inotify_rm_watch(ino, wd);
    wd = inotify_add_watch(ino, ".", IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO |
                           IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF);
#endif
}

static int change_dir(const char *target, const char *sel)
{
    char p[PATH_MAX];
    if (normpath(p, cwd, target) || chdir(p)) {
        snprintf(msg, sizeof msg, "error: cannot open '%s'", target);
        return -1;
    }
    remember();
    if (strcmp(cwd, p)) strcpy(prev_dir, cwd);
    strcpy(cwd, p);
    watch();
    pos = top = 0;
    *filter = 0;
    list_dir();
    if (!(sel && find_name(sel))) {
        const char *r = recall(cwd);
        if (r) find_name(r);
    }
    return 0;
}

/* ---------------------------------------------------------------- marks */

#define TOMB ((char *)1)                 /* deleted slot */

static size_t mslot(const char *p)      /* slot holding p, or the free slot for it */
{
    size_t h = 14695981039346656037u, i, tomb = (size_t)-1;
    for (const char *c = p; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211u;
    for (i = h & (mcap - 1);; i = (i + 1) & (mcap - 1)) {
        if (!mtab[i]) return tomb != (size_t)-1 ? tomb : i;
        if (mtab[i] == TOMB) { if (tomb == (size_t)-1) tomb = i; }
        else if (!strcmp(mtab[i], p)) return i;
    }
}

static int marked(const char *p)
{
    if (!nmarks) return 0;
    char *m = mtab[mslot(p)];
    return m && m != TOMB && !strcmp(m, p);
}

static void add_mark(const char *p)
{
    if ((mused + 1) * 10 >= mcap * 7) {             /* grow (also drops tombstones) */
        char **old = mtab;
        size_t ocap = mcap;
        mcap = mcap ? mcap * 2 : 256;
        if (!(mtab = calloc(mcap, sizeof *mtab))) { restore(); exit(1); }
        mused = 0;
        for (size_t k = 0; k < ocap; k++)
            if (old[k] && old[k] != TOMB) { mtab[mslot(old[k])] = old[k]; mused++; }
        free(old);
    }
    size_t i = mslot(p);
    if (mtab[i] && mtab[i] != TOMB) return;         /* already marked */
    if (!mtab[i]) mused++;
    mtab[i] = strdup(p);
    nmarks++;
}

static void toggle_mark(const char *p)
{
    size_t i;
    if (marked(p)) { i = mslot(p); free(mtab[i]); mtab[i] = TOMB; nmarks--; }
    else add_mark(p);
}

static void clear_marks(void)
{
    if (nmarks) full = 1;
    for (size_t k = 0; k < mcap; k++) {
        if (mtab[k] && mtab[k] != TOMB) free(mtab[k]);
        mtab[k] = NULL;
    }
    nmarks = 0;
    mused = 0;
    mark_op = NULL;
}

/* ---------------------------------------------------------------- drawing */

static void sep(void) { if (pv) outs(utf8 ? DIM "\xe2\x94\x82" BASE : DIM "|" BASE); }

/* right-hand columns for width w; returns their width (all ASCII) */
static int details(Ent *e, char *d, int w)
{
    char sz[16] = "-", tm[24] = "", pm[10];
    if (!show_details) return 0;
    if (!is_dir(e->type)) human(e->size, sz);
    if (w >= 16 + 33) {
        for (int i = 0; i < 9; i++) pm[i] = e->mode & (0400 >> i) ? "rwxrwxrwx"[i] : '-';
        pm[9] = 0;
        strftime(tm, sizeof tm, "%Y-%m-%d %H:%M", localtime(&e->mtime));
        return snprintf(d, 64, " %s %5s %s", pm, sz, tm);
    }
    return w >= 12 + 6 ? snprintf(d, 64, " %5s", sz) : 0;
}

static void draw_line(int i, int r)
{
    static const char types[] = "dLlbx", sufs[] = "//@@";
    static const char *colors[] = { "1;" BLUE, "1;" CYAN, CYAN, RED, GREEN, FG };
    char path[PATH_MAX], det[64];
    Ent *e = view[i];
    ent_stat(e);
    const char *q = strchr(types, e->type);
    int t = q && *q ? (int)(q - types) : 5, sel = i == pos;
    int w = listw - 1, dw = details(e, det, w), nw = w - dw, u;

    outf("\033[%d;1H", r);
    join(path, e->name);
    outs(marked(path) ? "\033[1;" YELLOW "m*" BASE : " ");
    outf("\033[0;" BG ";%s%sm", colors[t], sel ? ";7" : "");  /* 7 last: a leading 0 would cancel it */
    u = put_w(e->name, nw - (t < 4), 1);
    if (t < 4 && u < nw) { out(&sufs[t], 1); u++; }
    pad(nw - u);                        /* the cursor bar spans the whole list */
    if (dw) { if (!sel) outs(DIM); outs(det); }
    outs(BASE);
    if (pv) sep(); else outs("\033[K");
}

static void blank_row(int r, const char *text)
{
    int u = 0;
    outf("\033[%d;1H", r);
    if (text) { outs(DIM); u = put_w(text, listw, 1); outs(BASE); }
    if (pv) { pad(listw - u); sep(); } else outs("\033[K");
}

static void prow(int r, int x) { outf("\033[%d;%dH", r, x); }

static void pline(int r, int x, int w, const char *attr, const char *s)
{
    prow(r, x);
    outs(attr);
    put_w(s, w, 1);
    outs(BASE "\033[K");
}

static int preview_dir(const char *name, int x, int w, int r)
{
    DIR *d = opendir(name);
    struct dirent *de;
    Ent *v = NULL;
    int n = 0, cap = 0;

    if (!d) { pline(r, x, w, "\033[" RED "m", strerror(errno)); return r + 1; }
    while ((de = readdir(d)) && n < 2000) {
        if (skip_name(de->d_name)) continue;
        v = grow(v, &cap, n, sizeof *v);
        memset(&v[n], 0, sizeof *v);
        v[n].name = strdup(de->d_name);
        v[n].type = 'f';
#ifdef DT_DIR
        if (de->d_type == DT_DIR) v[n].type = 'd';
        else if (de->d_type == DT_LNK) v[n].type = 'l';
#endif
        n++;
    }
    closedir(d);
    qsort(v, n, sizeof *v, cmp);
    if (!n) pline(r++, x, w, DIM, "(empty)");
    int avail = rows - r + 1;
    if (pscroll > n - avail) pscroll = n - avail > 0 ? n - avail : 0;
    for (int i = 0; i < n; i++) {
        if (i >= pscroll && r <= rows) {
            int dir = v[i].type == 'd';
            prow(r++, x);
            outs(dir ? "\033[1;" BLUE "m" : v[i].type == 'l' ? "\033[" CYAN "m" : "");
            if (put_w(v[i].name, w - dir, 1) < w && dir) out("/", 1);
            outs(BASE "\033[K");
        }
        free(v[i].name);
    }
    free(v);
    return r;
}

/* next line of f into b (newline removed); the rest of an over-long line is skipped */
static int next_line(FILE *f, char *b, size_t n)
{
    int c;
    if (!fgets(b, (int)n, f)) return 0;
    size_t l = strlen(b);
    if (l && b[l - 1] == '\n') b[--l] = 0;
    else while ((c = getc(f)) != EOF && c != '\n') {}
    if (l && b[l - 1] == '\r') b[--l] = 0;
    return 1;
}

static void text_line(int r, int x, int w, const char *p)
{
    char line[1024];
    size_t k = 0;
    int c = 0;
    for (; *p && k < sizeof line - 8; p++) {
        if (*p == '\t') {
            do line[k++] = ' '; while (++c % 4 && k < sizeof line - 8);
        } else {
            line[k++] = *p;
            if ((*p & 0xC0) != 0x80) c++;
        }
    }
    line[k] = 0;
    prow(r, x);
    put_w(line, w, 0);
    outs("\033[K");
}

static int preview_file(Ent *e, int x, int w, int r)
{
    static char buf[4096];
    struct stat st;
    char s[32];
    ssize_t n = -2;
    int fd, er = 0;
    FILE *f;

    if (e->type == 'b') return r;
    if ((fd = open(e->name, O_RDONLY | O_NONBLOCK)) < 0) n = -1, er = errno;
    else if (!fstat(fd, &st) && S_ISREG(st.st_mode) && (n = read(fd, buf, sizeof buf)) < 0) er = errno;
    if (n == -1 || n == -2 || !n || !is_text(buf, n) || lseek(fd, 0, SEEK_SET) || !(f = fdopen(fd, "r"))) {
        if (fd >= 0) close(fd);
        if (n == -1) pline(r, x, w, "\033[" RED "m", strerror(er));
        else if (n == -2) pline(r, x, w, DIM, "(special file)");
        else if (!n) pline(r, x, w, DIM, "(empty)");
        else { human(e->size, s); prow(r, x); outf(DIM "binary file, %s" BASE "\033[K", s); }
        return r + 1;
    }
    int avail = rows - r + 1, total_l = 0;
    if (pscroll) {                      /* clamp: don't scroll past the last full page */
        while (total_l < pscroll + avail && next_line(f, buf, sizeof buf)) total_l++;
        if (total_l < pscroll + avail) pscroll = total_l > avail ? total_l - avail : 0;
        rewind(f);
        for (int i = 0; i < pscroll && next_line(f, buf, sizeof buf); i++) {}
    }
    while (r <= rows && next_line(f, buf, sizeof buf)) text_line(r++, x, w, buf);
    fclose(f);
    return r;
}

static void draw_preview(void)
{
    int x = listw + 2, w = cols - listw - 1, r = 1;
    if (!pv || w < 2) return;
    if (total) {
        Ent *e = view[pos];
        char key[PATH_MAX];
        join(key, e->name);
        if (strcmp(key, pv_key)) { pscroll = 0; strcpy(pv_key, key); }
        ent_stat(e);
        if (strchr("lLb", e->type)) {
            char t[PATH_MAX], a[PATH_MAX + 4];
            ssize_t n = readlink(e->name, t, sizeof t - 1);
            if (n >= 0) {
                t[n] = 0;
                snprintf(a, sizeof a, "-> %s", t);
                pline(r++, x, w, e->type == 'b' ? "\033[" RED "m" : DIM, a);
            }
        }
        r = is_dir(e->type) ? preview_dir(e->name, x, w, r) : preview_file(e, x, w, r);
    }
    for (; r <= rows; r++) { prow(r, x); outs("\033[K"); }
}

/* horizontal line between the panes and the status bar */
static void draw_divider(void)
{
    outf(DIM "\033[%d;1H", lines - 1);
    for (int c = 1; c <= cols; c++)
        outs(!utf8 ? (pv && c == listw + 1 ? "+" : "-") : pv && c == listw + 1 ? "\xe2\x94\xb4" : "\xe2\x94\x80");
    outs(BASE);
}

static void draw_status(void)
{
    char s[PATH_MAX + 512];
    int n = 0;
    outf(BASE "\033[%d;1H\033[K", lines);
    if (*msg) { put_w(msg, cols, 1); return; }
    n += snprintf(s + n, sizeof s - n, "[%d/%d] ", total ? pos + 1 : 0, total);
    if (nmarks) n += snprintf(s + n, sizeof s - n, "(%d marked: %s) ", nmarks, mark_op);
    if (sort_mode) n += snprintf(s + n, sizeof s - n, "sort:%s ", SORTS[sort_mode]);
    if (*filter) n += snprintf(s + n, sizeof s - n, "/%s ", filter);
    snprintf(s + n, sizeof s - n, "%s", cwd);
    put_w(s, cols, 1);
}

/* Redraw only what changed: a full frame when the list or viewport moved,
 * otherwise just the old/new cursor rows, the preview and the status line. */
static void refresh(void)
{
    if (resized) { resized = 0; get_size(); full = 1; }
    if (pos < top) top = pos;
    else if (pos >= top + rows) top = pos - rows + 1;
    if (top > 0 && top + rows > total) top = total - rows;
    if (top < 0) top = 0;

    if (full || top != drawn_top) {
        for (int r = 1; r <= rows; r++) {
            int i = top + r - 1;
            if (i < total) draw_line(i, r);
            else blank_row(r, !total && r == 1 ? (*filter ? "(no match)" : "(empty directory)") : NULL);
        }
        draw_preview();
        draw_divider();
    } else {
        dirty[ndirty++] = drawn_pos;
        dirty[ndirty++] = pos;
        for (int j = 0; j < ndirty; j++) {
            int i = dirty[j];
            if (i >= top && i < total && i < top + rows) draw_line(i, i - top + 1);
        }
        if (pos != drawn_pos || pdirty) draw_preview();
    }
    if (!editing) draw_status();
    flush();
    full = ndirty = pdirty = 0;
    drawn_top = top;
    drawn_pos = pos;
}

/* ---------------------------------------------------------------- input */

static int dir_changed(void)
{
    struct stat st;
    return ino < 0 && !stat(".", &st) &&
           (st.st_mtim.tv_sec != dir_mt.tv_sec || st.st_mtim.tv_nsec != dir_mt.tv_nsec);
}

/* Wait for keys and directory changes at once. Signals interrupt poll()
 * (instant redraw on resize); one read grabs a whole escape sequence. */
/* does the buffer end inside an escape sequence (ESC, ESC [ 1 ; ...)? */
static int partial_esc(const unsigned char *b, int n)
{
    for (int i = n - 1; i >= 0 && i >= n - 8; i--)
        if (b[i] == 27) {
            if (i == n - 1) return 1;
            if (b[i + 1] != '[' && b[i + 1] != 'O') return 0;
            for (int j = i + 2; j < n; j++)
                if (b[j] >= 0x40 && b[j] <= 0x7e) return 0;    /* has its final byte */
            return 1;
        }
    return 0;
}

static int read_input(unsigned char *b, int cap)
{
    for (;;) {
        struct pollfd pf[2] = { { tty, POLLIN, 0 }, { ino, POLLIN, 0 } };
        long now = now_ms();
        int to = pending_at ? (pending_at > now ? (int)(pending_at - now) : 0) : ino >= 0 ? -1 : 1000;
        int r = poll(pf, ino >= 0 ? 2 : 1, to);
        if (r < 0) {
            if (errno != EINTR) quit();
            if (conted) { conted = 0; setup_terminal(); resized = 1; }
            if (resized) refresh();
            continue;
        }
        if (!r) {
            if (pending_at || dir_changed()) { pending_at = 0; relist(); refresh(); }
            continue;
        }
        if (ino >= 0 && pf[1].revents) {   /* debounce bursts of fs events */
            char ev[4096];
            while (read(ino, ev, sizeof ev) > 0) {}
            if (!pending_at) pending_at = now_ms() + 150;
        }
        if (pf[0].revents) {
            ssize_t n = read(tty, b, cap - 16), m;
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) quit();            /* EOF: terminal went away */
            /* ends in ESC or half a sequence: the Escape key, or a sequence split
             * across reads (key repeat, paste). Give the rest 50 ms to arrive. */
            while (partial_esc(b, (int)n) && n < cap) {
                struct pollfd p1 = { tty, POLLIN, 0 };
                if (poll(&p1, 1, 50) <= 0 || (m = read(tty, b + n, cap - n)) <= 0) break;
                n += m;
            }
            return (int)n;
        }
    }
}

static int getkey(void)
{
    unsigned char b[64];
    ssize_t n;
    while ((n = read(tty, b, sizeof b)) < 0 && errno == EINTR) {}
    return n > 0 ? *b : -1;
}

/* Tab completion of the last path component in reply */
static void complete(void)
{
    char dir[PATH_MAX], path[PATH_MAX * 2 + 2], best[256];
    const char *h = env("HOME"), *sl = strrchr(reply, '/'), *pre = sl ? sl + 1 : reply;
    size_t pl = strlen(pre), len = strlen(reply);
    struct dirent *de;
    struct stat st;
    int nm = 0;

    if (!sl) strcpy(dir, ".");
    else if (sl == reply) strcpy(dir, "/");
    else snprintf(dir, sizeof dir, "%.*s", (int)(sl - reply), reply);
    if (h && dir[0] == '~' && (!dir[1] || dir[1] == '/')) {
        snprintf(path, sizeof path, "%s%s", h, dir + 1);
        snprintf(dir, sizeof dir, "%s", path);
    }
    DIR *d = opendir(dir);
    if (!d) return;
    while ((de = readdir(d))) {
        const char *n = de->d_name;
        if (strncmp(n, pre, pl) || !strcmp(n, ".") || !strcmp(n, "..") || (*n == '.' && *pre != '.'))
            continue;
        if (!nm++) snprintf(best, sizeof best, "%s", n);
        else { size_t k = 0; while (best[k] && best[k] == n[k]) k++; best[k] = 0; }
    }
    closedir(d);
    if (!nm) return;
    snprintf(reply + len, sizeof reply - len, "%s", best + pl);
    snprintf(path, sizeof path, "%s/%s", dir, best);
    len = strlen(reply);
    if (nm == 1 && !stat(path, &st) && S_ISDIR(st.st_mode) && len + 1 < sizeof reply)
        strcpy(reply + len, "/");
}

/* One-line editor on the status line. Returns 1 on Enter, 0 on Esc/Ctrl-C. */
static int edit_line(const char *p, const char *init, int flags)
{
    size_t len, cur;
    int ok = -1;

    snprintf(reply, sizeof reply, "%s", init ? init : "");
    len = cur = strlen(reply);
    prompted = editing = 1;
    tty_raw(0);                         /* Ctrl-C arrives as a byte: cancel */
    outs("\033[?25h");
    while (ok < 0) {
        unsigned char in[64];
        size_t st = 0;
        int pw, avail, cw;
        char c0;

        outf(BASE "\033[%d;1H\033[K", lines);
        pw = put_w(p, cols - 1, 1);
        avail = cols - pw - 1;
        while (st < cur && strw(reply + st, cur - st) > avail - 1)
            do st++; while (st < cur && (reply[st] & 0xC0) == 0x80);
        c0 = reply[cur]; reply[cur] = 0;
        cw = put_w(reply + st, avail, 0);
        reply[cur] = c0;
        put_w(reply + cur, avail - cw, 0);
        outf("\033[%d;%dH", lines, pw + cw + 1);
        flush();

        ssize_t n, m;
        if (nahead) {                   /* pasted after the key that opened us */
            n = nahead < (int)sizeof in - 8 ? nahead : (int)sizeof in - 8;
            memcpy(in, ahead, n);
            nahead = 0;
        } else {
            n = read(tty, in, sizeof in - 8);
        }
        if (n < 0 && errno == EINTR) {
            if (conted) { conted = 0; setup_terminal(); tty_raw(0); outs("\033[?25h"); resized = 1; }
            if (resized) refresh();
            continue;
        }
        if (n <= 0) { ok = 0; break; }
        if (in[n - 1] == 27 && n < (ssize_t)sizeof in - 1) {
            struct pollfd p1 = { tty, POLLIN, 0 };
            if (poll(&p1, 1, 50) > 0 && (m = read(tty, in + n, (size_t)(sizeof in - 1 - (size_t)n))) > 0) n += m;
        }
        for (ssize_t i = 0; i < n && ok < 0; i++) {
            unsigned char c = in[i];
            size_t s = cur;
            if (c == 27) {
                if (i + 1 >= n) { ok = 0; break; }       /* lone Esc */
                if (in[i + 1] == '[' || in[i + 1] == 'O') {
                    int pn = 0;
                    ssize_t j = i + 2;
                    while (j < n && isdigit(in[j])) pn = pn * 10 + in[j++] - '0';
                    while (j < n && (in[j] < 0x40 || in[j] > 0x7e)) j++;
                    if (j >= n) break;
                    switch (in[j]) {
                    case 'C': if (cur < len) do cur++; while (cur < len && (reply[cur] & 0xC0) == 0x80); break;
                    case 'D': while (cur && (reply[--cur] & 0xC0) == 0x80) {} break;
                    case 'H': cur = 0; break;
                    case 'F': cur = len; break;
                    case '~':
                        if (pn == 1 || pn == 7) cur = 0;
                        else if (pn == 4 || pn == 8) cur = len;
                        else if (pn == 3 && cur < len) {
                            size_t e = cur + 1;
                            while (e < len && (reply[e] & 0xC0) == 0x80) e++;
                            memmove(reply + cur, reply + e, len - e + 1);
                            len -= e - cur;
                        }
                        break;
                    }
                    i = j;
                } else {
                    i++;                                 /* Alt+key: ignore */
                }
                continue;
            }
            switch (c) {
            case '\r': case '\n': ok = 1; break;
            case 3: ok = 0; break;                       /* Ctrl-C */
            case 127: case 8:
                while (cur && (reply[--cur] & 0xC0) == 0x80) {}
                memmove(reply + cur, reply + s, len - s + 1);
                len -= s - cur;
                break;
            case 1: cur = 0; break;                      /* Ctrl-A */
            case 5: cur = len; break;                    /* Ctrl-E */
            case 11: reply[len = cur] = 0; break;        /* Ctrl-K */
            case 21:                                     /* Ctrl-U */
                memmove(reply, reply + cur, len - cur + 1);
                len -= cur; cur = 0;
                break;
            case 23:                                     /* Ctrl-W */
                while (cur && (reply[cur - 1] == ' ' || reply[cur - 1] == '/')) cur--;
                while (cur && reply[cur - 1] != ' ' && reply[cur - 1] != '/') cur--;
                memmove(reply + cur, reply + s, len - s + 1);
                len -= s - cur;
                break;
            case '\t':
                if ((flags & ED_COMPLETE) && cur == len) { complete(); len = cur = strlen(reply); }
                break;
            default:
                if (c >= 32 && len + 1 < sizeof reply) {
                    memmove(reply + cur + 1, reply + cur, len - cur + 1);
                    reply[cur++] = c;
                    len++;
                }
            }
        }
        if ((flags & ED_LIVE) && ok < 0) { set_filter(reply); refresh(); }
    }
    editing = 0;
    tty_raw(1);
    outs("\033[?25l");
    return ok == 1;
}

static int confirm(const char *p)
{
    prompted = 1;
    nahead = 0;                         /* never let typed-ahead keys answer */
    outf(BASE "\033[%d;1H\033[K", lines);
    outs(p);
    flush();
    int k = getkey();
    return k == 'y' || k == 'Y';
}

/* ---------------------------------------------------------------- file ops */

static char op_err[PATH_MAX + 64];
static int op_errs, op_i, op_n;
static long op_last;

static int fail_at(const char *p)
{
    const char *e = strerror(errno);
    if (!op_errs++) snprintf(op_err, sizeof op_err, "%s: %s", p, e);
    return -1;
}

/* progress on the status line, at most every 100ms */
static void tick(const char *p)
{
    char s[PATH_MAX + 64];
    long t = now_ms();
    const char *b = strrchr(p, '/');
    if (t - op_last < 100) return;
    op_last = t;
    snprintf(s, sizeof s, "%s %d/%d: %s  (Ctrl-C cancels)", mark_op, op_i, op_n, b ? b + 1 : p);
    outf(BASE "\033[%d;1H\033[K", lines);
    put_w(s, cols, 1);
    flush();
}

static int copy_file(const char *src, const char *dst)
{
    static char buf[65536];
    int in = open(src, O_RDONLY), o, rc = 0;
    if (in < 0) return fail_at(src);
    if ((o = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0) { close(in); return fail_at(dst); }
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf), w = 0;
        if (cancel) { rc = -1; break; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { rc = fail_at(src); break; }
        if (!n) break;
        while (w < n) {
            ssize_t k = write(o, buf + w, n - w);
            if (k < 0 && errno == EINTR && !cancel) continue;
            if (k < 0) break;
            w += k;
        }
        if (w < n) { rc = cancel ? -1 : fail_at(dst); break; }
    }
    close(in);
    if (close(o) && !rc) rc = fail_at(dst);
    if (rc) unlink(dst);
    return rc;
}

/* cp -Rp: files, dirs, symlinks, fifos; keeps mode, times and (as root) owner */
static int copy_tree(const char *src, const char *dst)
{
    struct stat st;
    int rc = 0;
    if (cancel) return -1;
    if (lstat(src, &st)) return fail_at(src);
    tick(src);
    if (S_ISLNK(st.st_mode)) {
        char t[PATH_MAX];
        ssize_t n = readlink(src, t, sizeof t - 1);
        if (n < 0) return fail_at(src);
        t[n] = 0;
        if (symlink(t, dst)) return fail_at(dst);
        if (!geteuid() && lchown(dst, st.st_uid, st.st_gid)) {}
        return 0;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR *d;
        struct dirent *de;
        if (mkdir(dst, 0700)) return fail_at(dst);
        if (!(d = opendir(src))) return fail_at(src);
        while ((de = readdir(d)) && !cancel) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            char *s = pjoin(src, de->d_name), *t = pjoin(dst, de->d_name);
            rc |= copy_tree(s, t);
            free(s);
            free(t);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        if ((rc = copy_file(src, dst))) return rc;
    } else if (S_ISFIFO(st.st_mode)) {
        if (mkfifo(dst, 0600)) return fail_at(dst);
    } else {
        errno = ENOTSUP;
        return fail_at(src);
    }
    struct timespec ts[2] = { st.st_atim, st.st_mtim };
    if (!geteuid() && chown(dst, st.st_uid, st.st_gid)) {}
    chmod(dst, st.st_mode & 07777);
    utimensat(AT_FDCWD, dst, ts, 0);
    return rc | (cancel ? -1 : 0);
}

static int rm_cb(const char *p, const struct stat *sb, int fl, struct FTW *f)
{
    (void)sb; (void)fl; (void)f;
    if (cancel) return 1;
    tick(p);
    if (remove(p)) fail_at(p);
    return 0;
}

static int rm_tree(const char *p)
{
    int e = op_errs;
    if (nftw(p, rm_cb, 16, FTW_DEPTH | FTW_PHYS) < 0) return fail_at(p);
    return op_errs > e || cancel ? -1 : 0;
}

static int move_tree(const char *src, const char *dst)
{
    if (!rename(src, dst)) return 0;
    if (errno != EXDEV) return fail_at(src);
    return copy_tree(src, dst) ? -1 : rm_tree(src);   /* across filesystems */
}

/* ---------------------------------------------------------------- actions */

static void move(int d)
{
    if (!total) return;
    pos += d;
    clamp();
}

static void run_fg(char *const av[])
{
    reset_terminal();
    spawn(av, SP_TTY);
    setup_terminal();
    get_size();
    full = 1;
}

static int file_is_text(const char *p)
{
    char b[4096];
    int fd = open(p, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 1;
    ssize_t n = read(fd, b, sizeof b);
    close(fd);
    return n <= 0 || is_text(b, n);
}

static void open_file(const char *path)
{
    const char *op = env("FILE_MANAGER_OPENER");
    char cmd[1024];

    /* on a desktop, binaries go to xdg-open; text always goes to $EDITOR */
    if (!op && (env("DISPLAY") || env("WAYLAND_DISPLAY")) && in_path("xdg-open") &&
        !file_is_text(path)) {
        char *av[] = { "xdg-open", (char *)path, NULL };
        spawn(av, SP_DETACH);
        return;
    }
    if (!op) op = env("VISUAL") ? env("VISUAL") : env("EDITOR") ? env("EDITOR") : "vi";
    snprintf(cmd, sizeof cmd, "%s \"$1\"", op);
    char *av[] = { "sh", "-c", cmd, "file-manager", (char *)path, NULL };
    run_fg(av);
    relist();
}

static void open_entry(void)
{
    char p[PATH_MAX];
    if (!total) return;
    join(p, view[pos]->name);
    if (is_dir(view[pos]->type)) change_dir(p, NULL);
    else open_file(p);
}

static void parent_dir(void)
{
    char base[PATH_MAX];
    if (!strcmp(cwd, "/")) return;
    strcpy(base, strrchr(cwd, '/') + 1);
    change_dir("..", base);
}

static void do_mark(const char *op, int all_)
{
    char p[PATH_MAX];
    if (!total) return;
    if (mark_op && mark_op != op) clear_marks();
    mark_op = op;
    if (all_) {
        for (int i = 0; i < total; i++) { join(p, view[i]->name); add_mark(p); }
        full = 1;
    } else {
        join(p, view[pos]->name);
        toggle_mark(p);
        if (ndirty < 14) dirty[ndirty++] = pos; else full = 1;
        move(1);
    }
    if (!nmarks) mark_op = NULL;
}

static void do_paste(void)
{
    char dest[PATH_MAX], here[PATH_MAX];
    struct sigaction sa = { .sa_handler = on_cancel }, old;
    struct stat st;
    int fail = 0, skip = 0, done = 0;

    if (!nmarks) return;
    if (mark_op == OP_DEL) {
        snprintf(dest, sizeof dest, "delete %d item(s)? [y/N] ", nmarks);
        if (!confirm(dest)) return;
    }
    cancel = op_errs = 0;
    op_n = nmarks;
    op_last = 0;
    sigaction(SIGINT, &sa, &old);       /* Ctrl-C cancels instead of quitting */
    join(here, "");                     /* "$cwd/" */
    op_i = 0;
    for (size_t k = 0; k < mcap && !cancel; k++) {
        if (!mtab[k] || mtab[k] == TOMB) continue;
        char *p = mtab[k];
        op_i++;                                     /* 1-based, for the progress line */
        size_t n = strlen(p);
        int e = op_errs, rc;
        if (mark_op == OP_DEL) {
            fail += rm_tree(p) != 0 && !cancel;
            continue;
        }
        join(dest, strrchr(p, '/') + 1);
        if (!lstat(dest, &st) || (!strncmp(here, p, n) && here[n] == '/')) { skip++; continue; }
        rc = mark_op == OP_COPY ? copy_tree(p, dest) : move_tree(p, dest);
        fail += (rc || op_errs > e) && !cancel;
    }
    sigaction(SIGINT, &old, NULL);
    done = cancel ? op_i - 1 : op_i;                /* the item in progress didn't finish */

    if (cancel)
        snprintf(msg, sizeof msg, "cancelled: %d of %d item(s) done", done, op_n);
    else if (fail)
        snprintf(msg, sizeof msg, "%d failed (%s)%s", fail, op_err, skip ? ", some skipped" : "");
    else if (skip)
        snprintf(msg, sizeof msg, "%d skipped (exists or into itself)", skip);
    clear_marks();
    relist();
}

static void do_rename(void)
{
    char pr[PATH_MAX + 16], old[PATH_MAX];
    struct stat st;
    if (!total) return;
    snprintf(old, sizeof old, "%s", view[pos]->name);
    snprintf(pr, sizeof pr, "rename: ");
    if (!edit_line(pr, old, 0) || !*reply || !strcmp(reply, old)) return;
    if (strchr(reply, '/'))       snprintf(msg, sizeof msg, "error: name cannot contain '/'");
    else if (!lstat(reply, &st))  snprintf(msg, sizeof msg, "warn: '%s' already exists", reply);
    else if (rename(old, reply))  snprintf(msg, sizeof msg, "error: rename failed: %s", strerror(errno));
    else { list_dir(); find_name(reply); }
}

static void do_mkdir(void)
{
    char first[PATH_MAX];
    struct stat st;
    if (!edit_line("new directory: ", NULL, ED_COMPLETE) || !*reply) return;
    snprintf(first, sizeof first, "%.*s", (int)strcspn(reply, "/"), reply);
    if (!stat(reply, &st))   snprintf(msg, sizeof msg, "warn: '%s' already exists", reply);
    else if (mkdir_p(reply)) snprintf(msg, sizeof msg, "error: mkdir failed: %s", strerror(errno));
    else { list_dir(); find_name(first); }
}

static void do_mkfile(void)
{
    struct stat st;
    int fd;
    if (!edit_line("new file: ", NULL, 0) || !*reply) return;
    if (!lstat(reply, &st))
        snprintf(msg, sizeof msg, "warn: '%s' already exists", reply);
    else if ((fd = open(reply, O_WRONLY | O_CREAT | O_EXCL, 0666)) < 0)
        snprintf(msg, sizeof msg, "error: cannot create '%s': %s", reply, strerror(errno));
    else { close(fd); list_dir(); find_name(reply); }
}

static void do_goto(void)
{
    char p[PATH_MAX];
    const char *h = env("HOME");
    if (!edit_line("go to: ", NULL, ED_COMPLETE) || !*reply) return;
    if (h && reply[0] == '~' && (!reply[1] || reply[1] == '/')) {
        snprintf(p, sizeof p, "%s%s", h, reply + 1);
        change_dir(p, NULL);
    } else {
        change_dir(reply, NULL);
    }
}

static void do_filter(void)
{
    char old[sizeof filter];
    snprintf(old, sizeof old, "%s", filter);
    set_filter(edit_line("/", filter, ED_LIVE) ? reply : old);
    full = 1;
}

static void pager_begin(void) { outs(BASE "\033[H\033[J\033[?7h"); }

static void pager_end(void)
{
    outs("\n" DIM "-- press any key --" BASE "\033[?7l");
    flush();
    getkey();
    full = 1;
}

static void do_attributes(void)
{
    if (!total) return;
    char *av[] = { "sh", "-c", "stat -- \"$1\" 2>/dev/null || ls -ld -- \"$1\"",
                   "file-manager", view[pos]->name, NULL };
    pager_begin();
    flush();
    spawn(av, SP_TTY);
    pager_end();
}

static void show_help(void)
{
    pager_begin();
    outs("file-manager keybindings\n\n"
         " Down / Up           move               c / C   mark (all) for copy\n"
         " Right / Enter       open / enter dir   m / M   mark (all) for move\n"
         " Left / Backspace    parent directory   t / T   mark (all) for delete\n"
         " PgUp / PgDn         page up / down     p       run marked operation\n"
         " Home / End          first / last       Esc     clear marks (then filter)\n"
         " /                   filter             r       rename\n"
         " :                   go to path         d       new directory\n"
         " ~                   home directory     f       new file\n"
         " -                   previous directory x       show attributes\n"
         " s                   cycle sort order   e       refresh\n"
         " i                   toggle details     .       toggle hidden\n"
         " v                   toggle preview     o       shell here\n"
         " Shift+Up / Down     scroll preview\n"
         " ?                   this help          q       quit\n\n"
         " prompts: Left/Right Home/End Ctrl-A/E/K/U/W, Tab completes paths, Esc cancels");
    pager_end();
}

static void quit(void)
{
    char dir[PATH_MAX], file[PATH_MAX + 16];
    const char *x = env("XDG_CACHE_HOME"), *h = env("HOME");
    FILE *f;
    if (x) snprintf(dir, sizeof dir, "%s/file-manager", x);
    else   snprintf(dir, sizeof dir, "%s/.cache/file-manager", h ? h : "");
    snprintf(file, sizeof file, "%s/.last_dir", dir);
    mkdir_p(dir);
    if ((f = fopen(file, "w"))) { fprintf(f, "%s\n", cwd); fclose(f); }
    reset_terminal();
    exit(0);
}

static void key(int k)
{
    switch (k) {
    case K_DOWN: move(1); break;
    case K_UP: move(-1); break;
    case K_RIGHT: case '\r': case '\n': open_entry(); break;
    case K_LEFT: case '\b': case 127: parent_dir(); break;
    case PGUP: move(-rows); break;
    case PGDN: move(rows); break;
    case K_HOME: pos = 0; break;
    case K_END: pos = total ? total - 1 : 0; break;
    case K_SUP: if (pv && pscroll) { pscroll--; pdirty = 1; } break;
    case K_SDOWN: if (pv) { pscroll++; pdirty = 1; } break;   /* clamped when drawn */
    case '/': do_filter(); break;
    case ESCKEY: if (nmarks) clear_marks(); else if (*filter) set_filter(""); break;
    case ':': do_goto(); break;
    case '~': if (env("HOME")) change_dir(env("HOME"), NULL); break;
    case '-': if (*prev_dir) change_dir(prev_dir, NULL); break;
    case '.': show_hidden = !show_hidden; relist(); break;
    case 'e': relist(); break;
    case 's': sort_mode = (sort_mode + 1) % 3; resort(); break;
    case 'i': show_details = !show_details; full = 1; break;
    case 'v': show_preview = !show_preview; layout(); full = 1; break;
    case 'c': do_mark(OP_COPY, 0); break;
    case 'm': do_mark(OP_MOVE, 0); break;
    case 't': do_mark(OP_DEL, 0); break;
    case 'C': do_mark(OP_COPY, 1); break;
    case 'M': do_mark(OP_MOVE, 1); break;
    case 'T': do_mark(OP_DEL, 1); break;
    case 'p': do_paste(); break;
    case 'r': do_rename(); break;
    case 'd': do_mkdir(); break;
    case 'f': do_mkfile(); break;
    case 'x': do_attributes(); break;
    case 'o': {
        char *av[] = { (char *)(env("SHELL") ? env("SHELL") : "/bin/sh"), NULL };
        run_fg(av);
        relist();
        break;
    }
    case '?': show_help(); break;
    case 'q': quit();
    }
}

static void handle_input(const unsigned char *b, int n)
{
    static const struct { const char *s; int k; } seqs[] = {
        { "\033[A", K_UP }, { "\033[B", K_DOWN }, { "\033[C", K_RIGHT }, { "\033[D", K_LEFT },
        { "\033OA", K_UP }, { "\033OB", K_DOWN }, { "\033OC", K_RIGHT }, { "\033OD", K_LEFT },
        { "\033[H", K_HOME }, { "\033[F", K_END }, { "\033OH", K_HOME }, { "\033OF", K_END },
        { "\033[1~", K_HOME }, { "\033[4~", K_END }, { "\033[7~", K_HOME }, { "\033[8~", K_END },
        { "\033[5~", PGUP }, { "\033[6~", PGDN },
        { "\033[1;2A", K_SUP }, { "\033[1;2B", K_SDOWN },     /* Shift+Up/Down (xterm) */
        { "\033[a", K_SUP }, { "\033[b", K_SDOWN },           /* Shift+Up/Down (rxvt) */
    };
    prompted = 0;
    for (int i = 0; i < n && !prompted;) {   /* a prompt takes the rest of the buffer */
        int k = b[i];
        if (k == 27 && i + 1 < n) {
            size_t j, len = 0;
            for (j = 0; j < sizeof seqs / sizeof *seqs; j++) {
                len = strlen(seqs[j].s);
                if ((size_t)(n - i) >= len && !memcmp(b + i, seqs[j].s, len)) break;
            }
            if (j == sizeof seqs / sizeof *seqs) {           /* unknown: skip just this one */
                int e = i + 1;                              /* ESC + byte (Alt+key) */
                if (b[i + 1] == '[' || b[i + 1] == 'O')     /* CSI/SS3: up to the final byte */
                    for (e = i + 2; e < n && (b[e] < 0x40 || b[e] > 0x7e); e++) {}
                i = e + 1;
                continue;
            }
            k = seqs[j].k;
            i += (int)len;
        } else {
            if (k == 27) k = ESCKEY;
            i++;
        }
        ahead = b + i;
        nahead = n - i;
        key(k);
        nahead = 0;
    }
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    const char *pwd = getenv("PWD"), *h = getenv("FILE_MANAGER_HIDDEN");
    const char *pr = getenv("FILE_MANAGER_PREVIEW");
    struct stat a, b;
    struct sigaction sa = { 0 };

    setlocale(LC_CTYPE, "");
    utf8 = MB_CUR_MAX > 1;

    /* prefer the logical $PWD (keeps symlinked paths) when it is accurate */
    if (pwd && *pwd == '/' && strlen(pwd) < PATH_MAX && !stat(pwd, &a) && !stat(".", &b) &&
        a.st_dev == b.st_dev && a.st_ino == b.st_ino)
        normpath(cwd, "/", pwd);
    else if (!getcwd(cwd, sizeof cwd)) { fputs("file-manager: cannot get cwd\n", stderr); return 1; }

    if (argc > 1) {
        char p[PATH_MAX];
        if (normpath(p, cwd, argv[1]) || chdir(p)) {
            fprintf(stderr, "file-manager: cannot open %s\n", argv[1]);
            return 1;
        }
        strcpy(cwd, p);
    }
    if ((tty = open("/dev/tty", O_RDWR | O_CLOEXEC)) < 0 || tcgetattr(tty, &orig)) {
        fputs("file-manager: no terminal\n", stderr);
        return 1;
    }
    show_hidden = !h || !*h || !strcmp(h, "1");
    show_preview = !pr || strcmp(pr, "0");
#ifdef __linux__
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#endif
    watch();

    sa.sa_handler = on_fatal;           /* restore the terminal, then die */
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_flag;            /* no SA_RESTART: interrupts poll() */
    sigaction(SIGWINCH, &sa, NULL);
    sigaction(SIGCONT, &sa, NULL);
    sa.sa_handler = on_tstp;
    sigaction(SIGTSTP, &sa, NULL);

    get_size();
    setup_terminal();
    list_dir();
    refresh();
    for (;;) {
        unsigned char buf[4096];
        int n = read_input(buf, sizeof buf);
        *msg = 0;
        handle_input(buf, n);
        refresh();
    }
}
