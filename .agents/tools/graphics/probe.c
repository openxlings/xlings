/* Who actually renders — measured by rendering, not by inspecting files.
 *
 * WHY THIS IS NOT `glxinfo`
 *
 * `glxinfo` answers "what does GL report" for ONE path (GLX on $DISPLAY) and
 * cannot answer the question this stack keeps getting wrong: each of glvnd's
 * entry points is its own load-chain root, and they fail INDEPENDENTLY. On the
 * host this was written against, GLX rendered on an RTX 4080 while EGL, GLESv1
 * and GLESv2 could not load the same vendor at all and silently fell back to
 * zink. A tool that covers one entry point calls that stack healthy.
 *
 * So this program probes each context type separately and reports what the
 * driver behind it says about itself. A cell that cannot even reach a context
 * is reported as such; "no answer" and "software answered" are different
 * results and must never print the same.
 *
 * WHY EVERYTHING IS dlopen'd
 *
 * One binary has to run in cells where libGL, libEGL, libX11 or libvulkan are
 * variously absent. Linking them would turn "this environment has no EGL" —
 * a legitimate matrix result — into a build failure that takes the whole run
 * down with it. dlopen makes absence a data point.
 *
 * Output: one line per probe, `api|vendor|renderer` on success or
 * `api|ERROR|<what failed>` otherwise. Parsed by matrix.sh.
 *
 * Build: cc -O0 -o probe probe.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

typedef unsigned int GLenum;
typedef const unsigned char* (*PFN_glGetString)(GLenum);
#define GL_VENDOR   0x1F00
#define GL_RENDERER 0x1F01

/* dlopen a library the way a SHIPPED program reaches it.
 *
 * Bare-name dlopen is wrong in a subos: nothing puts the subos's lib
 * directory on the loader's default search path, so every probe would come
 * back "libEGL.so.1 not loadable" in an environment whose libEGL works
 * perfectly — an artifact of the probe printed in the same column as a real
 * failure. An installed GL program does not hit this because elfpatch writes
 * it an RPATH at package build time.
 *
 * `GFX_PROBE_LIBDIR` supplies that directory. It is NOT LD_LIBRARY_PATH:
 * that variable would also be inherited by everything the driver itself
 * loads, quietly changing the resolution under test. This affects only the
 * four top-level libraries this program opens by hand.
 */
static void* xdlopen(const char* soname) {
    const char* dir = getenv("GFX_PROBE_LIBDIR");
    if (dir && *dir) {
        char buf[4096];
        snprintf(buf, sizeof buf, "%s/%s", dir, soname);
        void* h = dlopen(buf, RTLD_NOW | RTLD_GLOBAL);
        if (h) return h;
    }
    return dlopen(soname, RTLD_NOW | RTLD_GLOBAL);
}

static void emit(const char* api, const char* vendor, const char* renderer) {
    printf("%s|%s|%s\n", api, vendor ? vendor : "?", renderer ? renderer : "?");
    fflush(stdout);
}
static void emit_err(const char* api, const char* what) {
    printf("%s|ERROR|%s\n", api, what);
    fflush(stdout);
}

/* glGetString comes from whichever dispatch is present. Under glvnd the
 * dispatch is shared between desktop GL and GLES, so libGL answers for both;
 * a GLES-only environment has no libGL at all, hence the fallbacks. */
static PFN_glGetString find_glGetString(void) {
    static const char* libs[] = {
        "libGL.so.1", "libOpenGL.so.0", "libGLESv2.so.2", "libGLESv1_CM.so.1", NULL
    };
    for (int i = 0; libs[i]; ++i) {
        void* h = xdlopen(libs[i]);
        if (!h) continue;
        PFN_glGetString f = (PFN_glGetString)dlsym(h, "glGetString");
        if (f) return f;
    }
    return NULL;
}

/* ── GLX ──────────────────────────────────────────────────────────────────
 *
 * A pbuffer, not a window: this must work with no window manager and no
 * compositor, because those are exactly the environments (CI, a container, a
 * headless box) where a graphics stack quietly regresses. */
static int  x_error_code = 0;
static int  x_error_opcode = 0;
static int x_error_handler(void* dpy, void* ev) {
    /* XErrorEvent layout: int type; Display*; unsigned long serial;
     * unsigned char error_code, request_code, minor_code; ... */
    unsigned char* e = (unsigned char*)ev;
    const size_t off = sizeof(int) + sizeof(void*) + sizeof(unsigned long);
    x_error_code   = e[off];
    x_error_opcode = e[off + 1];
    (void)dpy;
    return 0;
}

static void probe_glx(void) {
    void* x11 = xdlopen("libX11.so.6");
    if (!x11) { emit_err("glx", "libX11.so.6 not loadable"); return; }
    void* gl = xdlopen("libGLX.so.0");
    if (!gl) gl = xdlopen("libGL.so.1");
    if (!gl) { emit_err("glx", "no libGLX.so.0 / libGL.so.1"); return; }

    /* Xlib's DEFAULT error handler prints to stderr and calls exit().
     *
     * Measured in the sandbox: GLX context creation returns a BadValue X
     * protocol error, Xlib exits, and this probe produces NOTHING — the cell
     * came back "(no output)", which is a broken measurement, not a result.
     * A real, nameable failure was being hidden by the measuring tool. So the
     * handler is replaced with one that records and returns; the probe then
     * reports the protocol error as the cell's answer. */
    int (*XSetErrorHandler)(void*) = dlsym(x11, "XSetErrorHandler");
    if (XSetErrorHandler) XSetErrorHandler((void*)x_error_handler);

    void* (*XOpenDisplay)(const char*) = dlsym(x11, "XOpenDisplay");
    int   (*XDefaultScreen)(void*)     = dlsym(x11, "XDefaultScreen");
    void* (*ChooseFBConfig)(void*, int, const int*, int*) = dlsym(gl, "glXChooseFBConfig");
    void* (*CreatePbuffer)(void*, void*, const int*)      = dlsym(gl, "glXCreatePbuffer");
    void* (*CreateNewContext)(void*, void*, int, void*, int) = dlsym(gl, "glXCreateNewContext");
    int   (*MakeContextCurrent)(void*, void*, void*, void*)  = dlsym(gl, "glXMakeContextCurrent");
    if (!XOpenDisplay || !ChooseFBConfig || !CreatePbuffer
        || !CreateNewContext || !MakeContextCurrent) {
        emit_err("glx", "GLX entry points missing"); return;
    }

    void* dpy = XOpenDisplay(NULL);
    if (!dpy) { emit_err("glx", "no X display (DISPLAY unset or unreachable)"); return; }

    /* GLX_DRAWABLE_TYPE=GLX_PBUFFER_BIT, GLX_RENDER_TYPE=GLX_RGBA_BIT */
    const int attrs[] = { 0x8010, 0x00000004, 0x8011, 0x00000001, 0 };
    int n = 0;
    void** cfgs = ChooseFBConfig(dpy, XDefaultScreen ? XDefaultScreen(dpy) : 0, attrs, &n);
    if (!cfgs || n <= 0) { emit_err("glx", "no pbuffer-capable FBConfig"); return; }

    const int pb[] = { 0x8041 /*WIDTH*/, 16, 0x8040 /*HEIGHT*/, 16, 0 };
    void* surf = CreatePbuffer(dpy, cfgs[0], pb);
    if (!surf || x_error_code) { emit_err("glx", "glXCreatePbuffer failed"); return; }

    void* ctx = CreateNewContext(dpy, cfgs[0], 0x8014 /*GLX_RGBA_TYPE*/, NULL, 1);
    if (!ctx || x_error_code) {
        char msg[128];
        if (x_error_code)
            snprintf(msg, sizeof msg,
                     "X protocol error %d on GLX opcode %d creating the context",
                     x_error_code, x_error_opcode);
        else
            snprintf(msg, sizeof msg, "glXCreateNewContext failed");
        emit_err("glx", msg); return;
    }
    if (!MakeContextCurrent(dpy, surf, surf, ctx) || x_error_code) {
        emit_err("glx", "glXMakeContextCurrent failed"); return;
    }

    PFN_glGetString gs = find_glGetString();
    if (!gs) { emit_err("glx", "glGetString unavailable"); return; }
    emit("glx", (const char*)gs(GL_VENDOR), (const char*)gs(GL_RENDERER));
}

/* ── EGL ──────────────────────────────────────────────────────────────────
 *
 * `platform` selects how the display is obtained, and that choice IS a matrix
 * axis: DEFAULT goes through whatever the environment provides (an X server,
 * a Wayland compositor), SURFACELESS is the headless/offline path that must
 * work with no display server at all, and DEVICE is how a headless box
 * addresses a specific GPU. They can and do disagree about who renders.
 *
 * `api` selects the client API — desktop GL vs GLESv1 vs GLESv2 — which under
 * glvnd means a DIFFERENT vendor library, and therefore a different load
 * chain that can fail on its own. */
static void probe_egl(const char* label, const char* platform, const char* api) {
    void* egl = xdlopen("libEGL.so.1");
    if (!egl) { emit_err(label, "libEGL.so.1 not loadable"); return; }

    void* (*GetDisplay)(void*) = dlsym(egl, "eglGetDisplay");
    void* (*GetProcAddress)(const char*) = dlsym(egl, "eglGetProcAddress");
    int   (*Initialize)(void*, int*, int*) = dlsym(egl, "eglInitialize");
    int   (*ChooseConfig)(void*, const int*, void**, int, int*) = dlsym(egl, "eglChooseConfig");
    int   (*BindAPI)(unsigned) = dlsym(egl, "eglBindAPI");
    void* (*CreateContext)(void*, void*, void*, const int*) = dlsym(egl, "eglCreateContext");
    int   (*MakeCurrent)(void*, void*, void*, void*) = dlsym(egl, "eglMakeCurrent");
    const char* (*QueryString)(void*, int) = dlsym(egl, "eglQueryString");
    if (!GetDisplay || !Initialize || !ChooseConfig || !CreateContext || !MakeCurrent) {
        emit_err(label, "EGL entry points missing"); return;
    }

    void* dpy = NULL;
    if (strcmp(platform, "surfaceless") == 0 || strcmp(platform, "device") == 0) {
        void* (*GetPlatformDisplay)(unsigned, void*, const int*) =
            GetProcAddress ? GetProcAddress("eglGetPlatformDisplayEXT") : NULL;
        if (!GetPlatformDisplay) {
            emit_err(label, "eglGetPlatformDisplayEXT unavailable"); return;
        }
        /* EGL_PLATFORM_SURFACELESS_MESA 0x31DD, EGL_PLATFORM_DEVICE_EXT 0x313F */
        if (strcmp(platform, "surfaceless") == 0) {
            dpy = GetPlatformDisplay(0x31DD, NULL, NULL);
        } else {
            /* Enumerate devices and take the first: on a headless box this is
             * the only route to the GPU, and "no devices" is a real answer. */
            int (*QueryDevices)(int, void**, int*) =
                GetProcAddress ? GetProcAddress("eglQueryDevicesEXT") : NULL;
            if (!QueryDevices) { emit_err(label, "eglQueryDevicesEXT unavailable"); return; }
            void* devs[8]; int ndev = 0;
            if (!QueryDevices(8, devs, &ndev) || ndev <= 0) {
                emit_err(label, "no EGL devices"); return;
            }
            dpy = GetPlatformDisplay(0x313F, devs[0], NULL);
        }
    } else {
        dpy = GetDisplay((void*)0 /* EGL_DEFAULT_DISPLAY */);
    }
    if (!dpy) { emit_err(label, "no EGL display"); return; }

    int major = 0, minor = 0;
    if (!Initialize(dpy, &major, &minor)) { emit_err(label, "eglInitialize failed"); return; }

    unsigned bind = 0x30A2;            /* EGL_OPENGL_API */
    int renderable = 0x0008;           /* EGL_OPENGL_BIT */
    int ctxattrs[8] = { 0x3038 };      /* EGL_NONE */
    if (strcmp(api, "gles2") == 0) {
        bind = 0x30A0; renderable = 0x0004;                 /* ES_API, ES2_BIT */
        ctxattrs[0] = 0x3098; ctxattrs[1] = 2; ctxattrs[2] = 0x3038;
    } else if (strcmp(api, "gles1") == 0) {
        bind = 0x30A0; renderable = 0x0001;                 /* ES_API, ES_BIT */
        ctxattrs[0] = 0x3098; ctxattrs[1] = 1; ctxattrs[2] = 0x3038;
    }
    if (BindAPI && !BindAPI(bind)) { emit_err(label, "eglBindAPI rejected"); return; }

    const int cfgattrs[] = {
        0x3040 /*EGL_RENDERABLE_TYPE*/, renderable,
        0x3033 /*EGL_SURFACE_TYPE*/,    0x0001 /*EGL_PBUFFER_BIT*/,
        0x3038
    };
    void* cfg = NULL; int ncfg = 0;
    if (!ChooseConfig(dpy, cfgattrs, &cfg, 1, &ncfg) || ncfg <= 0) {
        emit_err(label, "no EGL config for this API"); return;
    }

    void* ctx = CreateContext(dpy, cfg, NULL /*EGL_NO_CONTEXT*/, ctxattrs);
    if (!ctx) { emit_err(label, "eglCreateContext failed"); return; }
    if (!MakeCurrent(dpy, NULL, NULL, ctx)) {   /* surfaceless make-current */
        emit_err(label, "eglMakeCurrent failed"); return;
    }

    PFN_glGetString gs = find_glGetString();
    if (!gs) { emit_err(label, "glGetString unavailable"); return; }
    const char* r = (const char*)gs(GL_RENDERER);
    const char* v = (const char*)gs(GL_VENDOR);
    if (!r) {
        /* A current context that answers nothing is not a working context.
         * Reporting the EGL vendor here instead would name the dispatch, not
         * the driver — the exact substitution this program exists to avoid. */
        emit_err(label, "context current but GL_RENDERER is null"); return;
    }
    (void)QueryString;
    emit(label, v, r);
}

/* ── Vulkan ───────────────────────────────────────────────────────────────
 *
 * Not GL, but the same question with the same failure mode: an ICD that
 * cannot load leaves an enumeration that succeeds and returns nothing, and
 * "no GPU" is indistinguishable from "the loader found no ICD" unless the two
 * are reported apart. */
static void probe_vulkan(void) {
    void* vk = xdlopen("libvulkan.so.1");
    if (!vk) { emit_err("vulkan", "libvulkan.so.1 not loadable"); return; }

    typedef int (*PFN_CreateInstance)(const void*, const void*, void**);
    typedef int (*PFN_EnumPhys)(void*, unsigned*, void**);
    typedef void (*PFN_GetProps)(void*, void*);
    PFN_CreateInstance CreateInstance = dlsym(vk, "vkCreateInstance");
    PFN_EnumPhys EnumPhys = dlsym(vk, "vkEnumeratePhysicalDevices");
    PFN_GetProps GetProps = dlsym(vk, "vkGetPhysicalDeviceProperties");
    if (!CreateInstance || !EnumPhys || !GetProps) {
        emit_err("vulkan", "vulkan entry points missing"); return;
    }

    /* VkInstanceCreateInfo, laid out by hand because we do not link Vulkan.
     * `flags` sits between pNext and pApplicationInfo and is easy to omit —
     * doing so shifts every later member by one slot and the call segfaults
     * inside the loader, which reads as "Vulkan is broken here" rather than
     * "the caller lied about the struct". */
    struct { int sType; const void* pNext; unsigned flags;
             const void* pApplicationInfo;
             unsigned enabledLayerCount; const char* const* ppEnabledLayerNames;
             unsigned enabledExtensionCount; const char* const* ppEnabledExtensionNames;
    } ci = { 1 /*VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO*/, 0, 0, 0, 0, 0, 0, 0 };
    void* inst = NULL;
    if (CreateInstance(&ci, NULL, &inst) != 0 || !inst) {
        emit_err("vulkan", "vkCreateInstance failed"); return;
    }
    unsigned n = 0;
    if (EnumPhys(inst, &n, NULL) != 0) { emit_err("vulkan", "enumerate failed"); return; }
    if (n == 0) { emit_err("vulkan", "loader found no ICD / no device"); return; }
    void* devs[8]; if (n > 8) n = 8;
    EnumPhys(inst, &n, devs);

    /* VkPhysicalDeviceProperties: apiVersion, driverVersion, vendorID,
     * deviceID, deviceType, deviceName[256], ... — we only read deviceName,
     * but the driver writes the WHOLE struct (VkPhysicalDeviceLimits alone is
     * several hundred bytes), so the buffer must fit all of it. */
    unsigned char props[4096];
    memset(props, 0, sizeof props);
    GetProps(devs[0], props);
    emit("vulkan", "vulkan-icd", (const char*)(props + 5 * sizeof(unsigned)));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: probe <glx|egl|egl-surfaceless|egl-device|"
                        "gles1|gles2|gles2-surfaceless|vulkan|all>\n");
        return 2;
    }
    const char* what = argv[1];
    int all = strcmp(what, "all") == 0;

    if (all || !strcmp(what, "glx"))              probe_glx();
    if (all || !strcmp(what, "egl"))              probe_egl("egl", "default", "gl");
    if (all || !strcmp(what, "egl-surfaceless"))  probe_egl("egl-surfaceless", "surfaceless", "gl");
    if (all || !strcmp(what, "egl-device"))       probe_egl("egl-device", "device", "gl");
    if (all || !strcmp(what, "gles1"))            probe_egl("gles1", "default", "gles1");
    if (all || !strcmp(what, "gles2"))            probe_egl("gles2", "default", "gles2");
    if (all || !strcmp(what, "gles2-surfaceless")) probe_egl("gles2-surfaceless", "surfaceless", "gles2");
    if (all || !strcmp(what, "vulkan"))           probe_vulkan();
    return 0;
}
