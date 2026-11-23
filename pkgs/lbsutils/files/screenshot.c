/* screenshot - screenshot tool for Wayland: grim + slurp in one
 *
 * Usage:   screenshot [-c] [file | -]
 *            Drag to select a region, or click to take a whole output.
 *            Esc or right click cancels.
 *            -c: also put the PNG on the clipboard. screenshot then waits in the
 *                background, serving it, until something else is copied.
 *            file: the PNG to write; "-" writes to stdout.
 *            Default: $XDG_PICTURES_DIR, ~/Pictures or ., named
 *            YYYYmmdd_HHhMMmSSs_screenshot.png
 *
 * The screen is captured first and you select on that frozen image, so the
 * result appears the moment you let go, and the selection overlay can never
 * end up in the shot. The cursor is not included.
 *
 * Talks the Wayland wire protocol directly (no libwayland) and needs a
 * compositor with wlr-screencopy and wlr-layer-shell (sway, Hyprland, river,
 * Wayfire, labwc, ...), plus ext- or wlr-data-control for -c. Only
 * dependency: zlib, for PNG.
 *
 * Build:   cc -O2 -s -o screenshot screenshot.c -lz      (Alpine: apk add gcc musl-dev zlib-dev)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

static double dmin(double a, double b) { return a < b ? a : b; }
static double dmax(double a, double b) { return a > b ? a : b; }
static int ifloor(double x) { int i = (int)x; return i > x ? i - 1 : i; }
static int iceil(double x) { int i = (int)x; return i < x ? i + 1 : i; }

static const char *made;                /* output file we created: removed if we fail */

static void die(const char *fmt, ...)
{
    va_list ap;
    if (made) unlink(made);
    va_start(ap, fmt);
    fputs("screenshot: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ---------------------------------------------------------------- wire protocol
 * Every message: object id, (size << 16 | opcode), 32-bit arguments; strings
 * are length-prefixed and padded to 4 bytes; fds travel as SCM_RIGHTS. */

enum {                                  /* what each object id is, for dispatch */
    T_NONE, T_REGISTRY, T_SYNC, T_FRAMECB, T_OUTPUT, T_XDGOUT, T_FRAME, T_CAPBUF,
    T_LAYER, T_OVBUF, T_SEAT, T_POINTER, T_KEYBOARD, T_DCSOURCE,
};
enum { MAXID = 1 << 14 };

static int sock;
static uint32_t obuf[4096], msg[512], next_id = 2, freeids[256];
static size_t olen, mlen;
static uint32_t mop;
static int ofds[8], nofds, ifds[16], nifds, nfree;
static unsigned char otype[MAXID];
static void *optr[MAXID];

static uint32_t new_id(int type, void *ptr)
{
    uint32_t id = nfree ? freeids[--nfree] : next_id++;   /* reuse ids after delete_id */
    if (id >= MAXID) die("too many Wayland objects");
    otype[id] = type;
    optr[id] = ptr;
    return id;
}

static void flush(void)
{
    size_t off = 0, len = olen * 4;
    while (off < len) {
        struct iovec iov = { (char *)obuf + off, len - off };
        char cbuf[CMSG_SPACE(sizeof ofds)];
        struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1 };
        if (nofds) {
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

/* string argument at a[i]; *next gets the index after it */
static const char *arg_str(const uint32_t *a, size_t i, size_t *next)
{
    *next = i + 1 + (a[i] + 3) / 4;
    return (const char *)(a + i + 1);
}

static void *shm_buffer(int w, int h, int stride, uint32_t fmt, uint32_t *id, int type, void *ptr, size_t *size)
{
    extern uint32_t shm;
    int fd = memfd_create("screenshot", MFD_CLOEXEC);
    *size = (size_t)stride * h;
    if (fd < 0 || ftruncate(fd, *size) < 0) die("cannot allocate a screen buffer");
    void *p = mmap(NULL, *size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) die("cannot map a screen buffer");
    uint32_t pool = new_id(T_NONE, NULL);
    *id = new_id(type, ptr);
    begin(shm, 0); put(pool); put_fd(fd); put((uint32_t)*size); end();     /* create_pool */
    begin(pool, 0); put(*id); put(0); put(w); put(h); put(stride); put(fmt); end();
    begin(pool, 1); end();                                                  /* pool.destroy */
    flush();
    close(fd);
    return p;
}

/* ---------------------------------------------------------------- state */

enum { MAXOUT = 16, FMT_ARGB = 0, FMT_XRGB = 1, FMT_ABGR = 0x34324241, FMT_XBGR = 0x34324258 };

struct ovbuf { uint32_t id; uint32_t *px; size_t size; int busy, r[4]; };

struct output {
    uint32_t wl, xdg, frame;
    int x, y, w, h;                     /* logical position and size */
    int gx, gy, mw, mh, transform, scale, have_xdg;
    char name[64];
    /* capture */
    int bw, bh, stride, yinv, bgr, ready, failed, chosen;
    uint32_t fmt, buf;
    unsigned char *data;
    size_t size;
    /* selection overlay */
    uint32_t surf, layer, cb;
    int pw, ph, configured, dirty, waiting, shown;
    int last[4];                        /* the bright rect now on screen */
    uint32_t *frozen;                   /* the captured screen at overlay resolution */
    int *ox, *oy;                       /* overlay pixel -> offset into data */
    struct ovbuf ob[2];
};

uint32_t shm;
static uint32_t registry, compositor, copier, layershell, xdgman, seat, pointer, keyboard, shapes;
static uint32_t copier_ver, seat_ver;
static uint32_t dc_ext, dc_wlr;         /* data-control managers, for -c */
static unsigned char *png;              /* the written PNG, served by -c */
static size_t pnglen;
static struct output out[MAXOUT];
static int nout, synced;

static struct {                         /* selection */
    int active, pressed, done, cancelled;
    double ax, ay, cx, cy;              /* anchor and current point, global logical */
    struct output *focus;
} sel;

static void dispatch(uint32_t obj, uint32_t op, const uint32_t *a, size_t n);

static void read_events(void)
{
    static uint32_t ibuf[8192];
    static size_t ilen;
    flush();                            /* anything queued must reach the compositor first */
    char cbuf[CMSG_SPACE(sizeof ifds)];
    struct iovec iov = { (char *)ibuf + ilen, sizeof ibuf - ilen };
    struct msghdr mh = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
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

static void roundtrip(void)
{
    synced = 0;
    uint32_t cb = new_id(T_SYNC, NULL);
    begin(1, 0); put(cb); end();                            /* wl_display.sync */
    while (!synced) read_events();
}

/* ---------------------------------------------------------------- capture
 * The captured buffer is the output's framebuffer, untransformed. A logical
 * point maps to it through the output's scale and transform; each buffer
 * coordinate depends on only one logical coordinate, so the offset of any
 * pixel is xtab[x] + ytab[y]: no per-pixel math when copying. */

static long buf_offset(struct output *o, int tx, int ty)    /* transformed px -> byte offset */
{
    int bx, by, W = o->bw, H = o->bh;
    switch (o->transform) {
    default: bx = tx;         by = ty;         break;       /* normal */
    case 1:  bx = ty;         by = H - 1 - tx; break;       /* 90 */
    case 2:  bx = W - 1 - tx; by = H - 1 - ty; break;       /* 180 */
    case 3:  bx = W - 1 - ty; by = tx;         break;       /* 270 */
    case 4:  bx = W - 1 - tx; by = ty;         break;       /* flipped */
    case 5:  bx = ty;         by = tx;         break;       /* flipped 90 */
    case 6:  bx = tx;         by = H - 1 - ty; break;       /* flipped 180 */
    case 7:  bx = W - 1 - ty; by = H - 1 - tx; break;       /* flipped 270 */
    }
    if (o->yinv) by = H - 1 - by;
    return (long)by * o->stride + (long)bx * 4;
}

/* offsets for n points along x (or y) at logical positions start + (i + 0.5) * step,
 * relative to the output's origin */
static void axis_table(struct output *o, int horizontal, int n, double start, double step, int *tab)
{
    int odd = o->transform & 1, tw = odd ? o->bh : o->bw, th = odd ? o->bw : o->bh;
    long base = buf_offset(o, 0, 0);
    for (int i = 0; i < n; i++) {
        double l = start + (i + 0.5) * step;
        int t = horizontal ? (int)(l * tw / o->w) : (int)(l * th / o->h), lim = horizontal ? tw : th;
        t = t < 0 ? 0 : t >= lim ? lim - 1 : t;
        tab[i] = horizontal ? (int)buf_offset(o, t, 0) : (int)(buf_offset(o, 0, t) - base);
    }
}

static inline uint32_t pixel(struct output *o, long off)    /* -> 0xffRRGGBB */
{
    uint32_t v = *(const uint32_t *)(o->data + off);
    if (o->bgr) v = (v & 0x00ff00) | (v >> 16 & 0xff) | (v & 0xff) << 16;
    return v | 0xff000000u;
}

static void capture(struct output *o)
{
    o->frame = new_id(T_FRAME, o);
    begin(copier, 0); put(o->frame); put(0); put(o->wl); end();        /* capture_output, no cursor */
}

static void frame_copy(struct output *o)    /* the compositor said which buffer it wants */
{
    if (!o->chosen) { o->failed = 1; return; }
    o->data = shm_buffer(o->bw, o->bh, o->stride, o->fmt, &o->buf, T_CAPBUF, o, &o->size);
    begin(o->frame, 0); put(o->buf); end();                 /* copy */
}

/* ---------------------------------------------------------------- selection overlay */

static void sel_rect(double *x0, double *y0, double *x1, double *y1)
{
    *x0 = dmin(sel.ax, sel.cx); *x1 = dmax(sel.ax, sel.cx);
    *y0 = dmin(sel.ay, sel.cy); *y1 = dmax(sel.ay, sel.cy);
}

static void dim_rect(struct output *o, uint32_t *dst, const int *r)    /* frozen screen at half */
{                                                                       /* brightness */
    for (int y = r[1]; y < r[3]; y++) {
        uint32_t *d = dst + (size_t)y * o->pw;
        const uint32_t *f = o->frozen + (size_t)y * o->pw;
        for (int x = r[0]; x < r[2]; x++) d[x] = (f[x] >> 1 & 0x7f7f7f) | 0xff000000u;
    }
}

static void draw_overlay(struct output *o)
{
    struct ovbuf *b = !o->ob[0].busy ? &o->ob[0] : !o->ob[1].busy ? &o->ob[1] : NULL;
    int nr[4] = { 0, 0, 0, 0 }, s = o->scale, bw = 2 * s;
    if (!b || o->waiting) return;       /* drawn when the compositor is ready */
    if (!b->id) {
        int all[4] = { 0, 0, o->pw, o->ph };
        b->px = shm_buffer(o->pw, o->ph, o->pw * 4, FMT_XRGB, &b->id, T_OVBUF, b, &b->size);
        dim_rect(o, b->px, all);
    }
    if (sel.pressed) {                  /* the selection in overlay pixels, with its border */
        double x0, y0, x1, y1;
        sel_rect(&x0, &y0, &x1, &y1);
        int ix0 = ifloor((x0 - o->x) * s), iy0 = ifloor((y0 - o->y) * s);
        int ix1 = iceil((x1 - o->x) * s), iy1 = iceil((y1 - o->y) * s);
        nr[0] = ix0 - bw; nr[1] = iy0 - bw; nr[2] = ix1 + bw; nr[3] = iy1 + bw;
        nr[0] = nr[0] < 0 ? 0 : nr[0]; nr[1] = nr[1] < 0 ? 0 : nr[1];
        nr[2] = nr[2] > o->pw ? o->pw : nr[2]; nr[3] = nr[3] > o->ph ? o->ph : nr[3];
        if (nr[2] <= nr[0] || nr[3] <= nr[1]) memset(nr, 0, sizeof nr);
        if (b->r[2] > b->r[0]) dim_rect(o, b->px, b->r);    /* undo this buffer's last rect */
        int cx0 = ix0 > 0 ? ix0 : 0, cx1 = ix1 < o->pw ? ix1 : o->pw;
        for (int y = nr[1]; y < nr[3]; y++) {           /* border white, inside at full brightness */
            uint32_t *d = b->px + (size_t)y * o->pw;
            if (y < iy0 || y >= iy1 || cx1 <= cx0) {
                for (int x = nr[0]; x < nr[2]; x++) d[x] = 0xffffffffu;
                continue;
            }
            for (int x = nr[0]; x < cx0; x++) d[x] = 0xffffffffu;
            memcpy(d + cx0, o->frozen + (size_t)y * o->pw + cx0, (size_t)(cx1 - cx0) * 4);
            for (int x = cx1; x < nr[2]; x++) d[x] = 0xffffffffu;
        }
    } else if (b->r[2] > b->r[0]) {
        dim_rect(o, b->px, b->r);
    }
    /* damage: the screen is dim apart from the rect shown last, and so is this
     * buffer apart from the new one: what changes is those two rects. It must
     * not depend on which buffer was shown last: GPU renderers keep the last
     * picture and copy in only the damage, and the compositor may hand the
     * same buffer back every time. The very first frame is all new. */
    int dmg[4] = { 0, 0, 0, 0 };
    if (!o->shown) { dmg[2] = o->pw; dmg[3] = o->ph; o->shown = 1; }
    else {
        const int *rs[2] = { nr, o->last };
        for (int k = 0; k < 2; k++) {
            const int *r = rs[k];
            if (r[2] <= r[0]) continue;
            if (dmg[2] <= dmg[0]) { memcpy(dmg, r, sizeof dmg); continue; }
            if (r[0] < dmg[0]) dmg[0] = r[0];
            if (r[1] < dmg[1]) dmg[1] = r[1];
            if (r[2] > dmg[2]) dmg[2] = r[2];
            if (r[3] > dmg[3]) dmg[3] = r[3];
        }
    }
    memcpy(b->r, nr, sizeof nr);
    memcpy(o->last, nr, sizeof nr);
    b->busy = 1;
    o->waiting = 1;
    o->dirty = 0;
    o->cb = new_id(T_FRAMECB, o);
    begin(o->surf, 3); put(o->cb); end();                   /* frame callback */
    begin(o->surf, 1); put(b->id); put(0); put(0); end();   /* attach */
    begin(o->surf, 9); put(dmg[0]); put(dmg[1]); put(dmg[2] - dmg[0]); put(dmg[3] - dmg[1]); end();
    begin(o->surf, 6); end();                               /* commit */
}

static void start_overlay(struct output *o)
{
    o->pw = o->w * o->scale;
    o->ph = o->h * o->scale;
    if (!(o->frozen = malloc((size_t)o->pw * o->ph * 4)) || !(o->ox = malloc(o->pw * sizeof *o->ox)) ||
        !(o->oy = malloc(o->ph * sizeof *o->oy)))
        die("out of memory");
    axis_table(o, 1, o->pw, 0, 1.0 / o->scale, o->ox);
    axis_table(o, 0, o->ph, 0, 1.0 / o->scale, o->oy);
    for (int y = 0; y < o->ph; y++)                         /* the frozen screen */
        for (int x = 0; x < o->pw; x++)
            o->frozen[(size_t)y * o->pw + x] = pixel(o, o->ox[x] + o->oy[y]);
    o->surf = new_id(T_NONE, o);
    begin(compositor, 0); put(o->surf); end();
    o->layer = new_id(T_LAYER, o);
    begin(layershell, 0); put(o->layer); put(o->surf); put(o->wl); put(3); put_str("selection"); end();
    begin(o->layer, 1); put(15); end();                     /* anchor to all edges */
    begin(o->layer, 2); put(-1); end();                     /* cover panels too */
    begin(o->layer, 4); put(1); end();                      /* keyboard: exclusive (Esc) */
    begin(o->surf, 8); put(o->scale); end();                /* set_buffer_scale */
    begin(o->surf, 6); end();
}

static void stop_overlays(void)
{
    for (int i = 0; i < nout; i++) {
        struct output *o = &out[i];
        if (!o->layer) continue;
        begin(o->layer, 7); end();                          /* layer_surface.destroy */
        begin(o->surf, 0); end();                           /* surface.destroy */
        for (int k = 0; k < 2; k++) if (o->ob[k].id) { begin(o->ob[k].id, 0); end(); }
        o->layer = 0;
    }
    flush();
}

static struct output *output_at(double x, double y)
{
    for (int i = 0; i < nout; i++)
        if (x >= out[i].x && x < out[i].x + out[i].w && y >= out[i].y && y < out[i].y + out[i].h) return &out[i];
    return sel.focus;
}

static void sel_changed(void)
{
    for (int i = 0; i < nout; i++) {
        out[i].dirty = 1;
        if (out[i].configured) draw_overlay(&out[i]);
    }
}

/* ---------------------------------------------------------------- events */

static void on_global(uint32_t name, const char *iface, uint32_t v)
{
    uint32_t *id = NULL, ver = 1;
    int type = T_NONE;
    void *ptr = NULL;
    if (!strcmp(iface, "wl_compositor")) { id = &compositor; ver = v < 4 ? v : 4; }
    else if (!strcmp(iface, "wl_shm")) id = &shm;
    else if (!strcmp(iface, "zwlr_screencopy_manager_v1")) { id = &copier; ver = copier_ver = v < 3 ? v : 3; }
    else if (!strcmp(iface, "zwlr_layer_shell_v1")) { id = &layershell; ver = v < 4 ? v : 4; }
    else if (!strcmp(iface, "zxdg_output_manager_v1")) { id = &xdgman; ver = v < 3 ? v : 3; }
    else if (!strcmp(iface, "wp_cursor_shape_manager_v1")) id = &shapes;
    else if (!strcmp(iface, "ext_data_control_manager_v1")) id = &dc_ext;
    else if (!strcmp(iface, "zwlr_data_control_manager_v1")) id = &dc_wlr;
    else if (!strcmp(iface, "wl_seat") && !seat) { id = &seat; type = T_SEAT; ver = seat_ver = v < 4 ? v : 4; }
    else if (!strcmp(iface, "wl_output") && nout < MAXOUT) {
        struct output *o = &out[nout++];
        o->scale = 1;
        id = &o->wl; type = T_OUTPUT; ptr = o; ver = v < 4 ? v : 4;
    }
    if (!id) return;
    *id = new_id(type, ptr);
    begin(registry, 0); put(name); put_str(iface); put(ver); put(*id); end();
}

static void on_pointer(uint32_t op, const uint32_t *a)
{
    double x, y;
    if (op == 0) {                                          /* enter */
        struct output *o = a[1] < MAXID ? optr[a[1]] : NULL;
        if (!o || otype[a[1]] != T_NONE || o->surf != a[1]) return;
        sel.focus = o;
        if (shapes) {
            uint32_t dev = new_id(T_NONE, NULL);
            begin(shapes, 1); put(dev); put(pointer); end();
            begin(dev, 1); put(a[0]); put(8); end();        /* crosshair */
            begin(dev, 0); end();
        }
        x = (int32_t)a[2] / 256.0; y = (int32_t)a[3] / 256.0;
    } else if (op == 2) {                                   /* motion */
        x = (int32_t)a[1] / 256.0; y = (int32_t)a[2] / 256.0;
    } else if (op == 3) {                                   /* button */
        if (a[2] == 0x111 && a[3] == 1) { sel.cancelled = 1; return; }   /* right: cancel */
        if (a[2] != 0x110) return;                          /* left only */
        if (a[3] == 1) { sel.pressed = 1; sel.ax = sel.cx; sel.ay = sel.cy; sel_changed(); }
        else if (sel.pressed) sel.done = 1;
        return;
    } else {
        return;
    }
    if (!sel.focus) return;
    sel.cx = sel.focus->x + x;
    sel.cy = sel.focus->y + y;
    if (sel.pressed) sel_changed();
}

static void dispatch(uint32_t obj, uint32_t op, const uint32_t *a, size_t n)
{
    size_t nx;
    if (obj == 1) {
        if (op == 0 && n >= 3) die("protocol error %u on object %u: %.*s", a[1], a[0], (int)a[2], (const char *)(a + 3));
        if (op == 1 && a[0] < MAXID) {                      /* delete_id: reuse it */
            otype[a[0]] = T_NONE;
            optr[a[0]] = NULL;
            if (nfree < (int)(sizeof freeids / sizeof *freeids)) freeids[nfree++] = a[0];
        }
        return;
    }
    if (obj >= MAXID) return;
    struct output *o = optr[obj];
    switch (otype[obj]) {
    case T_REGISTRY:
        if (op == 0) { const char *iface = arg_str(a, 1, &nx); if (nx < n) on_global(a[0], iface, a[nx]); }
        break;
    case T_SYNC: if (op == 0) synced = 1; break;
    case T_OUTPUT:
        if (op == 0) {                                      /* geometry */
            o->gx = (int32_t)a[0]; o->gy = (int32_t)a[1];
            arg_str(a, 5, &nx); arg_str(a, nx, &nx);
            if (nx < n) o->transform = (int32_t)a[nx] & 7;
        } else if (op == 1 && (a[0] & 1)) { o->mw = (int32_t)a[1]; o->mh = (int32_t)a[2]; }   /* current mode */
        else if (op == 3) o->scale = (int32_t)a[0] > 0 ? (int32_t)a[0] : 1;
        else if (op == 4) snprintf(o->name, sizeof o->name, "%s", arg_str(a, 0, &nx));
        break;
    case T_XDGOUT:
        if (op == 0) { o->x = (int32_t)a[0]; o->y = (int32_t)a[1]; o->have_xdg = 1; }
        else if (op == 1) { o->w = (int32_t)a[0]; o->h = (int32_t)a[1]; }
        else if (op == 3 && !o->name[0]) snprintf(o->name, sizeof o->name, "%s", arg_str(a, 0, &nx));
        break;
    case T_FRAME:
        if (op == 0) {                                      /* buffer: an shm format it can copy into */
            uint32_t f = a[0];
            if (!o->chosen && (f == FMT_ARGB || f == FMT_XRGB || f == FMT_ABGR || f == FMT_XBGR)) {
                o->fmt = f; o->bw = a[1]; o->bh = a[2]; o->stride = a[3]; o->chosen = 1;
                o->bgr = f == FMT_ABGR || f == FMT_XBGR;
            }
            if (copier_ver < 3) frame_copy(o);
        } else if (op == 1) o->yinv = a[0] & 1;
        else if (op == 2) { o->ready = 1; begin(obj, 1); end(); }          /* ready: destroy frame */
        else if (op == 3) { o->failed = 1; begin(obj, 1); end(); }
        else if (op == 6) frame_copy(o);                    /* buffer_done */
        break;
    case T_LAYER:
        if (op == 0) {                                      /* configure */
            begin(obj, 6); put(a[0]); end();                /* ack */
            o->configured = 1;
            draw_overlay(o);
        } else if (op == 1) sel.cancelled = 1;              /* closed */
        break;
    case T_OVBUF: ((struct ovbuf *)optr[obj])->busy = 0; break;   /* release */
    case T_FRAMECB:
        o->waiting = 0;
        if (o->dirty) draw_overlay(o);
        break;
    case T_SEAT:
        if (op == 0) {
            if ((a[0] & 1) && !pointer) { pointer = new_id(T_POINTER, NULL); begin(seat, 0); put(pointer); end(); }
            if ((a[0] & 2) && !keyboard) { keyboard = new_id(T_KEYBOARD, NULL); begin(seat, 1); put(keyboard); end(); }
        }
        break;
    case T_POINTER: on_pointer(op, a); break;
    case T_DCSOURCE:
        if (op == 0) {                                      /* send: write the PNG to the reader */
            int fd = take_fd();
            const unsigned char *p = png;
            size_t left = pnglen;
            while (fd >= 0 && left) {
                ssize_t k = write(fd, p, left);
                if (k < 0 && errno == EINTR) continue;
                if (k <= 0) break;
                p += k;
                left -= k;
            }
            if (fd >= 0) close(fd);
        } else if (op == 1) exit(0);                        /* cancelled: something else was copied */
        break;
    case T_KEYBOARD:
        if (op == 0) { int fd = take_fd(); if (fd >= 0) close(fd); }   /* keymap: not needed */
        else if (op == 3 && a[2] == 1 && a[3] == 1) sel.cancelled = 1;  /* Esc (same key on every layout) */
        break;
    }
}

/* ---------------------------------------------------------------- PNG */

static int out_fd;
static unsigned char zbuf[1 << 16];

static void wr(const void *p, size_t n)
{
    const char *c = p;
    while (n) {
        ssize_t k = write(out_fd, c, n);
        if (k < 0 && errno == EINTR) continue;
        if (k < 0) die("write failed: %s", strerror(errno));
        c += k;
        n -= k;
    }
}

static void be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static void chunk(const char *type, const unsigned char *data, uint32_t len)
{
    unsigned char h[8], c[4];
    be32(h, len);
    memcpy(h + 4, type, 4);
    uLong crc = crc32(crc32(0, (const Bytef *)type, 4), data, len);
    be32(c, (uint32_t)crc);
    wr(h, 8);
    if (len) wr(data, len);
    wr(c, 4);
}

/* RGB PNG. Rows use the "Up" filter (difference to the row above) with zlib's
 * fastest level: on real screens that's within a few percent of the smallest
 * filters, at a fraction of the time. */
static void write_png(const uint32_t *img, int w, int h)
{
    size_t rb = (size_t)w * 3;
    unsigned char ihdr[13], *row = malloc(1 + rb), *prev = calloc(1, rb), *cur = malloc(rb);
    z_stream z = { 0 };
    if (!row || !prev || !cur || deflateInit(&z, Z_BEST_SPEED) != Z_OK) die("out of memory");
    wr("\x89PNG\r\n\x1a\n", 8);
    be32(ihdr, w); be32(ihdr + 4, h);
    memcpy(ihdr + 8, "\x08\x02\x00\x00\x00", 5);           /* 8-bit RGB */
    chunk("IHDR", ihdr, 13);
    z.next_out = zbuf;
    z.avail_out = sizeof zbuf;
    for (int y = 0; y <= h; y++) {
        if (y < h) {
            const uint32_t *s = img + (size_t)y * w;
            unsigned char *c = cur, *t;
            for (int x = 0; x < w; x++) { *c++ = s[x] >> 16; *c++ = s[x] >> 8; *c++ = s[x]; }
            row[0] = 2;                                     /* Up */
            for (size_t k = 0; k < rb; k++) row[1 + k] = cur[k] - prev[k];
            t = prev; prev = cur; cur = t;
            z.next_in = row;
            z.avail_in = 1 + (unsigned)rb;
        }
        int flush_mode = y < h ? Z_NO_FLUSH : Z_FINISH, r;
        do {
            r = deflate(&z, flush_mode);
            if (!z.avail_out || (flush_mode == Z_FINISH && r == Z_STREAM_END)) {
                chunk("IDAT", zbuf, sizeof zbuf - z.avail_out);
                z.next_out = zbuf;
                z.avail_out = sizeof zbuf;
            }
        } while (z.avail_in || (flush_mode == Z_FINISH && r != Z_STREAM_END));
    }
    deflateEnd(&z);
    chunk("IEND", NULL, 0);
    free(row);
    free(prev);
    free(cur);
}

/* ---------------------------------------------------------------- main */

static void usage(int code)
{
    fputs("usage: screenshot [-c] [file | -]\n"
          "  drag to select a region, click to take a whole output; Esc or right click cancels\n"
          "  -c   also put the PNG on the clipboard (served in the background until replaced)\n"
          "  file defaults to ~/Pictures/YYYYmmdd_HHhMMmSSs_screenshot.png; - writes to stdout\n",
          code ? stderr : stdout);
    exit(code);
}

static const char *default_path(void)
{
    static char p[4096];
    char name[64], dir[4000];
    const char *d = getenv("XDG_PICTURES_DIR"), *h = getenv("HOME");
    struct stat st;
    time_t t = time(NULL);
    strftime(name, sizeof name, "%Y%m%d_%Hh%Mm%Ss_screenshot.png", localtime(&t));
    if (d && *d) snprintf(dir, sizeof dir, "%s", d);
    else if (h && snprintf(dir, sizeof dir, "%s/Pictures", h) > 0 && !stat(dir, &st) && S_ISDIR(st.st_mode)) {}
    else snprintf(dir, sizeof dir, ".");
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return p;
}

int main(int argc, char **argv)
{
    const char *path = NULL;
    int rx, ry, rw, rh, i, copy = 0;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) usage(0);
        else if (!strcmp(a, "-c")) copy = 1;
        else if (a[0] == '-' && a[1]) usage(1);
        else if (!path) path = a;
        else usage(1);
    }

    /* open the output first, so a bad path fails before you select anything */
    if (path && !strcmp(path, "-")) out_fd = 1;
    else {
        if (!path) path = default_path();
        if ((out_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)) < 0) die("%s: %s", path, strerror(errno));
        made = path;
    }

    const char *disp = getenv("WAYLAND_DISPLAY"), *rt = getenv("XDG_RUNTIME_DIR");
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (!disp) disp = "wayland-0";
    if (*disp == '/') snprintf(sa.sun_path, sizeof sa.sun_path, "%s", disp);
    else if (rt) snprintf(sa.sun_path, sizeof sa.sun_path, "%s/%s", rt, disp);
    else die("XDG_RUNTIME_DIR is not set");
    if ((sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)) < 0 || connect(sock, (struct sockaddr *)&sa, sizeof sa) < 0)
        die("cannot connect to a Wayland display at %s", sa.sun_path);

    registry = new_id(T_REGISTRY, NULL);
    begin(1, 1); put(registry); end();                      /* get_registry */
    roundtrip();
    if (!copier) die("this compositor doesn't support wlr-screencopy (sway, Hyprland, river, ... do)");
    if (!shm || !nout) die("no outputs");
    if (!layershell || !compositor || !seat) die("this compositor doesn't support wlr-layer-shell (needed to select)");
    if (copy && !dc_ext && !dc_wlr) die("this compositor doesn't support data-control (needed for -c)");
    if (copy && out_fd == 1) die("-c needs a file to write, not -");
    for (i = 0; xdgman && i < nout; i++) {
        out[i].xdg = new_id(T_XDGOUT, &out[i]);
        begin(xdgman, 1); put(out[i].xdg); put(out[i].wl); end();
    }
    roundtrip();                                            /* output details */
    for (i = 0; i < nout; i++) {
        struct output *o = &out[i];
        if (!o->have_xdg) {                                 /* no xdg-output: work it out */
            int odd = o->transform & 1;
            o->x = o->gx; o->y = o->gy;
            o->w = (odd ? o->mh : o->mw) / o->scale;
            o->h = (odd ? o->mw : o->mh) / o->scale;
        }
        if (o->w <= 0 || o->h <= 0) die("output %s has no size", o->name);
    }

    for (i = 0; i < nout; i++) capture(&out[i]);          /* freeze every output */
    for (;;) {                                              /* wait for every copy */
        int pending = 0;
        for (i = 0; i < nout; i++) pending += !out[i].ready && !out[i].failed;
        if (!pending) break;
        read_events();
    }
    for (i = 0; i < nout; i++)
        if (out[i].failed) die("capturing output %s failed", out[i].name);

    for (i = 0; i < nout; i++) start_overlay(&out[i]);     /* select on the frozen screen */
    while (!sel.done && !sel.cancelled) read_events();
    stop_overlays();
    if (sel.cancelled) die("selection cancelled");
    double x0, y0, x1, y1;
    sel_rect(&x0, &y0, &x1, &y1);
    if (x1 - x0 < 2 && y1 - y0 < 2) {                       /* a click: that whole output */
        struct output *o = output_at(sel.cx, sel.cy);
        rx = o->x; ry = o->y; rw = o->w; rh = o->h;
    } else {
        rx = ifloor(x0); ry = ifloor(y0);
        rw = iceil(x1) - rx; rh = iceil(y1) - ry;
    }

    /* compose the region at the highest pixel density among the outputs it touches */
    double S = 0;
    for (i = 0; i < nout; i++) {
        struct output *o = &out[i];
        if (o->x < rx + rw && o->x + o->w > rx && o->y < ry + rh && o->y + o->h > ry)
            S = dmax(S, (double)((o->transform & 1) ? o->bh : o->bw) / o->w);
    }
    if (S <= 0) die("the region is outside every output");
    int W = (int)(rw * S + 0.5), H = (int)(rh * S + 0.5);
    uint32_t *img = calloc((size_t)W * H, 4);
    int *tx = malloc(W * sizeof *tx), *ty = malloc(H * sizeof *ty);
    if (!img || !tx || !ty) die("out of memory");
    for (i = 0; i < nout; i++) {
        struct output *o = &out[i];
        /* destination pixels whose centre falls on this output */
        int x0 = iceil((o->x - rx) * S - 0.5), x1 = iceil((o->x + o->w - rx) * S - 0.5);
        int y0 = iceil((o->y - ry) * S - 0.5), y1 = iceil((o->y + o->h - ry) * S - 0.5);
        x0 = x0 < 0 ? 0 : x0; y0 = y0 < 0 ? 0 : y0; x1 = x1 > W ? W : x1; y1 = y1 > H ? H : y1;
        if (x1 <= x0 || y1 <= y0) continue;
        axis_table(o, 1, x1 - x0, rx - o->x + x0 / S, 1 / S, tx);
        axis_table(o, 0, y1 - y0, ry - o->y + y0 / S, 1 / S, ty);
        for (int y = y0; y < y1; y++) {
            uint32_t *d = img + (size_t)y * W;
            for (int x = x0; x < x1; x++) d[x] = pixel(o, tx[x - x0] + ty[y - y0]);
        }
    }

    write_png(img, W, H);
    if (out_fd != 1 && close(out_fd)) die("%s: %s", path, strerror(errno));
    made = NULL;                        /* the file is complete: keep it whatever follows */
    if (!copy) return 0;

    /* -c: read the PNG back and offer it as the clipboard. Data-control needs
     * no focus or input serial; cclipd and other managers read it at once. */
    struct stat st;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fstat(fd, &st) || !(png = malloc(st.st_size ? st.st_size : 1))) die("%s: cannot read it back", path);
    for (size_t got = 0; got < (size_t)st.st_size;) {
        ssize_t k = read(fd, png + got, st.st_size - got);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) die("%s: cannot read it back", path);
        got += k;
    }
    pnglen = st.st_size;
    close(fd);
    uint32_t man = dc_ext ? dc_ext : dc_wlr;    /* both protocols use the same opcodes */
    uint32_t src = new_id(T_DCSOURCE, NULL), dev = new_id(T_NONE, NULL);
    begin(man, 0); put(src); end();                         /* create_data_source */
    begin(src, 0); put_str("image/png"); end();             /* offer */
    begin(man, 1); put(dev); put(seat); end();              /* get_data_device */
    begin(dev, 0); put(src); end();                         /* set_selection */
    roundtrip();

    /* serve it from the background until something else is copied */
    pid_t pid = fork();
    if (pid < 0) die("fork: %s", strerror(errno));
    if (pid > 0) _exit(0);
    setsid();
    signal(SIGPIPE, SIG_IGN);           /* a reader that goes away must not kill us */
    int nul = open("/dev/null", O_RDWR);
    for (int f = 0; nul >= 0 && f < 3; f++) if (f != sock) dup2(nul, f);   /* detach, keeping the socket */
    if (nul > 2 && nul != sock) close(nul);
    for (;;) read_events();
}
