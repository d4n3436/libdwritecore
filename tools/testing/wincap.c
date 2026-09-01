/* wincap.c - capture a window's pixels from inside a Windows guest, to a file
 * or over TCP.
 *
 *   wincap <out.ppm|-> [--class NAME] [--title SUBSTRING] [--screen] [--list]
 *   wincap --serve PORT [--class NAME] [--title SUBSTRING]
 *
 * --serve answers one request per connection, the shape the sweep's grabber
 * wants:
 *
 *   KEEP                 -> "OK\n", and the connection then takes further
 *                           requests instead of closing after one
 *   SIZE                 -> "W H\n", the matched window's size
 *   GRAB  x y w h        -> "P6\nW H\n255\n" then w*h*3 bytes of RGB
 *   GRABZ x y w h        -> "PK\nW H N\n" then N bytes, PackBits over whole
 *                           pixels, which a page of text compresses 20 to 50
 *                           times
 *
 * Rectangles are in window coordinates, cropped from a fresh capture of the
 * whole window, so a full frame and a later viewport crop share one origin.
 *
 * QEMU's screendump reads the emulated framebuffer, so it can only photograph
 * what a console is scanning out. This reads the window itself, through
 * PrintWindow with PW_RENDERFULLCONTENT, which asks the window to render into
 * a DC of ours. BitBlt from the screen DC is not usable on the serve path: it
 * returns the desktop, and returns black for a desktop nothing is displaying.
 *
 * --screen captures the composited desktop instead, which is what screendump
 * sees, for comparing the two against each other.
 */
/* winsock2.h has to come before windows.h, which pulls in the older winsock. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char* g_class;
static const char* g_title;
static HWND g_found;

static BOOL CALLBACK pick(HWND hwnd, LPARAM unused)
{
    (void)unused;
    char cls[256] = {0};
    char txt[512] = {0};
    GetClassNameA(hwnd, cls, sizeof(cls) - 1);
    GetWindowTextA(hwnd, txt, sizeof(txt) - 1);
    if (g_class != NULL && strcmp(cls, g_class) != 0) {
        return TRUE;
    }
    if (g_title != NULL && strstr(txt, g_title) == NULL) {
        return TRUE;
    }
    RECT r;
    if (!GetWindowRect(hwnd, &r)) {
        return TRUE;
    }
    if (r.right - r.left < 200 || r.bottom - r.top < 200) {
        return TRUE;           /* tooltips, tray helpers */
    }
    g_found = hwnd;
    return FALSE;
}

/* The matched window's size, without capturing it. Looked up per request, so
 * a browser that restarted mid-sweep is picked up again. */
static int window_size(int* w, int* h)
{
    g_found = NULL;
    EnumWindows(pick, 0);
    if (g_found == NULL) {
        return 0;
    }
    RECT r;
    if (!GetWindowRect(g_found, &r)) {
        return 0;
    }
    *w = (int)(r.right - r.left);
    *h = (int)(r.bottom - r.top);
    return *w > 0 && *h > 0;
}

/* The whole window, top-down BGRA, freshly allocated; the caller frees with
 * free(). PrintWindow asks the window to render itself, which is what makes
 * this work on a disconnected session. BitBlt from the screen DC is the route
 * that returns black there, so nothing on the serve path may use it. */
static unsigned char* capture_window(int* out_w, int* out_h)
{
    int w = 0, h = 0;
    if (!window_size(&w, &h)) {
        return NULL;
    }
    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old_obj = SelectObject(mem, dib);
    /* 2 is PW_RENDERFULLCONTENT, which mingw's headers do not always carry. */
    BOOL ok = PrintWindow(g_found, mem, 2);
    if (!ok) {
        ok = PrintWindow(g_found, mem, 0);
    }
    unsigned char* out = NULL;
    if (ok) {
        out = (unsigned char*)malloc((size_t)w * (size_t)h * 4);
        if (out != NULL) {
            memcpy(out, bits, (size_t)w * (size_t)h * 4);
        }
    }
    SelectObject(mem, old_obj);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    *out_w = w;
    *out_h = h;
    return out;
}

/* One rectangle of that window, in window coordinates, zero filled where the
 * request runs past the edge. */
static unsigned char* grab_rect(int x, int y, int w, int h)
{
    int fw = 0, fh = 0;
    unsigned char* frame = capture_window(&fw, &fh);
    if (frame == NULL) {
        return NULL;
    }
    unsigned char* out = (unsigned char*)calloc((size_t)w * (size_t)h, 4);
    if (out == NULL) {
        free(frame);
        return NULL;
    }
    for (int row = 0; row < h; ++row) {
        const int sy = y + row;
        if (sy < 0 || sy >= fh) {
            continue;
        }
        int sx = x, dx = 0, n = w;
        if (sx < 0) { dx = -sx; n -= dx; sx = 0; }
        if (sx + n > fw) { n = fw - sx; }
        if (n > 0) {
            memcpy(out + ((size_t)row * w + dx) * 4,
                   frame + ((size_t)sy * fw + sx) * 4, (size_t)n * 4);
        }
    }
    free(frame);
    return out;
}

/* One connection, one request, one PPM. The harness polls a strip of rows to
 * find its marker and then takes the viewport, so the rectangle is the whole
 * protocol; anything richer would be a second thing to keep in step with the
 * host side. */
/* One request on an open connection. False once the socket is finished with,
 * which is a client that has gone away, a malformed request, or an answer sent
 * on a connection that never asked to be kept. */
static int serve_one(SOCKET c, int* keep)
{
    char req[256] = {0};
    int got = 0;
    int whole = 0;
    /* One byte at a time, which stops exactly at the newline and so cannot eat
     * into the next request on a kept connection. The bytes are already in the
     * socket buffer, so this is syscalls and not round trips. */
    while (got < (int)sizeof(req) - 1) {
        const int n = recv(c, req + got, 1, 0);
        if (n <= 0) { return 0; }
        if (req[got] == '\n') { whole = 1; break; }
        ++got;
    }
    if (!whole) { return 0; }
    req[got] = '\0';

    if (strncmp(req, "KEEP", 4) == 0) {
        *keep = 1;
        send(c, "OK\n", 3, 0);
        return 1;
    }

    int x = 0, y = 0, w = 0, h = 0;
    const int packed = (strncmp(req, "GRABZ", 5) == 0);
    if (strncmp(req, "SIZE", 4) == 0) {
        char line[64];
        int fw = 0, fh = 0;
        window_size(&fw, &fh);
        const int n = snprintf(line, sizeof(line), "%d %d\n", fw, fh);
        send(c, line, n, 0);
        return *keep;
    }
    if (sscanf(req + (packed ? 5 : 4), " %d %d %d %d", &x, &y, &w, &h) != 4 ||
        w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        const char* err = "P6\n0 0\n255\n";
        send(c, err, (int)strlen(err), 0);
        return *keep;
    }
    unsigned char* bgra = grab_rect(x, y, w, h);
    if (bgra == NULL) {
        const char* err = "P6\n0 0\n255\n";
        send(c, err, (int)strlen(err), 0);
        return *keep;
    }
    if (packed) {
        /* PackBits over whole pixels. A page of text is mostly one color, so
         * this is worth 20 to 50 times on a viewport and turns the wire into
         * the cheap part of a sweep again. The host reverses it; the length
         * prefix lets it read exactly one answer. */
        const size_t pixels_n = (size_t)w * (size_t)h;
        unsigned char* packbuf = (unsigned char*)malloc(pixels_n * 4 + 64);
        size_t out_n = 0;
        size_t i = 0;
        while (i < pixels_n) {
            const unsigned char* px = bgra + i * 4;
            size_t run = 1;
            while (i + run < pixels_n && run < 128 &&
                   memcmp(bgra + (i + run) * 4, px, 3) == 0) {
                ++run;
            }
            if (run > 1) {
                packbuf[out_n++] = (unsigned char)(257 - run);
                packbuf[out_n++] = px[2];
                packbuf[out_n++] = px[1];
                packbuf[out_n++] = px[0];
                i += run;
                continue;
            }
            size_t lit = 1;
            while (i + lit < pixels_n && lit < 128 &&
                   memcmp(bgra + (i + lit) * 4, bgra + (i + lit - 1) * 4, 3) != 0) {
                ++lit;
            }
            packbuf[out_n++] = (unsigned char)(lit - 1);
            for (size_t k = 0; k < lit; ++k) {
                const unsigned char* q = bgra + (i + k) * 4;
                packbuf[out_n++] = q[2];
                packbuf[out_n++] = q[1];
                packbuf[out_n++] = q[0];
            }
            i += lit;
        }
        char ph[80];
        const int phn = snprintf(ph, sizeof(ph), "PK\n%d %d %llu\n", w, h,
                                 (unsigned long long)out_n);
        send(c, ph, phn, 0);
        size_t sent = 0;
        while (sent < out_n) {
            const size_t want = (out_n - sent) > 65536 ? 65536 : (out_n - sent);
            const int n = send(c, (const char*)packbuf + sent, (int)want, 0);
            if (n <= 0) { break; }
            sent += (size_t)n;
        }
        free(packbuf);
        free(bgra);
        return *keep;
    }
    char head[64];
    const int hn = snprintf(head, sizeof(head), "P6\n%d %d\n255\n", w, h);
    send(c, head, hn, 0);
    const size_t pixels = (size_t)w * (size_t)h;
    unsigned char* rgb = (unsigned char*)malloc(pixels * 3);
    if (rgb != NULL) {
        for (size_t i = 0; i < pixels; ++i) {
            rgb[i * 3 + 0] = bgra[i * 4 + 2];
            rgb[i * 3 + 1] = bgra[i * 4 + 1];
            rgb[i * 3 + 2] = bgra[i * 4 + 0];
        }
        size_t sent = 0;
        while (sent < pixels * 3) {
            const int n = send(c, (const char*)rgb + sent,
                               (int)((pixels * 3 - sent) > 65536 ? 65536 : (pixels * 3 - sent)), 0);
            if (n <= 0) { break; }
            sent += (size_t)n;
        }
        free(rgb);
    }
    free(bgra);
    return *keep;
}

static int serve(int port)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return 1;
    }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET) {
        printf("socket failed\n");
        return 1;
    }
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((unsigned short)port);
    if (bind(listener, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(listener, 4) != 0) {
        printf("bind/listen on %d failed: %d\n", port, WSAGetLastError());
        return 1;
    }
    printf("serving captures on %d\n", port);
    fflush(stdout);

    /* A kept connection stays open for a whole sweep and would hold the
     * server against everyone else: compare_pages.sh runs a sweeper per live
     * browser port against one capture server, and wincap_size.py polls the
     * same one. select() over the listener and the open connections keeps all
     * of them served from a single thread, which is what the capture wants
     * anyway, since GDI is doing one window at a time regardless. */
    SOCKET conns[FD_SETSIZE];
    int keeps[FD_SETSIZE];
    int nconn = 0;

    for (;;) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        for (int i = 0; i < nconn; ++i) {
            FD_SET(conns[i], &readable);
        }
        if (select(0, &readable, NULL, NULL, NULL) <= 0) {
            continue;
        }

        if (FD_ISSET(listener, &readable) && nconn < FD_SETSIZE - 1) {
            SOCKET c = accept(listener, NULL, NULL);
            if (c != INVALID_SOCKET) {
                /* The header and the payload go out as separate sends, so
                 * Nagle would hold the header back until the payload's ACK
                 * came in. */
                BOOL nodelay = TRUE;
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay,
                           sizeof(nodelay));
                conns[nconn] = c;
                keeps[nconn] = 0;
                ++nconn;
            }
        }

        for (int i = 0; i < nconn; ) {
            if (!FD_ISSET(conns[i], &readable)) {
                ++i;
                continue;
            }
            if (serve_one(conns[i], &keeps[i])) {
                ++i;
                continue;
            }
            closesocket(conns[i]);
            conns[i] = conns[nconn - 1];
            keeps[i] = keeps[nconn - 1];
            --nconn;
        }
    }
}

int main(int argc, char** argv)
{
    const char* out = NULL;
    int list = 0;
    int screen_mode = 0;
    int serve_port = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--class") == 0 && i + 1 < argc) { g_class = argv[++i]; }
        else if (strcmp(argv[i], "--title") == 0 && i + 1 < argc) { g_title = argv[++i]; }
        else if (strcmp(argv[i], "--list") == 0) { list = 1; }
        else if (strcmp(argv[i], "--screen") == 0) { screen_mode = 1; }
        else if (strcmp(argv[i], "--serve") == 0 && i + 1 < argc) { serve_port = atoi(argv[++i]); }
        else if (out == NULL) { out = argv[i]; }
    }
    if (serve_port > 0) {
        return serve(serve_port);
    }

    if (out == NULL && !list) {
        printf("usage: wincap <out.ppm|-> [--class C] [--title T]"
               " [--screen] [--list]\n");
        return 2;
    }

    if (list) {
        EnumWindows((WNDENUMPROC)(void*)+[](HWND h, LPARAM) -> BOOL {
            char c[256] = {0}, t[512] = {0};
            RECT r;
            GetClassNameA(h, c, sizeof(c) - 1);
            GetWindowTextA(h, t, sizeof(t) - 1);
            GetWindowRect(h, &r);
            if (r.right - r.left > 100 && r.bottom - r.top > 100) {
                printf("%p %-32s %4ldx%-4ld %s\n", (void*)h, c,
                       r.right - r.left, r.bottom - r.top, t);
            }
            return TRUE;
        }, 0);
        return 0;
    }

    int w = 0, h = 0;
    RECT r;
    if (screen_mode) {
        w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (w <= 0 || h <= 0) {
            printf("no virtual screen: %d x %d\n", w, h);
            return 1;
        }
    } else {
        EnumWindows(pick, 0);
        if (g_found == NULL) {
            printf("no window matched\n");
            return 1;
        }
        GetWindowRect(g_found, &r);
        w = (int)(r.right - r.left);
        h = (int)(r.bottom - r.top);
    }

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;          /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    HGDIOBJ old = SelectObject(mem, dib);

    BOOL ok;
    if (screen_mode) {
        /* The desktop as composited, which is what QEMU's screendump reads off
         * the emulated framebuffer. Only meaningful on a desktop this thread
         * is attached to. */
        ok = BitBlt(mem, 0, 0, w, h, screen, GetSystemMetrics(SM_XVIRTUALSCREEN),
                    GetSystemMetrics(SM_YVIRTUALSCREEN), SRCCOPY);
    } else {
        /* 2 is PW_RENDERFULLCONTENT, which mingw's headers do not always carry. */
        ok = PrintWindow(g_found, mem, 2);
        if (!ok) {
            ok = PrintWindow(g_found, mem, 0);
        }
    }

    /* Is this a real paint? Text rasterized with ClearType leaves per-channel
     * differences on most of its edge pixels; a window that never rendered, or
     * one re-drawn onto an alpha surface, does not. Reported here so the
     * question can be answered without moving the image off the guest. */
    {
        const unsigned char* q = (const unsigned char*)bits;
        long ink = 0, fringed = 0;
        for (int i = 0; i < w * h; ++i) {
            const int b = q[i * 4 + 0], g = q[i * 4 + 1], rr = q[i * 4 + 2];
            if ((b + g + rr) / 3 < 200) {
                ++ink;
                if (abs(rr - g) > 2 || abs(g - b) > 2) {
                    ++fringed;
                }
            }
        }
        printf("ink %ld fringed %ld %.2f%%\n", ink, fringed,
               ink ? 100.0 * (double)fringed / (double)ink : 0.0);
    }

    if (strcmp(out, "-") == 0) {
        printf("stats only, no file written\n");
        return ok ? 0 : 1;
    }
    FILE* f = fopen(out, "wb");
    if (f == NULL) {
        printf("cannot write %s\n", out);
        return 1;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    const unsigned char* p = (const unsigned char*)bits;
    for (int i = 0; i < w * h; ++i) {
        fputc(p[i * 4 + 2], f);      /* BGRA to RGB */
        fputc(p[i * 4 + 1], f);
        fputc(p[i * 4 + 0], f);
    }
    fclose(f);
    printf("wrote %s %dx%d (%s %s)\n", out, w, h,
           screen_mode ? "BitBlt" : "PrintWindow", ok ? "ok" : "failed");
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    return ok ? 0 : 1;
}
