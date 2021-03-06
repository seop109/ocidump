#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include "ocidump.h"

#ifndef OCI_CONTINUE
#define OCI_CONTINUE -24200
#endif

#define OCIDUMP_ENABLE_KERNEL32_DLL_HOOK 0 /* This option is problematic...  */

BOOL forward_only = FALSE;
BOOL ocidump_use_dbghelp = TRUE; /* FIXME: changed by a directive in OCIDUMP_CONFIG */
CRITICAL_SECTION ocidump_dbghelp_lock;

static HMODULE hModuleOCI = NULL;
static HMODULE hThisModule;
static void ocidump_setup_win32_api_hook(void);

static void setup_oci_module_handle(void)
{
    HMODULE hMod = NULL;

    if (hModuleOCI != NULL) {
        return;
    }
    hMod = GetModuleHandle("OCI.DLL");
    if (hMod == hThisModule) {
        hMod = LoadLibrary("OCI-ORIG.DLL");
        if (hMod == NULL) {
          ocidump_log(0, "ERROR! Could not load library OCI-ORIG.DLL.");
          exit(1);
        }
    } else if (hMod == NULL) {
        ocidump_log(0, "ERROR! Could not load library OCI.DLL.");
        exit(1);
    }
    hModuleOCI = hMod;
}

static void replace_import_section(HANDLE hModule);

__declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason) {
        char path[MAX_PATH];
        char *p;

    case DLL_PROCESS_ATTACH:
        GetModuleFileName(hinstDLL, path, sizeof(path));
        p = strrchr(path, '\\');
        if (p == NULL) {
            return FALSE;
        }
        if (stricmp(p + 1, "OCI.DLL") == 0) {
            /* ocidump.dll acts as oci.dll. */
            char *val = getenv("OCIDUMP_ENABLE");
            if (val == NULL || atoi(val) == 0) {
                /* disable all hooks */
                int i;
                forward_only = TRUE;
                ocidump_use_dbghelp = FALSE;
                for (i = 0; i < ocidump_hook_cnt; i++) {
                    *ocidump_hooks[i].flags = 0;
                }
            }
        }
        hThisModule = hinstDLL;
        ocidump_tls_key = TlsAlloc();
        break;
    case DLL_PROCESS_DETACH:
        TlsFree(ocidump_tls_key);
        break;
    }
    return TRUE;
}

static sword ocidumpEnvCallback(OCIEnv *env, ub4 mode, size_t xtramem_sz, void *usrmemp, OCIUcb *ucbDesc)
{
    return OCI_CONTINUE;
}

/* The entry point of ORA_OCI_UCBPKG. */
__declspec(dllexport) sword ocidumpInit(void *metaCtx, void *libCtx, ub4 argfmt, sword argc, void **argv)
{
    ocidump_init();
    ocidump_setup_win32_api_hook();
    return OCISharedLibInit(metaCtx, libCtx, argfmt, argc, argv, ocidumpEnvCallback);
}

#if OCIDUMP_ENABLE_KERNEL32_DLL_HOOK
static FARPROC (WINAPI *GetProcAddress_orig)(HMODULE hModule, LPCSTR lpProcName);

static FARPROC WINAPI GetProcAddress_hook(HMODULE hModule, LPCSTR lpProcName)
{
    FARPROC ret = GetProcAddress_orig(hModule, lpProcName);

    if (ret != NULL && hModule == hModuleOCI) {
        int i;
        for (i = 0; i < ocidump_hook_cnt; i++) {
            if (ret == *ocidump_hooks[i].orig_func) {
                return (FARPROC)ocidump_hooks[i].hook_func;
            }
        }
    }
    return ret;
}

static HMODULE (WINAPI *LoadLibraryA_orig)(LPCSTR lpLibFileName);

static HMODULE WINAPI LoadLibraryA_hook(LPCSTR lpLibFileName)
{
    HMODULE hModule = LoadLibraryA_orig(lpLibFileName);

    if (hModule != NULL) {
      replace_import_section(hModule);
    }
    return hModule;
}

static HMODULE (WINAPI *LoadLibraryW_orig)(LPCWSTR lpLibFileName);

static HMODULE WINAPI LoadLibraryW_hook(LPCWSTR lpLibFileName)
{
    HMODULE hModule = LoadLibraryW_orig(lpLibFileName);

    if (hModule != NULL) {
        replace_import_section(hModule);
    }
    return hModule;
}

static HMODULE (WINAPI *LoadLibraryExA_orig)(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

static HMODULE WINAPI LoadLibraryExA_hook(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    HMODULE hModule = LoadLibraryExA_orig(lpLibFileName, hFile, dwFlags);

    if (hModule != NULL && (dwFlags & LOAD_LIBRARY_AS_DATAFILE) == 0) {
        replace_import_section(hModule);
    }
    return hModule;
}

static HMODULE (WINAPI *LoadLibraryExW_orig)(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags);

static HMODULE WINAPI LoadLibraryExW_hook(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
{
    HMODULE hModule = LoadLibraryExW_orig(lpLibFileName, hFile, dwFlags);

    if (hModule != NULL && (dwFlags & LOAD_LIBRARY_AS_DATAFILE) == 0) {
        replace_import_section(hModule);
    }
    return hModule;
}

static ocidump_hook_t kernel32_dll_hooks[] = {
    {"GetProcAddress", (void*)GetProcAddress_hook, (void**)&GetProcAddress_orig, NULL},
    {"LoadLibraryA", (void*)LoadLibraryA_hook, (void**)&LoadLibraryA_orig, NULL},
    {"LoadLibraryW", (void*)LoadLibraryW_hook, (void**)&LoadLibraryW_orig, NULL},
    {"LoadLibraryExA", (void*)LoadLibraryExA_hook, (void**)&LoadLibraryExA_orig, NULL},
    {"LoadLibraryExW", (void*)LoadLibraryExW_hook, (void**)&LoadLibraryExW_orig, NULL},
};
#define NUM_OF_KERNEL32_DLL_HOOKS (sizeof(kernel32_dll_hooks)/sizeof(kernel32_dll_hooks[0]))
#endif

static void replace_import_section(HANDLE hModule)
{
    ULONG ulSize;
    IMAGE_IMPORT_DESCRIPTOR *desc = (IMAGE_IMPORT_DESCRIPTOR*)ImageDirectoryEntryToData(hModule, TRUE, IMAGE_DIRECTORY_ENTRY_IMPORT, &ulSize);

    if (desc != NULL) {
        while (desc->Name != 0) {
            ocidump_hook_t *hooks = NULL;
            int cnt;
            const char *dll_name = (const char*)hModule + desc->Name;
            if (stricmp(dll_name, "OCI.DLL") == 0) {
                hooks = ocidump_hooks;
                cnt = ocidump_hook_cnt;
#if OCIDUMP_ENABLE_KERNEL32_DLL_HOOK
            } else if (stricmp(dll_name, "KERNEL32.DLL") == 0) {
                hooks = kernel32_dll_hooks;
                cnt = NUM_OF_KERNEL32_DLL_HOOKS;
#endif
            }
            if (hooks != NULL) {
                IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA*)((char*)hModule + desc->FirstThunk);
                while (thunk->u1.Function != 0) {
                    int i;
                    for (i = 0; i < cnt; i++) {
                        if ((void *)thunk->u1.Function == *hooks[i].orig_func) {
                            DWORD dwOld;
                            DWORD dwDummy;

                            ocidump_log(OCIDUMP_LOG_HOOK, "# Replace %s from %p to %p.\n",
                                        hooks[i].name, (void*)thunk->u1.Function, ocidump_hooks[i].hook_func);
                            VirtualProtect(&thunk->u1.Function, sizeof(void *), PAGE_EXECUTE_READWRITE, &dwOld);
                            thunk->u1.Function = (size_t)ocidump_hooks[i].hook_func;
                            VirtualProtect(&thunk->u1.Function, sizeof(void *), dwOld, &dwDummy);
                        }
                    }
                    thunk++;
                }
            }
            desc++;
        }
    }
}

static void ocidump_setup_win32_api_hook(void)
{
    HANDLE hSnapModule;
    MODULEENTRY32 me = {sizeof(me)};
    BOOL ok;
#if OCIDUMP_ENABLE_KERNEL32_DLL_HOOK
    HMODULE hModuleKernel32 = GetModuleHandle("Kernel32.dll");
    int i;

    for (i = 0; kernel32_dll_hooks[i].name != NULL; i++) {
        *kernel32_dll_hooks[i].orig_func = (void*)GetProcAddress(hModuleKernel32, kernel32_dll_hooks[i].name);
        ocidump_log(OCIDUMP_LOG_HOOK, "# GetProcAddress(\"KERNEL32.DLL\", \"%s\") => %p\n",
                    kernel32_dll_hooks[i].name, *kernel32_dll_hooks[i].orig_func);
    }
#endif

    hSnapModule = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (hSnapModule == INVALID_HANDLE_VALUE) {
        ocidump_log(0, "ERROR: Could not enumerate modules to hook functions.\n");
        return;
    }
    for (ok = Module32First(hSnapModule, &me); ok; ok = Module32Next(hSnapModule, &me)) {
        if (me.hModule == hThisModule) {
            continue;
        }
        ocidump_log(OCIDUMP_LOG_HOOK, "# Checking %s\n", me.szExePath);
        replace_import_section(me.hModule);
    }
    CloseHandle(hSnapModule);
}

void ocidump_init_win32(void)
{
    int i;

    setup_oci_module_handle();

    if (ocidump_use_dbghelp) {
        SymInitialize(GetCurrentProcess(), NULL, TRUE);
        InitializeCriticalSectionAndSpinCount(&ocidump_dbghelp_lock, 4000);
    }

    for (i = 0; i < ocidump_hook_cnt; i++) {
        *ocidump_hooks[i].orig_func = (void*)GetProcAddress(hModuleOCI, ocidump_hooks[i].name);
        ocidump_log(OCIDUMP_LOG_HOOK, "# GetProcAddress(\"OCI.DLL\", \"%s\") => %p\n",
                    ocidump_hooks[i].name, *ocidump_hooks[i].orig_func);
    }
}
