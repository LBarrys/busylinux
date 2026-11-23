/* viewer - a minimal PDF/image viewer for Wayland; MuPDF is its only dependency
 *
 * Usage:   viewer --image | -i path/to/image     PNG JPEG GIF BMP TIFF PNM PSD JPEG2000 JXR JBIG2
 *          viewer --pdf   | -p path/to/file.pdf
 *
 * Keys:    q / Esc                         quit
 *          Right Down Space PageDown       next page (PDF)
 *          Left Up Backspace PageUp        previous page (PDF)
 *          Home / End                      first / last page (PDF)
 *
 * Talks the Wayland wire protocol directly over the compositor's socket (no
 * libwayland) and reads key names from the compositor's keymap itself (no
 * xkbcommon). Decoding and rendering is done by the system's MuPDF library.
 *
 * Colours: the background is taken from the terminal viewer is started in
 *          (asked with the standard OSC 11 query); without one it is #282828.
 *
 * Build:   cc -O2 -s -o viewer viewer.c -lmupdf -lm -lpthread
 *          (Alpine: apk add gcc musl-dev mupdf-dev; at runtime only mupdf-libs is needed)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <mupdf/fitz.h>

static unsigned bg_r = 0x28, bg_g = 0x28, bg_b = 0x28;   /* replaced by the terminal's background */
enum { MAX_W = 1280, MAX_H = 960 };               /* largest initial window */
enum { A_NONE, A_QUIT, A_NEXT, A_PREV, A_FIRST, A_LAST };

static int running = 1;
static int tq_fd = -1;                  /* terminal being asked for its colour */
static struct termios tq_old;

static void die(const char *fmt, ...)
{
    va_list ap;
    if (tq_fd >= 0) tcsetattr(tq_fd, TCSANOW, &tq_old);
    va_start(ap, fmt);
    fputs("viewer: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ---------------------------------------------------------------- wire protocol
 * Every message is: object id, (size << 16 | opcode), then 32-bit arguments.
 * Strings are length-prefixed and padded to 4 bytes; file descriptors travel
 * separately as SCM_RIGHTS ancillary data. Object id 1 is wl_display. */

static int sock;
static uint32_t next_id = 2;
static uint32_t obuf[2048], msg[512];
static size_t olen, mlen;               /* in 32-bit words */
static uint32_t mop;
static int ofds[8], nofds;
static int ifds[16], nifds;

static uint32_t new_id(void) { return next_id++; }

static void flush(void)
{
    size_t off = 0, len = olen * 4;
    while (off < len) {
        struct iovec iov = { (char *)obuf + off, len - off };
        char cbuf[CMSG_SPACE(sizeof ofds)];
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
        if (nofds) {                    /* fds ride along with the first chunk */
            memset(cbuf, 0, sizeof cbuf);
            mh.msg_control = cbuf;
            mh.msg_controllen = CMSG_SPACE(nofds * sizeof(int));
            struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
            c->cmsg_level = SOL_SOCKET;
            c->cmsg_type = SCM_RIGHTS;
            c->cmsg_len = CMSG_LEN(nofds * sizeof(int));
            memcpy(CMSG_DATA(c), ofds, nofds * sizeof(int));
        }
        ssize_t n = sendmsg(sock, &mh, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) die("lost connection to the compositor: %s", strerror(errno));
        off += n;
        nofds = 0;
    }
    olen = 0;
}

static void begin(uint32_t obj, uint32_t op) { msg[0] = obj; mlen = 2; mop = op; }
static void put(uint32_t v) { msg[mlen++] = v; }
static void put_fd(int fd) { ofds[nofds++] = fd; }

static void put_str(const char *s)
{
    size_t n = strnlen(s, 1000) + 1;
    msg[mlen] = n;
    memset(&msg[mlen + 1], 0, (n + 3) & ~3u);
    memcpy(&msg[mlen + 1], s, n - 1);
    mlen += 1 + (n + 3) / 4;
}

static void end(void)
{
    msg[1] = (uint32_t)(mlen * 4) << 16 | mop;
    if (olen + mlen > sizeof obuf / 4 || nofds == (int)(sizeof ofds / sizeof *ofds)) flush();
    memcpy(obuf + olen, msg, mlen * 4);
    olen += mlen;
}

static int take_fd(void)
{
    if (!nifds) return -1;
    int fd = ifds[0];
    memmove(ifds, ifds + 1, --nifds * sizeof *ifds);
    return fd;
}

static void wl_connect(void)
{
    const char *d = getenv("WAYLAND_DISPLAY"), *r = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (!d) d = "wayland-0";
    if (*d == '/') snprintf(sa.sun_path, sizeof sa.sun_path, "%s", d);
    else if (r) snprintf(sa.sun_path, sizeof sa.sun_path, "%s/%s", r, d);
    else die("XDG_RUNTIME_DIR is not set");
    if ((sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 ||
        connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0)
        die("cannot connect to a Wayland display at %s", sa.sun_path);
}

/* ---------------------------------------------------------------- keymap
 * The compositor sends its keymap as XKB text. We only need the first symbol
 * of each key, so a few string searches are enough: xkb_keycodes gives
 * "<NAME> = code;", xkb_symbols gives "key <NAME> { [ sym, ... ] };". */

static unsigned char act[256];          /* xkb keycode -> action */

static void default_keys(void)          /* evdev positions (US) if there's no keymap */
{
    memset(act, 0, sizeof act);
    act[9] = act[24] = A_QUIT;          /* Esc, q */
    act[65] = act[114] = act[116] = act[117] = A_NEXT;  /* Space, Right, Down, PgDn */
    act[22] = act[113] = act[111] = act[112] = A_PREV;  /* BkSp, Left, Up, PgUp */
    act[110] = A_FIRST;                 /* Home */
    act[115] = A_LAST;                  /* End */
}

static int sym_action(const char *s, size_t n)
{
    static const struct { const char *s; char a; } t[] = {
        { "q", A_QUIT }, { "Q", A_QUIT }, { "Escape", A_QUIT },
        { "Right", A_NEXT }, { "Down", A_NEXT }, { "space", A_NEXT },
        { "Next", A_NEXT }, { "Page_Down", A_NEXT },
        { "Left", A_PREV }, { "Up", A_PREV }, { "BackSpace", A_PREV },
        { "Prior", A_PREV }, { "Page_Up", A_PREV },
        { "Home", A_FIRST }, { "End", A_LAST },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (strlen(t[i].s) == n && !memcmp(t[i].s, s, n)) return t[i].a;
    return A_NONE;
}

static void parse_keymap(const char *km)
{
    static struct { char name[16]; int code; } kc[1024];
    unsigned char tmp[256] = { 0 };
    int nk = 0, found = 0;
    const char *sec = strstr(km, "xkb_keycodes"), *sym = strstr(km, "xkb_symbols");
    const char *secend = sec ? strstr(sec, "};") : NULL;
    if (!sec || !sym || !secend) return;

    for (const char *p = sec; (p = strchr(p, '<')) && p < secend && nk < 1024;) {
        const char *e = strchr(p, '>'), *q;
        if (!e || e - p - 1 >= 16) break;
        for (q = e + 1; *q == ' ' || *q == '\t'; q++) {}
        if (*q == '=') {
            for (q++; *q == ' ' || *q == '\t'; q++) {}
            int code = -1;
            if (isdigit((unsigned char)*q)) code = atoi(q);
            else if (*q == '<')        /* alias <A> = <B>; */
                for (int i = 0; i < nk; i++)
                    if (!strncmp(kc[i].name, q + 1, strlen(kc[i].name)) && q[1 + strlen(kc[i].name)] == '>') {
                        code = kc[i].code;
                        break;
                    }
            if (code >= 0) {
                memcpy(kc[nk].name, p + 1, e - p - 1);
                kc[nk].name[e - p - 1] = 0;
                kc[nk++].code = code;
            }
        }
        p = e + 1;
    }
    for (const char *p = sym; (p = strstr(p, "key <"));) {
        const char *n = p + 5, *e = strchr(n, '>'), *blk = e ? strstr(e, "};") : NULL;
        int code = -1;
        if (!blk) break;
        for (int i = 0; i < nk; i++)
            if ((size_t)(e - n) == strlen(kc[i].name) && !memcmp(kc[i].name, n, e - n)) {
                code = kc[i].code;
                break;
            }
        for (const char *b = e; code >= 0 && code < 256 && (b = memchr(b, '[', blk - b)); b++) {
            const char *pr = b - 1;     /* the symbol list follows '{' or '=' */
            while (*pr == ' ' || *pr == '\t' || *pr == '\n') pr--;
            if (*pr != '{' && *pr != '=') continue;
            const char *s = b + 1;
            while (*s == ' ' || *s == '\t') s++;
            size_t len = 0;
            while (isalnum((unsigned char)s[len]) || s[len] == '_') len++;
            if ((tmp[code] = sym_action(s, len))) found = 1;
            break;
        }
        p = e;
    }
    if (found) memcpy(act, tmp, sizeof act);
}

/* ---------------------------------------------------------------- terminal colour
 * Ask the controlling terminal for its background with OSC 11; it answers
 * "ESC ] 11 ; rgb:RRRR/GGGG/BBBB" ending in BEL or ESC \. Only done when we
 * are in the terminal's foreground (touching it from the background would
 * stop us), and we give up after 100 ms if the terminal doesn't reply. */

static unsigned hexcol(const char **p)
{
    char *e;
    unsigned long v = strtoul(*p, &e, 16);
    int n = (int)(e - *p);
    *p = *e == '/' ? e + 1 : e;
    if (n < 1 || n > 4) return 256;
    return (unsigned)(v * 255 / ((1ul << (4 * n)) - 1));
}

static long mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

static long tq_start;

static void tq_signal(int sig)          /* Ctrl-C while asking: give the terminal back */
{
    if (tq_fd >= 0) tcsetattr(tq_fd, TCSANOW, &tq_old);
    _exit(128 + sig);
}

/* send the query now; the answer is read by term_query_end() once the file
 * has loaded, so the terminal's round trip doesn't add to startup time */
static void term_query_begin(void)
{
    struct termios raw;
    int fd = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) return;
    if (tcgetpgrp(fd) != getpgrp() || tcgetattr(fd, &tq_old)) { close(fd); return; }
    raw = tq_old;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    signal(SIGINT, tq_signal);
    signal(SIGTERM, tq_signal);
    tcsetattr(fd, TCSANOW, &raw);
    if (write(fd, "\033]11;?\033\\", 8) != 8) { tcsetattr(fd, TCSANOW, &tq_old); close(fd); return; }
    tq_fd = fd;
    tq_start = mono_ms();
}

static void term_query_end(void)
{
    char buf[128];
    size_t len = 0;
    if (tq_fd < 0) return;
    struct pollfd pf = { tq_fd, POLLIN, 0 };
    while (len < sizeof buf - 1) {
        long left = 100 - (mono_ms() - tq_start);
        if (poll(&pf, 1, left > 0 ? (int)left : 0) <= 0) break;
        ssize_t n = read(tq_fd, buf + len, sizeof buf - 1 - len);
        if (n <= 0) break;
        len += n;
        if (memchr(buf, '\a', len) || (len >= 2 && memmem(buf, len, "\033\\", 2))) break;
    }
    tcsetattr(tq_fd, TCSANOW, &tq_old);
    close(tq_fd);
    tq_fd = -1;
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    buf[len] = 0;
    const char *p = strstr(buf, "rgb:");
    if (!p) return;
    p += 4;
    unsigned r = hexcol(&p), g = hexcol(&p), b = hexcol(&p);
    if (r < 256 && g < 256 && b < 256) { bg_r = r; bg_g = g; bg_b = b; }
}

/* ---------------------------------------------------------------- content */

static fz_context *ctx;
static fz_document *doc;                /* PDF mode */
static fz_image *img;                   /* image mode (decoded on demand) */
static int page, npages = 1;
static const char *name;

/* Rendered pages at the current window size: the visible one plus its
 * neighbours, which are rendered while idle so that page flips are instant. */
enum { NCACHE = 3, MAXTHREADS = 8 };
static struct { fz_pixmap *pm; int page, w, h; } cache[NCACHE];
static struct { fz_display_list *dl; int page; } lists[NCACHE];   /* recorded pages */
static int nthreads = 1;

/* ---- multithreaded rendering: MuPDF needs locks to share its context */
static pthread_mutex_t locks[FZ_LOCK_MAX];
static void lock(void *u, int i) { (void)u; pthread_mutex_lock(&locks[i]); }
static void unlock(void *u, int i) { (void)u; pthread_mutex_unlock(&locks[i]); }
static fz_locks_context lockctx = { NULL, lock, unlock };

struct band { fz_context *ctx; fz_display_list *dl; fz_matrix m; fz_irect r; unsigned char *px; int err; };

/* rasterise one horizontal band of the page straight into the shared pixmap */
static void *render_band(void *arg)
{
    struct band *b = arg;
    fz_context *c = b->ctx;
    fz_pixmap *bp = NULL;
    fz_device *dev = NULL;
    fz_var(bp); fz_var(dev);
    fz_try(c) {
        bp = fz_new_pixmap_with_bbox_and_data(c, fz_device_bgr(c), b->r, NULL, 1, b->px);
        fz_clear_pixmap_with_value(c, bp, 255);
        dev = fz_new_draw_device(c, fz_identity, bp);
        fz_run_display_list(c, b->dl, dev, b->m, fz_rect_from_irect(b->r), NULL);
        fz_close_device(c, dev);
    }
    fz_always(c) { fz_drop_device(c, dev); fz_drop_pixmap(c, bp); }
    fz_catch(c) b->err = 1;
    return NULL;
}

/* a page's display list: its content interpreted once, then re-rasterised at
 * any size (resizes skip parsing) and by several threads at once */
static fz_display_list *get_list(int pg)
{
    for (int i = 0; i < NCACHE; i++)
        if (lists[i].dl && lists[i].page == pg) return lists[i].dl;
    fz_page *p = fz_load_page(ctx, doc, pg);
    fz_display_list *dl = NULL;
    fz_try(ctx) dl = fz_new_display_list_from_page(ctx, p);
    fz_always(ctx) fz_drop_page(ctx, p);
    fz_catch(ctx) fz_rethrow(ctx);
    int v = 0, worst = -1;
    for (int i = 0; i < NCACHE; i++) {  /* evict: empty slot, else the farthest page */
        int score = !lists[i].dl ? 1 << 30 : abs(lists[i].page - page);
        if (score > worst) { worst = score; v = i; }
    }
    fz_drop_display_list(ctx, lists[v].dl);
    lists[v].dl = dl;
    lists[v].page = pg;
    return dl;
}

/* natural size of the content in pixels: image pixels, or PDF points at 96 dpi */
static void natural_size(float *w, float *h)
{
    if (img) {
        *w = img->w;
        *h = img->h;
        return;
    }
    fz_rect b = fz_empty_rect;
    fz_page *p = NULL;
    fz_var(b); fz_var(p);
    fz_try(ctx) {
        p = fz_load_page(ctx, doc, page);
        b = fz_bound_page(ctx, p);
    }
    fz_always(ctx) fz_drop_page(ctx, p);
    fz_catch(ctx) fprintf(stderr, "viewer: %s\n", fz_caught_message(ctx));
    *w = (b.x1 - b.x0) * 96 / 72;
    *h = (b.y1 - b.y0) * 96 / 72;
}

/* Render page pg (or the image) to fit w x h, as BGR(A) ready to copy.
 * PDF pages are rendered at the exact scale (sharp text at any size) onto
 * opaque white, so whole rows can be memcpy'd. Images are only shrunk, and
 * JPEGs are decoded straight at 1/2, 1/4 or 1/8 size when that's enough. */
static fz_pixmap *render_page(int pg, int w, int h)
{
    fz_pixmap *pm = NULL, *raw = NULL, *tmp = NULL;
    fz_page *p = NULL;
    fz_device *dev = NULL;
    fz_var(pm); fz_var(raw); fz_var(tmp); fz_var(p); fz_var(dev);
    fz_try(ctx) {
        if (img) {
            float s = fminf(1, fminf((float)w / img->w, (float)h / img->h));
            int tw = (int)fmaxf(1, img->w * s), th = (int)fmaxf(1, img->h * s);
            fz_matrix m = fz_scale(tw, th);
            raw = fz_get_pixmap_from_image(ctx, img, NULL, &m, NULL, NULL);
            tmp = fz_convert_pixmap(ctx, raw, fz_device_bgr(ctx), NULL, NULL, fz_default_color_params, 1);
            if (fz_pixmap_width(ctx, tmp) != tw || fz_pixmap_height(ctx, tmp) != th)
                pm = fz_scale_pixmap(ctx, tmp, 0, 0, tw, th, NULL);
            else { pm = tmp; tmp = NULL; }
        } else {
            fz_display_list *dl = get_list(pg);
            fz_rect b = fz_bound_display_list(ctx, dl);
            float s = fminf(w / (b.x1 - b.x0), h / (b.y1 - b.y0));
            fz_matrix m = fz_scale(s, s);
            fz_irect r = fz_round_rect(fz_transform_rect(b, m));
            pm = fz_new_pixmap_with_bbox(ctx, fz_device_bgr(ctx), r, NULL, 1);
            /* split the page into horizontal bands, one per core (at least 64 rows each) */
            int rows = r.y1 - r.y0, nb = nthreads < rows / 64 ? nthreads : rows / 64, err = 0;
            pthread_t th[MAXTHREADS];
            struct band bd[MAXTHREADS];
            if (nb < 1) nb = 1;
            for (int i = 0; i < nb; i++) {
                bd[i].r = r;
                bd[i].r.y0 = r.y0 + rows * i / nb;
                bd[i].r.y1 = r.y0 + rows * (i + 1) / nb;
                bd[i].px = fz_pixmap_samples(ctx, pm) + (size_t)(bd[i].r.y0 - r.y0) * fz_pixmap_stride(ctx, pm);
                bd[i].dl = dl;
                bd[i].m = m;
                bd[i].err = 0;
                bd[i].ctx = i ? fz_clone_context(ctx) : ctx;
                if (i && (!bd[i].ctx || pthread_create(&th[i], NULL, render_band, &bd[i]))) {
                    if (bd[i].ctx) fz_drop_context(bd[i].ctx);
                    bd[i].ctx = ctx;            /* can't start a thread: do it here instead */
                    render_band(&bd[i]);
                    bd[i].ctx = NULL;
                }
            }
            render_band(&bd[0]);
            for (int i = 1; i < nb; i++)
                if (bd[i].ctx) { pthread_join(th[i], NULL); fz_drop_context(bd[i].ctx); }
            for (int i = 0; i < nb; i++) err |= bd[i].err;
            if (err) fz_throw(ctx, FZ_ERROR_GENERIC, "cannot render page");
        }
    }
    fz_always(ctx) {
        fz_drop_device(ctx, dev);
        fz_drop_page(ctx, p);
        fz_drop_pixmap(ctx, raw);
        fz_drop_pixmap(ctx, tmp);
    }
    fz_catch(ctx) {
        fz_drop_pixmap(ctx, pm);
        pm = NULL;
        fprintf(stderr, "viewer: page %d: %s\n", pg + 1, fz_caught_message(ctx));
    }
    return pm;
}

static int cached(int pg, int w, int h)
{
    for (int i = 0; i < NCACHE; i++)
        if (cache[i].pm && cache[i].page == pg && cache[i].w == w && cache[i].h == h) return i;
    return -1;
}

static fz_pixmap *get_page(int pg, int w, int h)
{
    int i = cached(pg, w, h), v = 0, worst = -1;
    if (i >= 0) return cache[i].pm;
    fz_pixmap *pm = render_page(pg, w, h);
    if (!pm) return NULL;
    for (i = 0; i < NCACHE; i++) {      /* evict: empty, other size, or farthest page */
        int score = !cache[i].pm ? 1 << 30 : (cache[i].w != w || cache[i].h != h) ? (1 << 30) - 1
                  : abs(cache[i].page - page);
        if (score > worst) { worst = score; v = i; }
    }
    fz_drop_pixmap(ctx, cache[v].pm);
    cache[v].pm = pm;
    cache[v].page = pg;
    cache[v].w = w;
    cache[v].h = h;
    return pm;
}

/* Check the file's contents (not its name) match what was asked for:
 * -p takes only PDFs, -i takes only image formats. */
static void check_type(const char *path, int pdf)
{
    unsigned char h[1024] = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) die("%s: %s", path, strerror(errno));
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    int is_pdf = memmem(h, n, "%PDF-", 5) != NULL;  /* may follow up to 1 KB of junk */
    int is_img = n >= 8 && fz_recognize_image_format(ctx, h) != FZ_IMAGE_UNKNOWN;
    if (pdf && !is_pdf) die("%s: not a PDF file%s", path, is_img ? " (it's an image: use --image)" : "");
    if (!pdf && !is_img) die("%s: not a supported image file%s", path, is_pdf ? " (it's a PDF: use --pdf)" : "");
}

static void load(const char *path, int pdf)
{
    for (int i = 0; i < FZ_LOCK_MAX; i++) pthread_mutex_init(&locks[i], NULL);
    if (!(ctx = fz_new_context(NULL, &lockctx, FZ_STORE_DEFAULT))) die("cannot create MuPDF context");
    long nc = sysconf(_SC_NPROCESSORS_ONLN);
    nthreads = nc < 1 ? 1 : nc > MAXTHREADS ? MAXTHREADS : (int)nc;
    check_type(path, pdf);
    fz_try(ctx) {
        if (pdf) {
            fz_register_document_handlers(ctx);
            fz_stream *stm = fz_open_file(ctx, path);
            fz_try(ctx) doc = fz_open_document_with_stream(ctx, "application/pdf", stm);
            fz_always(ctx) fz_drop_stream(ctx, stm);   /* the document keeps its own reference */
            fz_catch(ctx) fz_rethrow(ctx);
            if (fz_needs_password(ctx, doc)) fz_throw(ctx, FZ_ERROR_GENERIC, "password protected");
            npages = fz_count_pages(ctx, doc);
            if (npages < 1) fz_throw(ctx, FZ_ERROR_GENERIC, "no pages");
        } else {
            img = fz_new_image_from_file(ctx, path);
            if (img->w < 1 || img->h < 1) fz_throw(ctx, FZ_ERROR_GENERIC, "empty image");
        }
    }
    fz_catch(ctx) die("%s: %s", path, fz_caught_message(ctx));
}

/* ---------------------------------------------------------------- window */

static uint32_t registry, compositor, shm, wm, seat, keyboard, deco_mgr;
static uint32_t surface, xsurf, toplevel, sync_cb;
static uint32_t comp_ver, seat_ver;
static int win_w, win_h, pend_w, pend_h, configured, dirty, synced, unlocked;

struct shmbuf { uint32_t id; void *data; size_t size; int w, h, busy; struct shmbuf *next; };
static struct shmbuf *bufs;             /* reused once the compositor releases them */

static void free_buffer(struct shmbuf *sb)
{
    for (struct shmbuf **pp = &bufs; *pp; pp = &(*pp)->next)
        if (*pp == sb) { *pp = sb->next; break; }
    begin(sb->id, 0); end();                                /* wl_buffer.destroy */
    munmap(sb->data, sb->size);
    free(sb);
}

static struct shmbuf *get_buffer(int w, int h)
{
    struct shmbuf *sb, *next;
    for (sb = bufs; sb; sb = next) {    /* a free one of this size, dropping stale sizes */
        next = sb->next;
        if (sb->busy) continue;
        if (sb->w == w && sb->h == h) return sb;
        free_buffer(sb);
    }
    int stride = w * 4, fd = memfd_create("viewer", MFD_CLOEXEC);
    if (!(sb = calloc(1, sizeof *sb)) || fd < 0) die("out of memory");
    sb->size = (size_t)stride * h;
    sb->w = w;
    sb->h = h;
    if (ftruncate(fd, sb->size) < 0) die("ftruncate: %s", strerror(errno));
    sb->data = mmap(NULL, sb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sb->data == MAP_FAILED) die("mmap: %s", strerror(errno));
    uint32_t pool = new_id();
    sb->id = new_id();
    begin(shm, 0); put(pool); put_fd(fd); put((uint32_t)sb->size); end();  /* wl_shm.create_pool */
    begin(pool, 0); put(sb->id); put(0); put(w); put(h); put(stride); put(1); end(); /* XRGB8888 */
    begin(pool, 1); end();                                  /* wl_shm_pool.destroy */
    flush();
    close(fd);
    sb->next = bufs;
    bufs = sb;
    return sb;
}

static inline void fill(uint32_t *d, int n, uint32_t v) { while (n-- > 0) *d++ = v; }

static void set_title(void)
{
    char t[512];
    if (npages > 1) snprintf(t, sizeof t, "%s (%d/%d)", name, page + 1, npages);
    else snprintf(t, sizeof t, "%s", name);
    begin(toplevel, 2); put_str(t); end();                  /* xdg_toplevel.set_title */
}

static void draw(void)
{
    int w = win_w, h = win_h, pw = 0, ph = 0, ox = 0, oy = 0, n = 0, ss = 0, alpha = 0;
    struct shmbuf *sb = get_buffer(w, h);
    uint32_t *px = sb->data, bg = 0xff000000u | bg_r << 16 | bg_g << 8 | bg_b;
    const unsigned char *src = NULL;
    fz_pixmap *pm = get_page(page, w, h);

    if (pm) {
        pw = fz_pixmap_width(ctx, pm);
        ph = fz_pixmap_height(ctx, pm);
        if (pw > w) pw = w;
        if (ph > h) ph = h;
        ox = (w - pw) / 2;
        oy = (h - ph) / 2;
        n = fz_pixmap_components(ctx, pm);
        ss = fz_pixmap_stride(ctx, pm);
        alpha = img && fz_pixmap_alpha(ctx, pm);            /* PDF pages are opaque */
        src = fz_pixmap_samples(ctx, pm);
    }
    for (int y = 0; y < h; y++) {
        uint32_t *d = px + (size_t)y * w;
        if (!src || y < oy || y >= oy + ph) { fill(d, w, bg); continue; }
        const unsigned char *s = src + (size_t)(y - oy) * ss;
        fill(d, ox, bg);
        fill(d + ox + pw, w - ox - pw, bg);
        d += ox;
        if (n == 4 && !alpha) {                             /* BGRA == XRGB in memory */
            memcpy(d, s, (size_t)pw * 4);
        } else if (n == 4) {                                /* premultiplied over the background */
            for (int x = 0; x < pw; x++, s += 4) {
                unsigned k = 255 - s[3];
                d[x] = 0xff000000u | (s[2] + bg_r * k / 255) << 16 | (s[1] + bg_g * k / 255) << 8
                     | (s[0] + bg_b * k / 255);
            }
        } else {
            for (int x = 0; x < pw; x++, s += n) d[x] = 0xff000000u | s[2] << 16 | s[1] << 8 | s[0];
        }
    }

    sb->busy = 1;
    begin(surface, 1); put(sb->id); put(0); put(0); end();  /* wl_surface.attach */
    begin(surface, comp_ver >= 4 ? 9 : 2);                  /* damage_buffer / damage */
    put(0); put(0); put(w); put(h); end();
    begin(surface, 6); end();                               /* wl_surface.commit */
    if (!unlocked) {                    /* mapped as floating: allow resizing again */
        begin(toplevel, 8); put(0); put(0); end();          /* set_min_size(0, 0) */
        begin(toplevel, 7); put(0); put(0); end();          /* set_max_size(0, 0) */
        begin(surface, 6); end();
        unlocked = 1;
    }
    flush();
}

/* While idle, render the neighbouring pages so the next flip is instant.
 * Returns 1 if it rendered one (the caller then checks for input again). */
static int prefetch(void)
{
    struct pollfd pf = { sock, POLLIN, 0 };
    int cand[2] = { page + 1, page - 1 };
    if (!doc || !configured) return 0;
    for (int i = 0; i < 2; i++) {
        if (cand[i] < 0 || cand[i] >= npages || cached(cand[i], win_w, win_h) >= 0) continue;
        if (poll(&pf, 1, 0) > 0) return 0;                  /* input waiting: that comes first */
        get_page(cand[i], win_w, win_h);
        return 1;
    }
    return 0;
}

static void sync_roundtrip(void);

static void on_global(uint32_t gname, const char *iface, uint32_t v)
{
    uint32_t *id = NULL, ver = 1;
    if (!strcmp(iface, "wl_compositor")) { id = &compositor; ver = comp_ver = v < 4 ? v : 4; }
    else if (!strcmp(iface, "wl_shm")) id = &shm;
    else if (!strcmp(iface, "xdg_wm_base")) id = &wm;
    else if (!strcmp(iface, "wl_seat") && !seat) { id = &seat; ver = seat_ver = v < 4 ? v : 4; }
    else if (!strcmp(iface, "zxdg_decoration_manager_v1")) id = &deco_mgr;
    if (!id) return;
    *id = new_id();
    begin(registry, 0); put(gname); put_str(iface); put(ver); put(*id); end();  /* bind */
}

static void on_key(uint32_t key, uint32_t state)
{
    int p = page;
    if (state != 1 || key + 8 > 255) return;                /* pressed only */
    switch (act[key + 8]) {
    case A_QUIT: running = 0; return;
    case A_NEXT: p++; break;
    case A_PREV: p--; break;
    case A_FIRST: p = 0; break;
    case A_LAST: p = npages - 1; break;
    }
    if (p < 0) p = 0;
    if (p >= npages) p = npages - 1;
    if (p != page) { page = p; set_title(); dirty = 1; }
}

static void on_keymap(uint32_t format, int fd, uint32_t size)
{
    if (fd < 0) return;
    char *m = format == 1 ? mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0) : MAP_FAILED;
    close(fd);
    if (m == MAP_FAILED) return;
    char *km = malloc(size + 1);                            /* NUL-terminated copy */
    if (km) {
        memcpy(km, m, size);
        km[size] = 0;
        parse_keymap(km);
        free(km);
    }
    munmap(m, size);
}

static void dispatch(uint32_t obj, uint32_t op, const uint32_t *a, size_t n)
{
    if (obj == 1) {                                         /* wl_display */
        if (op == 0 && n >= 3) die("protocol error %u on object %u: %.*s", a[1], a[0],
                                    (int)a[2], (const char *)(a + 3));
        return;                                             /* delete_id */
    }
    if (obj == registry && op == 0 && n >= 3) {
        size_t sl = (a[1] + 3) / 4;
        if (2 + sl < n) on_global(a[0], (const char *)(a + 2), a[2 + sl]);
    } else if (obj == sync_cb && op == 0) {
        synced = 1;
    } else if (obj == wm && op == 0) {
        begin(wm, 3); put(a[0]); end();                     /* pong */
    } else if (obj == xsurf && op == 0) {
        begin(xsurf, 4); put(a[0]); end();                  /* ack_configure */
        if (pend_w > 0 && pend_h > 0 && (pend_w != win_w || pend_h != win_h)) {
            win_w = pend_w;
            win_h = pend_h;
            dirty = 1;
        }
        if (!configured) configured = dirty = 1;
    } else if (obj == toplevel) {
        if (op == 0) { pend_w = (int32_t)a[0]; pend_h = (int32_t)a[1]; }  /* 0: our choice */
        else if (op == 1) running = 0;                      /* close */
    } else if (obj == seat && op == 0) {
        if ((a[0] & 2) && !keyboard) {                      /* keyboard capability */
            keyboard = new_id();
            begin(seat, 1); put(keyboard); end();
        } else if (!(a[0] & 2) && keyboard) {
            if (seat_ver >= 3) { begin(keyboard, 0); end(); }   /* release */
            keyboard = 0;
        }
    } else if (keyboard && obj == keyboard) {
        if (op == 0) on_keymap(a[0], take_fd(), a[1]);
        else if (op == 3) on_key(a[2], a[3]);
    } else {
        for (struct shmbuf *sb = bufs; sb; sb = sb->next)
            if (sb->id == obj) {                            /* wl_buffer.release */
                sb->busy = 0;
                if (sb->w != win_w || sb->h != win_h) free_buffer(sb);
                break;
            }
    }
}

/* read once from the socket, then dispatch every complete message */
static void read_events(void)
{
    static uint32_t ibuf[4096];
    static size_t ilen;
    char cbuf[CMSG_SPACE(sizeof ifds)];
    struct iovec iov = { (char *)ibuf + ilen, sizeof ibuf - ilen };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1,
                         .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    ssize_t n = recvmsg(sock, &mh, MSG_CMSG_CLOEXEC);
    if (n < 0 && errno == EINTR) return;
    if (n <= 0) die("the compositor closed the connection");
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            int k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int), *f = (int *)CMSG_DATA(c);
            for (int i = 0; i < k; i++)
                if (nifds < (int)(sizeof ifds / sizeof *ifds)) ifds[nifds++] = f[i];
                else close(f[i]);
        }
    ilen += n;
    size_t off = 0;
    while (ilen - off >= 8) {
        const uint32_t *m = (const uint32_t *)((char *)ibuf + off);
        uint32_t size = m[1] >> 16;
        if (size < 8 || size % 4) die("malformed message from the compositor");
        if (ilen - off < size) break;
        dispatch(m[0], m[1] & 0xffff, m + 2, (size - 8) / 4);
        off += size;
    }
    memmove(ibuf, (char *)ibuf + off, ilen - off);
    ilen -= off;
}

static void sync_roundtrip(void)
{
    synced = 0;
    sync_cb = new_id();
    begin(1, 0); put(sync_cb); end();                       /* wl_display.sync */
    flush();
    while (!synced) read_events();
}

/* ---------------------------------------------------------------- main */

static void usage(int code)
{
    fputs("usage: viewer --image|-i path/to/image\n"
          "       viewer --pdf|-p path/to/file.pdf\n", code ? stderr : stdout);
    exit(code);
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int pdf = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage(0);
        if (!strcmp(a, "-i") || !strcmp(a, "--image")) pdf = 0;
        else if (!strcmp(a, "-p") || !strcmp(a, "--pdf")) pdf = 1;
        else usage(1);
        if (path || i + 1 >= argc) usage(1);
        path = argv[++i];
    }
    if (!path) usage(1);
    name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

    term_query_begin();                 /* answered while the file loads */
    load(path, pdf);
    default_keys();
    wl_connect();
    registry = new_id();
    begin(1, 1); put(registry); end();                      /* wl_display.get_registry */
    sync_roundtrip();
    if (!compositor || !shm || !wm) die("compositor lacks wl_compositor, wl_shm or xdg_wm_base");

    /* initial size: the content's natural size, shrunk to fit MAX_W x MAX_H */
    float nw, nh;
    natural_size(&nw, &nh);
    if (nw < 1 || nh < 1) nw = nh = 512;
    float s = fminf(1, fminf(MAX_W / nw, MAX_H / nh));
    win_w = nw * s > 64 ? (int)(nw * s) : 64;
    win_h = nh * s > 64 ? (int)(nh * s) : 64;

    surface = new_id();
    begin(compositor, 0); put(surface); end();              /* create_surface */
    xsurf = new_id();
    begin(wm, 2); put(xsurf); put(surface); end();          /* get_xdg_surface */
    toplevel = new_id();
    begin(xsurf, 1); put(toplevel); end();                  /* get_toplevel */
    begin(toplevel, 3); put_str("viewer"); end();        /* set_app_id */
    set_title();
    /* Tiling compositors (sway, Hyprland, ...) float a window whose minimum and
     * maximum sizes are equal, so report a fixed size until the first frame. */
    begin(toplevel, 8); put(win_w); put(win_h); end();     /* set_min_size */
    begin(toplevel, 7); put(win_w); put(win_h); end();     /* set_max_size */
    if (deco_mgr) {                     /* ask for server-side borders if offered */
        uint32_t deco = new_id();
        begin(deco_mgr, 1); put(deco); put(toplevel); end();
        begin(deco, 1); put(2); end();                      /* set_mode(server_side) */
    }
    begin(surface, 6); end();                               /* initial commit */
    flush();
    term_query_end();
    if (!get_page(page, win_w, win_h) && img) die("%s: cannot decode image", path);  /* while the window is set up */

    while (running) {
        if (dirty && configured) { dirty = 0; draw(); }
        flush();
        if (running && !prefetch()) read_events();
    }
    close(sock);
    for (int i = 0; i < NCACHE; i++) {
        fz_drop_pixmap(ctx, cache[i].pm);
        fz_drop_display_list(ctx, lists[i].dl);
    }
    fz_drop_image(ctx, img);
    fz_drop_document(ctx, doc);
    fz_drop_context(ctx);
    return 0;
}
