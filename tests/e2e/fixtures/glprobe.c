// Copied from xim-pkgindex .agents/tools/graphics/glprobe.c (the graphics stack's probe),
// for the luban-desktop scenario (tests/e2e/rootfs_instance_test.sh step 9).
// glprobe — the S1..S4 assertions from the graphics-stack design, as one binary.
//
// Renders through EGL's surfaceless platform (no X server, no Wayland, no
// window) and reads the framebuffer back. That last part is the point:
// eglInitialize succeeding and a renderer string printing are both things a
// half-working stack does. Only a pixel that came back the colour we asked for
// proves something rendered.
//
// Prints one KEY=VALUE per line so the harness can assert on them:
//   GL_VENDOR / GL_RENDERER / GL_VERSION
//   PIXEL=RRGGBB
//   RESULT=ok|fail:<why>
//   LOADED=<abs path>            one per GL/vendor object actually mapped
//
// LOADED is not decoration. "GL_RENDERER=NVIDIA GeForce RTX 4080" is printed
// just as happily by the HOST's stack running inside a subos that overrode
// nothing -- the host binary under the host loader gives exactly that line.
// Only the path each object was mapped from can tell "our payload drove this"
// apart from "we measured the host". Measured 2026-08-06: a `glxinfo` run
// inside a subos printed the NVIDIA renderer while every single object came
// from /usr/lib.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <stdio.h>
#include <string.h>

// Every GL/vendor object mapped into this process, by the path it came from.
static void dump_loaded(void) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) { printf("LOADED=<maps unreadable>\n"); return; }
    char line[4096], seen[64][512];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = strchr(line, '/');
        if (!p) continue;
        p[strcspn(p, "\n")] = 0;
        const char *base = strrchr(p, '/');
        base = base ? base + 1 : p;
        if (!(strstr(base, "_nvidia.so") || strstr(base, "libnvidia-")
              || strstr(base, "_mesa.so")  || strstr(base, "libgallium")
              || strstr(base, "libGLX.so") || strstr(base, "libEGL.so")
              || strstr(base, "libGLdispatch") || strstr(base, "libGL.so")))
            continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp(seen[i], p)) { dup = 1; break; }
        if (dup || n >= 64) continue;
        snprintf(seen[n++], sizeof seen[0], "%s", p);
    }
    fclose(f);
    for (int i = 0; i < n; i++) printf("LOADED=%s\n", seen[i]);
}

#define W 64
#define H 64

static int fail(const char *why) {
    dump_loaded();
    printf("RESULT=fail:%s\n", why);
    return 1;
}

int main(void) {
    // Surfaceless rather than EGL_DEFAULT_DISPLAY: the whole point is to run
    // with no display server reachable, which is what an empty host looks like.
    EGLDisplay dpy = EGL_NO_DISPLAY;

    // Three ways to reach a display, reported separately. Collapsing them into
    // one "no-display" hides which layer refused: a missing client extension
    // (the loader never advertised surfaceless), a vendor that declined the
    // platform, and no vendor at all are three different bugs, and the first
    // two are indistinguishable from "the stack is not there" without this.
    const char *clientExts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    printf("EGL_CLIENT_EXTENSIONS=%s\n", clientExts ? clientExts : "(none)");

    PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplay =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (getPlatformDisplay) {
        dpy = getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
        if (dpy == EGL_NO_DISPLAY)
            printf("EGL_NOTE=surfaceless refused, egl error 0x%x\n", eglGetError());
    } else {
        printf("EGL_NOTE=no eglGetPlatformDisplayEXT\n");
    }
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (dpy == EGL_NO_DISPLAY)
            printf("EGL_NOTE=default display refused, egl error 0x%x\n", eglGetError());
    }
    if (dpy == EGL_NO_DISPLAY) return fail("no-display");

    EGLint major = 0, minor = 0;
    if (!eglInitialize(dpy, &major, &minor)) return fail("eglInitialize");
    printf("EGL_VERSION=%d.%d\n", major, minor);

    if (!eglBindAPI(EGL_OPENGL_API)) return fail("eglBindAPI");

    const EGLint cfgAttr[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(dpy, cfgAttr, &cfg, 1, &n) || n < 1)
        return fail("eglChooseConfig");

    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
    if (ctx == EGL_NO_CONTEXT) return fail("eglCreateContext");

    const EGLint pbAttr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pbAttr);
    if (surf == EGL_NO_SURFACE) return fail("eglCreatePbufferSurface");

    if (!eglMakeCurrent(dpy, surf, surf, ctx)) return fail("eglMakeCurrent");

    const char *vendor   = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version  = (const char *)glGetString(GL_VERSION);
    printf("GL_VENDOR=%s\n",   vendor   ? vendor   : "?");
    printf("GL_RENDERER=%s\n", renderer ? renderer : "?");
    printf("GL_VERSION=%s\n",  version  ? version  : "?");

    // A colour no default clear would produce by accident.
    glClearColor(0.2f, 0.4f, 0.6f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();

    unsigned char px[4] = {0, 0, 0, 0};
    glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("PIXEL=%02X%02X%02X\n", px[0], px[1], px[2]);

    // 0.2/0.4/0.6 → 51/102/153, ±2 for the rounding a driver is entitled to.
    if (px[0] < 49 || px[0] > 53 || px[1] < 100 || px[1] > 104 ||
        px[2] < 151 || px[2] > 155) {
        return fail("pixel-mismatch");
    }
    dump_loaded();
    printf("RESULT=ok\n");
    return 0;
}
