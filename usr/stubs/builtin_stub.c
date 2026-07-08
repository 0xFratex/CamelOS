/**
 * usr/stubs/builtin_stub.c
 *
 * Tiny CDL module written into /Applications/<App>.app/Contents/MacOS/<name>
 * for every built-in CamelOS app.
 *
 * WHY THIS EXISTS
 * ---------------
 * The user wants macOS-style app bundles where the bundle directory
 * contains real, browsable files — including a real executable in
 * Contents/MacOS/. Without this stub, Contents/MacOS/ would be empty,
 * and the launcher would dispatch by app-name string fallback only.
 *
 * With this stub in place, Contents/MacOS/<name> is a real ELF shared
 * object that users can see (and hex-dump) via Files.app. It's compiled
 * once at build time and its bytes are embedded into the kernel as a
 * C array; app_bootstrap_init() writes those bytes into every built-in
 * app's Contents/MacOS/ directory.
 *
 * EXECUTION MODEL
 * ---------------
 * For built-in apps, the launcher (cdl_loader.c::resolve_and_load)
 * normally bypasses this stub entirely: it reads the bundle's Info.plist,
 * sees CamelBuiltin=true, and dispatches directly to the in-kernel
 * init_<app>_app() function. So in normal use this stub never runs.
 *
 * However, if a user manually opens Contents/MacOS/<name> from Files.app
 * (e.g. to inspect it), the CDL loader will load and execute it. In that
 * case, cdl_main reads the launch args (which the launcher sets to the
 * bundle name before loading) and calls sys->exec_with_args() with a
 * "/builtin/<name>" sentinel path — which resolve_and_load recognizes
 * and dispatches to kernel_launch_builtin_app(name).
 *
 * If launch args are empty (e.g. the stub was loaded some other way),
 * it just prints a friendly "this is a built-in app bundle" message
 * and returns. This keeps the stub useful as a marker file without
 * requiring elaborate state.
 *
 * BUILD
 * -----
 * Compiled with the CDL flags (-fPIC, -shared, entry=cdl_main) using
 * linker_cdl.ld. Output is builtin_stub.cdl, which is then converted
 * to a C array (builtin_stub_blob.c) by a Python script invoked from
 * the Makefile.
 */

#include "../../sys/cdl_defs.h"

/* The launcher sets launch args to the bundle's display name (e.g. "Browser")
 * before loading this stub. We read them and dispatch via /builtin/<name>. */
cdl_exports_t* cdl_main(kernel_api_t* sys) {
    if (!sys) return 0;

    char app_name[128];
    app_name[0] = 0;
    sys->get_launch_args(app_name, sizeof(app_name) - 1);

    if (app_name[0] != 0) {
        /* Dispatch via the /builtin/ sentinel path. resolve_and_load
         * recognises this prefix and calls kernel_launch_builtin_app. */
        char dispatch_path[160];
        /* Build "/builtin/<name>" manually (sys->sprintf is available). */
        sys->strcpy(dispatch_path, "/builtin/");
        int n = 0;
        while (app_name[n] && dispatch_path[n + 9] != 0 /* room check */) {
            dispatch_path[9 + n] = app_name[n];
            n++;
            if (9 + n >= (int)sizeof(dispatch_path) - 1) break;
        }
        dispatch_path[9 + n] = 0;

        sys->print("[stub] dispatching to built-in: ");
        sys->print(dispatch_path);
        sys->print("\n");

        sys->exec(dispatch_path);
        return 0;
    }

    /* No launch args — stub was loaded standalone. Just identify ourselves. */
    sys->print("[stub] CamelOS built-in app bundle\n");
    sys->print("[stub] This file is a placeholder executable. The real\n");
    sys->print("[stub] implementation is compiled into the kernel and is\n");
    sys->print("[stub] launched automatically when you open the bundle.\n");
    return 0;
}
