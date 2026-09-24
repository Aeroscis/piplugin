/*
 * piplugin example - plugin discovery, first step (FUT-07)
 *
 * WHAT THIS IS
 *   A directory scanner that answers "what plugins are in this folder?" without
 *   running any of them: it walks a directory, tries every candidate, reads the
 *   descriptor (name / vendor / version / category / capabilities / properties)
 *   and IMMEDIATELY unloads it again. Nothing is ever instantiated, so discovery
 *   cannot execute plugin code beyond the factory itself.
 *
 * WHY IT LIVES IN examples/
 *   FUT-07 (discovery + distribution) is a whole direction, not a feature: a
 *   manifest format, version resolution, capability/dependency solving and
 *   (eventually) signing. This file is deliberately the smallest thing that
 *   produces a CODE FACT about it - can a host learn a directory's contents from
 *   descriptors alone? - so the design can be argued with evidence instead of
 *   opinion. It changes NO framework ABI and no core file.
 *
 * WHAT IS REAL HERE vs. WHAT IS A PLACEHOLDER
 *   real:        walking a directory; "not a plugin" is a normal, reported
 *                outcome (load failures, version-gate rejections, missing
 *                capabilities); every candidate is unloaded again, including the
 *                ones that fail.
 *   placeholder: version selection. Plugins that share a descriptor `name` are
 *                grouped and the highest `version` wins. That is a numeric
 *                component compare, nothing more - a real resolver also weighs
 *                api_version, capabilities, platform and dependencies (FUT-07).
 *
 * The other thing this example demonstrates is a lifetime rule: a descriptor's
 * strings belong to the MODULE. Everything needed later is deep-copied before
 * pi_plugin_host_session_unload() (see ScanEntry).
 *
 * Windows-only for now, because it lists files with FindFirstFileA; it has no
 * other platform or GUI dependency. See README.md for the three steps.
 */
#include "piplugin/pi_plugin.h"
#include "pi_host_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>

#define SCAN_MAX_ENTRIES  128u
#define SCAN_MAX_PROPS    8u
#define SCAN_TEXT_MAX     128u
#define SCAN_VALUE_MAX    96u
#define SCAN_PATH_MAX     512u

typedef struct ScanProp {
    char key[SCAN_TEXT_MAX];
    char value[SCAN_VALUE_MAX];
} ScanProp;

typedef struct ScanEntry {
    char     file[SCAN_TEXT_MAX];
    char     name[SCAN_TEXT_MAX];
    char     vendor[SCAN_TEXT_MAX];
    char     version[SCAN_TEXT_MAX];
    char     category[SCAN_TEXT_MAX];
    uint32_t api_version;
    uint32_t provides;
    uint32_t required;
    uint32_t optional;
    uint32_t property_count;          /* as declared (may exceed SCAN_MAX_PROPS) */
    uint32_t property_copied;
    ScanProp properties[SCAN_MAX_PROPS];
    int      usable;
    char     reason[256];             /* why not, when !usable */
} ScanEntry;

static ScanEntry g_entries[SCAN_MAX_ENTRIES];
static uint32_t  g_entry_count     = 0;
static uint32_t  g_candidate_count = 0;

static void CopyString(char* dst, size_t dst_size, const char* src)
{
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy_s(dst, dst_size, src, _TRUNCATE);
}

/* Placeholder version compare: numeric components, dot separated, missing
 * components count as 0 ("1.2" == "1.2.0"). Returns <0, 0 or >0. */
static int VersionCompare(const char* a, const char* b)
{
    int i;
    for (i = 0; i < 4; ++i) {
        long va = 0, vb = 0;
        int  ha = 0, hb = 0;

        while (a && *a >= '0' && *a <= '9') { va = va * 10 + (*a - '0'); ++a; ha = 1; }
        while (b && *b >= '0' && *b <= '9') { vb = vb * 10 + (*b - '0'); ++b; hb = 1; }
        if (va != vb) return (va < vb) ? -1 : 1;
        if (a && *a == '.') ++a;
        if (b && *b == '.') ++b;
        if (!ha && !hb) break;               /* nothing numeric left on either side */
    }
    return 0;
}

static void ScanFile(PiPluginHostSession* session, const char* directory, const char* file)
{
    ScanEntry* e;
    char       path[SCAN_PATH_MAX];
    uint32_t   slot = PI_PLUGIN_HOST_SESSION_INVALID_SLOT;
    PiResult   hr;
    const PiPluginDescriptor* desc;
    uint32_t   i;

    if (g_entry_count >= SCAN_MAX_ENTRIES) return;
    e = &g_entries[g_entry_count];
    memset(e, 0, sizeof(*e));
    CopyString(e->file, sizeof(e->file), file);
    CopyString(e->reason, sizeof(e->reason), "unknown failure");

    snprintf(path, sizeof(path), "%s\\%s", directory, file);

    /* inspect = load module + factory + version gate + capability gates, and
     * STOP there (no instance is created). A failure is a normal outcome: most
     * DLLs in a deployment folder are not plugins. */
    hr = pi_plugin_host_session_inspect(session, path, &slot);
    if (PI_FAILED(hr)) {
        CopyString(e->reason, sizeof(e->reason), pi_plugin_host_session_last_error(session));
        ++g_entry_count;
        return;
    }

    desc = pi_plugin_host_session_get_descriptor(session, slot);
    if (desc) {
        /* Deep-copy everything we want to keep: the descriptor's memory belongs
         * to the module, which we unload below. */
        CopyString(e->name, sizeof(e->name), desc->name ? desc->name : "(unnamed)");
        CopyString(e->vendor, sizeof(e->vendor), desc->vendor ? desc->vendor : "-");
        CopyString(e->version, sizeof(e->version), desc->version ? desc->version : "0");
        CopyString(e->category, sizeof(e->category), desc->category ? desc->category : "-");
        e->api_version    = desc->api_version;
        e->property_count = desc->property_count;
        for (i = 0; i < desc->capability_count; ++i) {
            const PiPluginCapability* cap = &desc->capabilities[i];
            if (cap->flags & PI_PLUGIN_CAP_PROVIDES) ++e->provides;
            if (cap->flags & PI_PLUGIN_CAP_REQUIRED) ++e->required;
            if (cap->flags & PI_PLUGIN_CAP_OPTIONAL) ++e->optional;
        }
        for (i = 0; i < desc->property_count && i < SCAN_MAX_PROPS; ++i) {
            CopyString(e->properties[i].key, SCAN_TEXT_MAX, desc->properties[i].key);
            CopyString(e->properties[i].value, SCAN_VALUE_MAX, desc->properties[i].value);
            ++e->property_copied;
        }
        e->usable = 1;
    } else {
        CopyString(e->reason, sizeof(e->reason), "descriptor missing");
    }

    /* Unload immediately: discovery must not leave modules mapped. */
    pi_plugin_host_session_unload(session, slot);
    ++g_entry_count;
}

static void ScanDirectory(PiPluginHostSession* session, const char* directory)
{
    WIN32_FIND_DATAA fd;
    HANDLE           h;
    char             pattern[SCAN_PATH_MAX];

    snprintf(pattern, sizeof(pattern), "%s\\*.dll", directory);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        printf("cannot list '%s' (no *.dll found)\n", directory);
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ++g_candidate_count;
        ScanFile(session, directory, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void PrintInventory(void)
{
    uint32_t i, usable = 0, rejected = 0;

    printf("\n== inventory: usable plugins ==\n");
    printf("%-34s %-9s %-17s %s\n", "name", "version", "category", "file");
    for (i = 0; i < g_entry_count; ++i) {
        const ScanEntry* e = &g_entries[i];
        if (!e->usable) continue;
        ++usable;
        printf("%-34s %-9s %-17s %s\n", e->name, e->version, e->category, e->file);
    }
    if (!usable) printf("(none)\n");

    printf("\n== rejected / not a plugin ==\n");
    for (i = 0; i < g_entry_count; ++i) {
        const ScanEntry* e = &g_entries[i];
        if (e->usable) continue;
        ++rejected;
        printf("%-34s %s\n", e->file, e->reason);
    }
    if (!rejected) printf("(none)\n");
    printf("\nusable=%u rejected=%u\n", (unsigned)usable, (unsigned)rejected);
}

/* Placeholder selection: same descriptor name -> highest version wins. */
static void PrintVersionSelection(void)
{
    uint32_t i, j;
    int      any = 0;

    printf("\n== version selection (PLACEHOLDER: highest version per name) ==\n");
    for (i = 0; i < g_entry_count; ++i) {
        const ScanEntry* winner;
        uint32_t         candidates = 0;

        if (!g_entries[i].usable) continue;
        /* only act on the first entry of each name group */
        for (j = 0; j < i; ++j) {
            if (g_entries[j].usable && strcmp(g_entries[j].name, g_entries[i].name) == 0) break;
        }
        if (j != i) continue;

        winner = &g_entries[i];
        for (j = 0; j < g_entry_count; ++j) {
            if (!g_entries[j].usable) continue;
            if (strcmp(g_entries[j].name, winner->name) != 0) continue;
            ++candidates;
            if (VersionCompare(g_entries[j].version, winner->version) > 0) winner = &g_entries[j];
        }
        if (candidates > 1) {
            any = 1;
            printf("'%s': %u candidate(s) -> picks %s (%s)\n",
                   winner->name, (unsigned)candidates, winner->version, winner->file);
        }
    }
    if (!any) printf("no descriptor name appears twice - nothing to choose\n");
    printf("(a real resolver also weighs api_version, capabilities, platform and "
           "dependencies; see FUT-07)\n");
}

int main(int argc, char** argv)
{
    const char*          directory = (argc > 1 && argv[1]) ? argv[1] : ".";
    IPiPluginHostServices*     services  = NULL;
    PiPluginHostSession* session   = NULL;
    PiResult             hr;
    uint32_t             i;

    printf("== piplugin plugin scan (FUT-07 first step) ==\n");
    printf("directory: %s\n\n", directory);

    /* A scanner needs no UI and no message loop: headless services are enough
     * for the gates, and nothing is ever instantiated. */
    hr = pi_plugin_host_services_create_default(NULL, NULL, PI_INVALID_WINDOW, &services);
    if (PI_FAILED(hr)) { printf("FATAL: host services (hr=%d)\n", (int)hr); return 1; }

    hr = pi_plugin_host_session_create(services, &session);
    if (PI_FAILED(hr)) {
        printf("FATAL: session (hr=%d)\n", (int)hr);
        pi_iunknown_release((IPiUnknown*)services);
        return 1;
    }

    ScanDirectory(session, directory);

    printf("candidates: %u file(s), %u inspected\n",
           (unsigned)g_candidate_count, (unsigned)g_entry_count);

    for (i = 0; i < g_entry_count; ++i) {
        const ScanEntry* e = &g_entries[i];
        uint32_t         p;
        if (!e->usable) continue;
        printf("\n[%u] %s\n", (unsigned)(i + 1), e->file);
        printf("     name     : %s\n", e->name);
        printf("     vendor   : %s\n", e->vendor);
        printf("     version  : %s\n", e->version);
        printf("     category : %s\n", e->category);
        printf("     api      : 0x%08X\n", (unsigned)e->api_version);
        printf("     caps     : provides=%u required=%u optional=%u\n",
               (unsigned)e->provides, (unsigned)e->required, (unsigned)e->optional);
        printf("     props    : %u\n", (unsigned)e->property_count);
        for (p = 0; p < e->property_copied; ++p)
            printf("       %s = %s\n", e->properties[p].key, e->properties[p].value);
        if (e->property_count > e->property_copied)
            printf("       ... (%u more not shown)\n",
                   (unsigned)(e->property_count - e->property_copied));
    }

    PrintInventory();
    PrintVersionSelection();

    pi_plugin_host_session_destroy(session);
    pi_iunknown_release((IPiUnknown*)services);

    printf("\nRESULT: %s\n", g_entry_count ? "PASS" : "FAIL");
    return g_entry_count ? 0 : 1;
}
