/*
 * BENCsnip - where a Windows OpenGL context goes to wait
 *
 * Not part of the program. This exists because a machine was found where
 * BENCsnip took ten and a half seconds to open a window, all of it inside one
 * call - glfwCreateWindow - which is raylib's, inside GLFW's, and therefore
 * not somewhere a timestamp can be put.
 *
 * So this does the same job by hand, in about a hundred lines of Win32, with
 * a clock on every step: register a class, make a window, choose a pixel
 * format, create a GL context, make it current, ask who is drawing. Whichever
 * line has the ten seconds after it is the answer, and it is no longer
 * possible to blame a library for it.
 *
 * Ten seconds that does not vary between runs is a timeout rather than work,
 * and the candidates are all things that happen the first time a process
 * loads the graphics driver: a scanner reading a hundred megabytes of
 * nvoglv64.dll on behalf of an untrusted executable, a certificate
 * revocation check that cannot reach the internet, a driver service being
 * waited on. This says which call is holding, which narrows that list to one.
 *
 *   cc -O2 tools/glprobe.c -o glprobe.exe -lopengl32 -lgdi32 -luser32
 */

#ifndef _WIN32
#include <stdio.h>
int main(void)
{
    printf("glprobe is for Windows. Everywhere else, this problem has not"
           " turned up.\n");
    return 0;
}
#else

#include <windows.h>

#include <GL/gl.h>
#include <stdio.h>

static LARGE_INTEGER g_freq, g_start;

static void clock_start(void)
{
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
}

static double ms_now(void)
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (double)(n.QuadPart - g_start.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

static double g_last = 0.0;

static void step(const char *what)
{
    const double at = ms_now();
    printf("  %9.1f ms  %+9.1f  %s\n", at, at - g_last, what);
    fflush(stdout);
    g_last = at;
}

int main(void)
{
    clock_start();
    printf("\nglprobe: each Win32 and WGL call on the way to a GL context.\n");
    printf("  %9s     %9s  %s\n", "at", "took", "step");
    step("started");

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "BENCsnipGLProbe";
    wc.style = CS_OWNDC;
    if (!RegisterClassA(&wc)) { printf("  RegisterClass failed\n"); return 1; }
    step("RegisterClass");

    HWND wnd = CreateWindowExA(0, wc.lpszClassName, "glprobe", WS_OVERLAPPEDWINDOW,
                               CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, NULL, NULL,
                               wc.hInstance, NULL);
    if (!wnd) { printf("  CreateWindowEx failed\n"); return 1; }
    step("CreateWindowEx");

    HDC dc = GetDC(wnd);
    step("GetDC");

    PIXELFORMATDESCRIPTOR pfd;
    ZeroMemory(&pfd, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;

    const int pf = ChoosePixelFormat(dc, &pfd);
    step("ChoosePixelFormat");
    if (!pf) { printf("  no pixel format\n"); return 1; }

    if (!SetPixelFormat(dc, pf, &pfd)) { printf("  SetPixelFormat failed\n"); return 1; }
    step("SetPixelFormat");

    /* The one that loads the driver's OpenGL implementation. If the ten
     * seconds is anywhere, the money is here. */
    HGLRC rc = wglCreateContext(dc);
    step("wglCreateContext");
    if (!rc) { printf("  no context\n"); return 1; }

    if (!wglMakeCurrent(dc, rc)) { printf("  wglMakeCurrent failed\n"); return 1; }
    step("wglMakeCurrent");

    const char *vendor = (const char *)glGetString(GL_VENDOR);
    const char *renderer = (const char *)glGetString(GL_RENDERER);
    const char *version = (const char *)glGetString(GL_VERSION);
    step("glGetString");

    /* ---------------------------------------------------------------- *
     * The rest: what GLFW does that the plain path above does not.
     *
     * Everything so far is the old GDI way of getting a context, and on the
     * machine this was written for it takes a fifth of a second. GLFW asks
     * differently - it uses the WGL extensions, and to choose a format it
     * reads the attributes of every format the driver offers, one call each.
     * That loop is the thing to time, because a driver with several hundred
     * formats and a slow answer for each turns into ten seconds without
     * anything having gone wrong in a way an error could report.
     * ---------------------------------------------------------------- */

    typedef BOOL(WINAPI * PFN_GETATTRIBIV)(HDC, int, int, UINT, const int *, int *);
    typedef BOOL(WINAPI * PFN_CHOOSEFMT)(HDC, const int *, const FLOAT *, UINT, int *,
                                         UINT *);
    typedef HGLRC(WINAPI * PFN_CREATECTX)(HDC, HGLRC, const int *);

    PFN_GETATTRIBIV getAttribiv =
        (PFN_GETATTRIBIV)(void *)wglGetProcAddress("wglGetPixelFormatAttribivARB");
    PFN_CHOOSEFMT chooseARB =
        (PFN_CHOOSEFMT)(void *)wglGetProcAddress("wglChoosePixelFormatARB");
    PFN_CREATECTX createCtx =
        (PFN_CREATECTX)(void *)wglGetProcAddress("wglCreateContextAttribsARB");
    step("load the WGL extension entry points");

    printf("\n  the WGL path, which is the one GLFW takes:\n");
    printf("    wglGetPixelFormatAttribivARB  %s\n", getAttribiv ? "present" : "MISSING");
    printf("    wglChoosePixelFormatARB       %s\n", chooseARB ? "present" : "MISSING");
    printf("    wglCreateContextAttribsARB    %s\n", createCtx ? "present" : "MISSING");
    printf("  %9s     %9s  %s\n", "at", "took", "step");

    if (!getAttribiv) {
        printf("  no wglGetPixelFormatAttribivARB; nothing more to measure\n");
    } else {
        enum { WGL_NUMBER_PIXEL_FORMATS_ARB = 0x2000 };
        int nFormats = 0;
        const int countAttrib = WGL_NUMBER_PIXEL_FORMATS_ARB;
        getAttribiv(dc, 1, 0, 1, &countAttrib, &nFormats);
        step("ask how many pixel formats there are");
        printf("               %d formats offered\n", nFormats);

        /* The same attributes GLFW asks for, in the same shape: one call per
         * format, every attribute at once. */
        static const int attribs[] = {
            0x2001, /* DRAW_TO_WINDOW */ 0x2010, /* SUPPORT_OPENGL   */
            0x2011, /* DOUBLE_BUFFER  */ 0x2003, /* ACCELERATION     */
            0x2013, /* PIXEL_TYPE     */ 0x2015, /* RED_BITS         */
            0x2017, /* GREEN_BITS     */ 0x2019, /* BLUE_BITS        */
            0x201B, /* ALPHA_BITS     */ 0x2022, /* DEPTH_BITS       */
            0x2023, /* STENCIL_BITS   */ 0x2042, /* SAMPLES          */
        };
        const UINT nAttribs = (UINT)(sizeof attribs / sizeof attribs[0]);
        int values[16];

        const double before = ms_now();
        for (int i = 1; i <= nFormats; i++) getAttribiv(dc, i, 0, nAttribs, attribs, values);
        const double after = ms_now();
        g_last = before;
        step("read every format's attributes, one call each");

        if (nFormats > 0)
            printf("               %.2f ms per format, %d of them\n",
                   (after - before) / nFormats, nFormats);
    }

    /* The other half of the same loop. GLFW uses the extension when the
     * driver offers it and falls back to this when it does not, and both are
     * one call per format - so whichever path is taken on a given machine,
     * the cost is here. Timed as well as the other, so this does not need
     * running twice to find out which one mattered. */
    {
        const int nLegacy = DescribePixelFormat(dc, 1, sizeof(PIXELFORMATDESCRIPTOR), NULL);
        PIXELFORMATDESCRIPTOR d;
        const double before = ms_now();
        for (int i = 1; i <= nLegacy; i++)
            DescribePixelFormat(dc, i, sizeof(PIXELFORMATDESCRIPTOR), &d);
        const double after = ms_now();
        g_last = before;
        step("DescribePixelFormat for every format, the older way");
        if (nLegacy > 0)
            printf("               %.2f ms per format, %d of them\n",
                   (after - before) / nLegacy, nLegacy);
    }

    if (chooseARB) {
        int fmt = 0;
        UINT n = 0;
        static const int want[] = {0x2001, 1, 0x2010, 1, 0x2011, 1,
                                   0x2013, 0x202B, 0x2015, 8, 0x2017, 8,
                                   0x2019, 8, 0x2022, 24, 0};
        chooseARB(dc, want, NULL, 1, &fmt, &n);
        step("wglChoosePixelFormatARB, which asks the driver to choose");
    }

    if (createCtx) {
        /* A 3.3 core context, which is what raylib asks for and what the
         * plain path above does not: the context it got was 4.6
         * compatibility, chosen by the driver. */
        static const int attr[] = {0x2091, 3, 0x2092, 3, 0x9126, 0x00000001, 0};
        HGLRC rc2 = createCtx(dc, NULL, attr);
        step("wglCreateContextAttribsARB for 3.3 core");
        if (rc2) wglDeleteContext(rc2);
        else printf("               (the driver refused it)\n");
    }

    printf("\n  vendor:   %s\n", vendor ? vendor : "?");
    printf("  renderer: %s\n", renderer ? renderer : "?");
    printf("  version:  %s\n\n", version ? version : "?");

    printf("  Whichever line above holds the missing seconds is the answer.\n"
           "  If it is the per-format loop, the driver is slow to describe\n"
           "  each of its pixel formats and GLFW reads all of them; that is\n"
           "  work rather than a fault, and it is the same every run.\n\n");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    return 0;
}

#endif /* _WIN32 */
