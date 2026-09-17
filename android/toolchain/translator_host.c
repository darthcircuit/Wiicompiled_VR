/*
 * Starts the WiiCompiled translator, a self-contained .NET app, on the headset.
 *
 * Android refuses to exec files in an app's private storage, so the on-device builder starts its
 * tools through /system/bin/linker64. Under the linker, the .NET apphost reads its own location from
 * /proc/self/exe, gets the linker's, and cannot find the app. This host is told the app's directory
 * and name instead and hands them to hostfxr exactly as the apphost would:
 *
 *   linker64 translator_host <app directory> <app name> [app arguments...]
 *
 * It also switches off bionic's heap pointer tagging first. The .NET runtime for Android (Mono)
 * crashes at startup on tagged heap pointers.
 */
#include <dlfcn.h>
#include <limits.h>
#include <malloc.h>
#include <stdio.h>

#ifndef M_BIONIC_SET_HEAP_TAGGING_LEVEL
#define M_BIONIC_SET_HEAP_TAGGING_LEVEL (-204)
#endif
#ifndef M_HEAP_TAGGING_LEVEL_NONE
#define M_HEAP_TAGGING_LEVEL_NONE 0
#endif

typedef int (*hostfxr_main_startupinfo_fn)(int argc, const char **argv, const char *host_path,
                                           const char *dotnet_root, const char *app_path);

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: translator_host <app directory> <app name> [arguments...]\n");
        return 2;
    }
    /* Only lowering the level is allowed, and bionic accepts pointers tagged before this call. */
    mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);

    const char *directory = argv[1];
    const char *name = argv[2];
    char hostfxr[PATH_MAX], apphost[PATH_MAX], assembly[PATH_MAX];
    if (snprintf(hostfxr, sizeof hostfxr, "%s/libhostfxr.so", directory) >= (int)sizeof hostfxr ||
        snprintf(apphost, sizeof apphost, "%s/%s", directory, name) >= (int)sizeof apphost ||
        snprintf(assembly, sizeof assembly, "%s/%s.dll", directory, name) >= (int)sizeof assembly) {
        fprintf(stderr, "translator_host: path too long\n");
        return 2;
    }

    void *library = dlopen(hostfxr, RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        fprintf(stderr, "translator_host: %s\n", dlerror());
        return 3;
    }
    hostfxr_main_startupinfo_fn start = (hostfxr_main_startupinfo_fn)dlsym(library, "hostfxr_main_startupinfo");
    if (start == NULL) {
        fprintf(stderr, "translator_host: %s\n", dlerror());
        return 3;
    }

    /* The app sees the apphost path as argv[0], then its own arguments. */
    argv[2] = apphost;
    return start(argc - 2, (const char **)argv + 2, apphost, directory, assembly);
}
