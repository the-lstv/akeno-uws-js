#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* List of platform features */
#ifdef _WIN32
#define OS "win32"
#define IS_WINDOWS
#endif
#ifdef __linux
#define OS "linux"
#define IS_LINUX
#endif
#ifdef __APPLE__
#define OS "darwin"
#define IS_MACOS
#endif

/* ASAN vs. optimized build flags (used via C string literal concatenation).
 * OPT_FLAGS / LINK_FLAGS: inserted mid-string, so each definition starts with a space.
 * LINUX_LINK_EXTRAS: passed as a standalone argument, so no leading space.
 * MACOS_LINK_EXTRAS: appended after "-undefined dynamic_lookup", so ASAN variant starts with a space. */
#ifdef WITH_ASAN
#define OPT_FLAGS " -fsanitize=address -fno-omit-frame-pointer -g -O1"
#define LINK_FLAGS " -fsanitize=address"
#define LINUX_LINK_EXTRAS "-fsanitize=address"
#define MACOS_LINK_EXTRAS " -fsanitize=address"
#else
#define OPT_FLAGS " -flto -O3"
#define LINK_FLAGS " -flto -O3"
#define LINUX_LINK_EXTRAS "-static-libstdc++ -static-libgcc -s"
#define MACOS_LINK_EXTRAS ""
#endif

const char *ARM = "arm";
const char *ARM64 = "arm64";
const char *X64 = "x64";

int addon_only = 0;
int latest_only = 0;
int debug_mode = 0;
int disable_http3 = 0;
char *selected_version = NULL;

int buildingForElectron = 0;

int exists(const char *fname) {
    FILE *file;
    if ((file = fopen(fname, "r"))) {
        fclose(file);
        return 1;
    }
    return 0;
}

/* System, but with string replace */
int run(const char *cmd, ...) {
    char buf[2048];
    va_list args;
    va_start(args, cmd);
    vsprintf(buf, cmd, args);
    va_end(args);
    printf("--> %s\n\n", buf);
    return system(buf);
}

/* List of Node.js versions */
struct node_version {
    char *name;
    char *abi;
    char *runtime;
} versions[] = {
    {"v20.0.0", "115", "node"},
    {"v22.0.0", "127", "node"},
    {"v24.0.0", "137", "node"},
    // {"v25.0.0", "141", "node"}, // v25 is broken in the latest? Unsure why
    {"v26.0.0", "147", "node"},

    // We can also build for other runtimes, since Electron has it's own setup
    // No guarantee it will work though as most v8 APIs when used in an addon are broken on Electron.
    // {"v43.2.0", "148", "electron"},
};

/* Downloads headers, creates folders */
void prepare(const char *windows_lib_arch) {
#ifdef IS_WINDOWS
    if (run("if not exist dist mkdir dist") || run("if not exist fragments mkdir fragments") || run("if not exist targets mkdir targets") || run("if not exist targets\\node mkdir targets\\node") || run("if not exist targets\\electron mkdir targets\\electron")) {
        return;
    }
#else
    if (run("mkdir -p dist") || run("mkdir -p fragments") || run("mkdir -p targets") || run("mkdir -p targets/node") || run("mkdir -p targets/electron")) {
        return;
    }
#endif
    /* For all versions */
    int j = 0;
    for (unsigned int i = 0; i < sizeof(versions) / sizeof(struct node_version); i++) {
        if (selected_version && strcmp(versions[i].name, selected_version)) {
            continue;
        }

        if(buildingForElectron && strcmp(versions[i].runtime, "electron") != 0) {
            continue;
        } else if(!buildingForElectron && strcmp(versions[i].runtime, "node") != 0) {
            continue;
        }

        char source[256];
        if(buildingForElectron) {
            sprintf(source, "https://artifacts.electronjs.org/headers/dist/%s/node-%s-headers.tar.gz", versions[i].name, versions[i].name);
        } else {
            sprintf(source, "https://nodejs.org/dist/%s/node-%s-headers.tar.gz", versions[i].name, versions[i].name);
        }

        run("mkdir -p targets/%s/%s", versions[i].runtime, versions[i].name);

        char path[256];
        sprintf(path, "targets/%s/node-%s-headers.tar.gz", versions[i].runtime, versions[i].name);
        if (!exists(path)) {
            run("cd targets/%s && curl -OJ %s", versions[i].runtime, source);
            run("tar xzf %s --strip-components=1 -C targets/%s/%s", path, versions[i].runtime, versions[i].name);
        }


        if(!buildingForElectron) {
#ifdef IS_WINDOWS
            sprintf(path, "targets/%s/%s/node.lib", versions[i].runtime, versions[i].name);
            if (!exists(path)) {
                run("curl https://nodejs.org/dist/%s/win-%s/node.lib > targets/%s/%s/node.lib", versions[i].name, windows_lib_arch, versions[i].runtime, versions[i].name);
            }
#endif

            /* v8-fast-api-calls.h is missing from the Node.js header distribution; fetch the matching Node version */
            sprintf(path, "targets/%s/%s/include/node/v8-fast-api-calls.h", versions[i].runtime, versions[i].name);
            if (!exists(path)) {
                run("curl -fL https://raw.githubusercontent.com/nodejs/node/%s/deps/v8/include/v8-fast-api-calls.h > targets/%s/%s/include/node/v8-fast-api-calls.h", versions[i].name, versions[i].runtime, versions[i].name);
            }
        }

        j++;

        if (latest_only) {
            break;
        }
    }

    if (j == 0) {
        printf("No versions were built. Check your --version argument.\n");
    }
}

void build_lsquic(const char *arch) {
    if (disable_http3) {
        return;
    }
#ifndef IS_WINDOWS
    /* Build for arm64 and x64 for macOS */

#ifdef IS_MACOS
    if (arch == X64) {
        run("cd uWebSockets/uSockets/lsquic && mkdir -p arm64 && cd arm64 && cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -DBORINGSSL_DIR=../boringssl -DCMAKE_BUILD_TYPE=Release -DLSQUIC_BIN=Off .. && make lsquic");
    } else if (arch == ARM64) {
        run("cd uWebSockets/uSockets/lsquic && mkdir -p x64 && cd x64 && cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_OSX_ARCHITECTURES=x86_64 -DBORINGSSL_DIR=../boringssl -DCMAKE_BUILD_TYPE=Release -DLSQUIC_BIN=Off .. && make lsquic");
    }
#else
    /* Linux */
    run("cd uWebSockets/uSockets/lsquic && mkdir -p %s && cd %s && cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBORINGSSL_DIR=../boringssl -DCMAKE_BUILD_TYPE=Release -DLSQUIC_BIN=Off .. && make lsquic", arch, arch);

#endif
    
#else
    /* Windows */

    /* Download zlib */
    if (!exists("zlib-1.3.1.tar.gz")) {
        run("curl -OL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz");
    }

    run("tar xzf zlib-1.3.1.tar.gz");
    run("cd uWebSockets/uSockets/lsquic && cmake -DCMAKE_C_FLAGS=\"-DWIN32 /wd4201 -I..\\..\\..\\zlib-1.3.1\" -DZLIB_INCLUDE_DIR=..\\..\\..\\zlib-1.3.1 -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DBORINGSSL_DIR=../boringssl -DCMAKE_BUILD_TYPE=Release -DLSQUIC_BIN=Off -GNinja . && ninja");
#endif
}

/* Build boringssl */
void build_boringssl(const char *arch) {

#ifdef IS_MACOS
    /* Only macOS uses cross-compilation */
    if (arch == X64) {
        run("cd uWebSockets/uSockets/boringssl && mkdir -p x64 && cd x64 && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 .. && make crypto ssl");
    } else if (arch == ARM64) {
        run("cd uWebSockets/uSockets/boringssl && mkdir -p arm64 && cd arm64 && cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 .. && make crypto ssl");
    }
#endif
    
#ifdef IS_LINUX
    /* Build for x64 or arm/arm64 (depending on the host) */
    run("cd uWebSockets/uSockets/boringssl && mkdir -p %s && cd %s && cmake -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_BUILD_TYPE=Release .. && make crypto ssl", arch, arch);
#endif
    
#ifdef IS_WINDOWS
    /* Build for x64 (the host) */
    run("cd uWebSockets/uSockets/boringssl && if not exist x64 mkdir x64 && cd x64 && cmake -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DCMAKE_BUILD_TYPE=Release -GNinja .. && ninja crypto ssl");
#endif

}

/* Build for Unix systems */
void build(char *compiler, char *cpp_compiler, char *cpp_linker, char *os, const char *arch) {
    const char *opt_flags = debug_mode ? "-g -O0" : "-O3";
    const char *http3_flags = disable_http3 ? "-DUWS_NO_HTTP3" : "-DLIBUS_USE_QUIC";
    const char *http3_include = disable_http3 ? "" : " -I uWebSockets/uSockets/lsquic/include";

    char c_shared[1024];
    char cpp_shared[1024];
    snprintf(c_shared, sizeof(c_shared), "-DWIN32_LEAN_AND_MEAN -DLIBUS_USE_LIBUV -DUWS_USE_LIBDEFLATE %s -I uWebSockets/libdeflate -I uWebSockets/uSockets/boringssl/include -pthread -DLIBUS_USE_OPENSSL -flto %s%s -c -fPIC -I uWebSockets/uSockets/src uWebSockets/uSockets/src/*.c uWebSockets/uSockets/src/eventing/*.c uWebSockets/uSockets/src/crypto/*.c", http3_flags, opt_flags, http3_include);
    snprintf(cpp_shared, sizeof(cpp_shared), "-DWIN32_LEAN_AND_MEAN -DUWS_WITH_PROXY -DUWS_REMOTE_ADDRESS_USERSPACE -DUWS_USE_LIBDEFLATE -DLIBUS_USE_LIBUV %s -I uWebSockets/uSockets/boringssl/include -I uWebSockets/libdeflate -pthread -DLIBUS_USE_OPENSSL -flto %s%s -c -fPIC -std=c++20 -I uWebSockets/uSockets/src -I uWebSockets/src src/addon.cpp uWebSockets/uSockets/src/crypto/sni_tree.cpp -static -lbrotlienc", http3_flags, opt_flags, http3_include); // Static link so we don't need to depend
    
    char lsquic_libs[512] = "";
    if (!disable_http3) {
        snprintf(lsquic_libs, sizeof(lsquic_libs), " uWebSockets/uSockets/lsquic/%s/src/liblsquic/liblsquic.a", arch);
    }

    for (unsigned int i = 0; i < sizeof(versions) / sizeof(struct node_version); i++) {
        if (selected_version && strcmp(versions[i].name, selected_version)) {
            continue;
        }

        if(!addon_only) {
            run("%s %s -I targets/%s/%s/include/node", compiler, c_shared, versions[i].runtime, versions[i].name);
            run("%s %s -I targets/%s/%s/include/node", cpp_compiler, cpp_shared, versions[i].runtime, versions[i].name);
        }

        run("%s -pthread -flto %s *.o uWebSockets/uSockets/boringssl/%s/ssl/libssl.a uWebSockets/uSockets/boringssl/%s/crypto/libcrypto.a%s -I uWebSockets/libdeflate -std=c++20 -shared %s -o dist/akeno_%s_%s_%s.node", cpp_compiler, opt_flags, arch, arch, lsquic_libs, cpp_linker, os, arch, versions[i].abi);

        if(addon_only || latest_only) {
            break; // Only build for one version
        }
    }
}

void copy_files() {
#ifdef IS_WINDOWS
    run("copy \"src\\akeno.js\" dist /Y");
#else
    run("cp src/akeno.js dist/akeno.js");
#endif
}

/* Special case for windows */
void build_windows(const char *os, const char *arch) {
    const char *http3_defs = disable_http3 ? "/DUWS_NO_HTTP3" : "/DLIBUS_USE_QUIC";
    const char *http3_includes = disable_http3 ? "" : " /I uWebSockets/uSockets/lsquic/include /I uWebSockets/uSockets/lsquic/wincompat";
    const char *http3_libs = disable_http3 ? "" : " uWebSockets\\uSockets\\lsquic\\src\\liblsquic\\Debug\\lsquic.lib";
    const char *opt_flags = debug_mode ? "/Zi /Od" : "/O2";

    char c_shared[1800];
    char cpp_shared[1800];

    snprintf(c_shared, sizeof(c_shared), "/nologo /c /FS /MT /DWIN32_LEAN_AND_MEAN /DLIBUS_USE_LIBUV /DUWS_USE_LIBDEFLATE %s /I uWebSockets/uSockets/boringssl/include /I uWebSockets/libdeflate /DLIBUS_USE_OPENSSL %s%s /I uWebSockets/uSockets/src uWebSockets/uSockets/src/*.c uWebSockets/uSockets/src/eventing/*.c uWebSockets/uSockets/src/crypto/*.c", http3_defs, opt_flags, http3_includes);
    snprintf(cpp_shared, sizeof(cpp_shared), "/nologo /c /FS /MT /std:c++20 /EHsc /DWIN32_LEAN_AND_MEAN /DUWS_WITH_PROXY /DUWS_REMOTE_ADDRESS_USERSPACE /DLIBUS_USE_LIBUV /DUWS_USE_LIBDEFLATE %s /I uWebSockets/uSockets/boringssl/include /I uWebSockets/libdeflate /DLIBUS_USE_OPENSSL %s%s /I uWebSockets/uSockets/src /I uWebSockets/src src/addon.cpp uWebSockets/uSockets/src/crypto/sni_tree.cpp", http3_defs, opt_flags, http3_includes);

    for (unsigned int i = 0; i < sizeof(versions) / sizeof(struct node_version); i++) {
        if (selected_version && strcmp(versions[i].name, selected_version)) {
            continue;
        }

        if (!addon_only) {
            run("del /Q *.obj >NUL 2>&1");
            run("cl %s /I targets/%s/%s/include/node", c_shared, versions[i].runtime, versions[i].name);
            run("cl %s /I targets/%s/%s/include/node", cpp_shared, versions[i].runtime, versions[i].name);
        }

        run("link /NOLOGO /DLL /OUT:dist\\akeno_%s_%s_%s.node *.obj uWebSockets\\uSockets\\boringssl\\%s\\ssl\\ssl.lib uWebSockets\\uSockets\\boringssl\\%s\\crypto\\crypto.lib%s targets\\node-%s\\node.lib BrotliEnc.lib BrotliCommon.lib Ws2_32.lib Crypt32.lib Bcrypt.lib Iphlpapi.lib Userenv.lib Psapi.lib Advapi32.lib", os, arch, versions[i].abi, arch, arch, http3_libs, versions[i].name);

        if (addon_only || latest_only) {
            break;
        }
    }
}

int main(int argc, char **argv) {
#ifdef IS_WINDOWS
    printf("[Warning] Building Akeno-uWS for Windows is not fully supported. Any Windows build is considered experimental/unsupported and can break or not be up to expectations. Use at your own risk\n\n");
#endif
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--addon-only")) {
            addon_only = 1;
            printf("Only building for one Node.js version and skipping preparation, assuming you have built before\n");
        }
        if (!strcmp(argv[i], "--latest-only")) {
            latest_only = 1;
            printf("Only building for one Node.js version.\n");
        }
        if (!strcmp(argv[i], "--debug")) {
            debug_mode = 1;
            printf("Debug build enabled (-g -O0).\n");
        }
        if (!strcmp(argv[i], "--no-http3")) {
            disable_http3 = 1;
            printf("HTTP/3 and QUIC support disabled.\n");
        }
        if (strncmp(argv[i], "--version=", 10) == 0) {
            selected_version = argv[i] + 10;
        } else if (!strcmp(argv[i], "--version") && i + 1 < argc) {
            selected_version = argv[++i];
        }
    }

    const char *arch = X64;
#ifdef __arm__
    arch = ARM;
#endif
#ifdef __aarch64__
    arch = ARM64;
#endif

    if (!addon_only) {
        printf("[Preparing]\n");
        prepare("x64");
    }
    printf("\n[Building]\n");

#ifdef IS_MACOS
    /* If we are macOS, build both arm64 and x64 */
    if (!addon_only) {
        build_boringssl(X64);
        build_boringssl(ARM64);
        if (!disable_http3) {
            build_lsquic(X64);
            build_lsquic(ARM64);
        }
    }
#else
    /* For other platforms we simply compile the host */
    if (!addon_only) {
        build_boringssl(arch);
        if (!disable_http3) {
            build_lsquic(arch);
        }
    }
#endif


#ifdef IS_WINDOWS
    build_windows(OS, X64);
    // /* We can use clang, but we currently do use cl.exe still */
    // build_windows("clang -fms-runtime-lib=static",
    //       "clang++ -fms-runtime-lib=static",
    //       "",
    //       OS,
    //       X64);
#else
#ifdef IS_MACOS

    /* Apple special case */
    build("clang -target x86_64-apple-macos12",
          "clang++ -stdlib=libc++ -target x86_64-apple-macos12",
          "-undefined dynamic_lookup" MACOS_LINK_EXTRAS,
          OS,
          X64);

    /* Try and build for arm64 macOS 12 */
    build("clang -target arm64-apple-macos12",
          "clang++ -stdlib=libc++ -target arm64-apple-macos12",
          "-undefined dynamic_lookup" MACOS_LINK_EXTRAS,
          OS,
          ARM64);

#else
    /* Linux does not cross-compile but picks whatever arch the host is on (we run on both x64 and ARM64) */
    build("clang-18",
          "clang++-18",
          LINUX_LINK_EXTRAS,
          OS,
          arch);
#endif
#endif

    copy_files();
}
