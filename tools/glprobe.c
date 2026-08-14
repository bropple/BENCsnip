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

    printf("\n  vendor:   %s\n", vendor ? vendor : "?");
    printf("  renderer: %s\n", renderer ? renderer : "?");
    printf("  version:  %s\n\n", version ? version : "?");

    printf("  A step that took ten seconds on its own is a timeout, not work.\n"
           "  wglCreateContext is where the graphics driver is first loaded\n"
           "  into this process; CreateWindowEx is where anything hooking\n"
           "  windows would be.\n\n");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    return 0;
}

#endif /* _WIN32 */
