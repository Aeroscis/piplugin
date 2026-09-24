/*
 * piplugin example - a plugin that uses the minimal Win32 kit next door
 *
 * The point of this pair (examples/minimal_kit_win32) is docs/design/adapter-spec.md:
 * the kit is ~250 lines of plain C written from that spec alone, and this plugin
 * is what a plugin author writes against such a kit - a paint callback and
 * nothing else. No toolkit, no window management, no event loop.
 *
 * Run it with examples/minimal_host, or send it through the official conformance
 * harness (scripts/run_selftest.ps1 -Plugin pi_plugin_example_plugin_win32.dll):
 *
 *     pi_plugin_example_minimal_host.exe pi_plugin_example_plugin_win32.dll
 */
#include <windows.h> /* the paint callback talks GDI directly (the kit's header
                           * stays toolkit-agnostic and passes the HDC as void*) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pi_win32_view.h"
#include "piplugin/pi_plugin.h"

static PiGuid const EXAMPLE_WIN32_CLASS_GUID =
    PI_GUID(0x7A2E9C10, 0x5F84, 0x4D37, 0xB6, 0x02, 0xE9, 0x53, 0x1C, 0x88, 0x47, 0xAF);

typedef struct Win32Plugin {
    PiRefCountedBase       base;    /* MUST be first */
    IPiPluginHostServices* host;    /* add-ref'd */
    IPiPluginHostUI*       host_ui; /* add-ref'd, NULL when the host has no UI */
    uint32_t               paints;
} Win32Plugin;

static uint32_t PI_CALL Plugin_AddRef(void* self)
{
    return pi_refcounted_add_ref(self);
}
static uint32_t PI_CALL Plugin_Release(void* self)
{
    return pi_refcounted_release(self);
}

/* ---- the whole UI: GDI text on the kit's window -------------------------- */
static void PaintUi(void* user_data, void* hdc_ptr, int32_t width, int32_t height)
{
    Win32Plugin* me = (Win32Plugin*)user_data;
    HDC          dc = (HDC)hdc_ptr;
    RECT         rc;
    char         line[128];

    ++me->paints;

    rc.left   = 0;
    rc.top    = 0;
    rc.right  = width;
    rc.bottom = height;
    FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(24, 24, 28));

    TextOutA(dc, 16, 16, "Hello from a Win32 example plugin!", 34);
    snprintf(line, sizeof(line), "paint #%u   client %dx%d",
             (unsigned)me->paints, (int)width, (int)height);
    TextOutA(dc, 16, 40, line, (int)strlen(line));
    TextOutA(dc, 16, 64, "(this UI is drawn by the kit next door, in plain C)", 51);

    FrameRect(dc, &rc, (HBRUSH)GetStockObject(GRAY_BRUSH));
}

/* ---- IPiPluginBase ------------------------------------------------------- */
static PiResult PI_CALL Plugin_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_BASE))
    {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static PiResult PI_CALL Plugin_Init(void* self_ptr, IPiPluginHostServices* host)
{
    Win32Plugin* me = (Win32Plugin*)self_ptr;
    if (me->host)
    {
        return PI_OK; /* idempotent */
    }
    if (!host)
    {
        return PI_OK;
    }

    me->host = host;
    pi_iunknown_add_ref((IPiUnknown*)host);

    /* Optional capability: a headless host does not expose IPiPluginHostUI, and then we
     * publish no view at all (see GetView). */
    if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)host, &PI_PLUGIN_IID_HOST_UI,
                                                 (void**)&me->host_ui)))
    {
        printf("[win32 plugin] GUI host detected\n");
    }
    else
    {
        me->host_ui = NULL;
        printf("[win32 plugin] headless host: no UI, no view\n");
    }
    return PI_OK;
}

static PiResult PI_CALL Plugin_Term(void* self_ptr)
{
    Win32Plugin* me = (Win32Plugin*)self_ptr;
    printf("[win32 plugin] terminating after %u paint(s)\n", (unsigned)me->paints);
    /* The kit's window class and WndProc live in THIS module, so tear them down
     * before the host unloads us. Idempotent; the host detaching first is fine. */
    pi_plugin_win32_view_shutdown();
    return PI_OK;
}

static PiResult PI_CALL Plugin_GetView(void* self_ptr, IPiPluginView** out)
{
    Win32Plugin*          me = (Win32Plugin*)self_ptr;
    PiPluginWin32ViewDesc desc;

    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (!me->host_ui)
    {
        *out = NULL;
        return PI_E_NOINTERFACE;
    }

    memset(&desc, 0, sizeof(desc));
    desc.paint     = &PaintUi;
    desc.user_data = me;
    return pi_plugin_win32_view_create(&desc, out);
}

static void Plugin_Destroy(void* self)
{
    Win32Plugin* me = (Win32Plugin*)self;
    if (me->host_ui)
    {
        pi_iunknown_release((IPiUnknown*)me->host_ui);
        me->host_ui = NULL;
    }
    if (me->host)
    {
        pi_iunknown_release((IPiUnknown*)me->host);
        me->host = NULL;
    }
    free(me);
}

static IPiPluginBaseVtbl const s_plugin_vtbl = {
    {&Plugin_Qi, &Plugin_AddRef, &Plugin_Release},
    &Plugin_Init,
    &Plugin_Term,
    &Plugin_GetView
};

/* ---- factory + entry point ---------------------------------------------- */
typedef struct Win32Factory {
    PiRefCountedBase base;
} Win32Factory;

static Win32Factory       s_factory;
static PiPluginCapability s_caps[2];
static PiPluginProperty   s_props[1];
static PiPluginDescriptor s_desc;
static int                s_initialized = 0;

static uint32_t PI_CALL Factory_AddRef(void* self)
{
    return pi_refcounted_add_ref(self);
}
static uint32_t PI_CALL Factory_Release(void* self)
{
    return pi_refcounted_release(self);
}

static PiResult PI_CALL Factory_Qi(void* self_ptr, PiGuid const* iid, void** out)
{
    if (!out)
    {
        return PI_E_INVALIDARG;
    }
    if (pi_guid_equal(iid, &PI_IID_UNKNOWN) || pi_guid_equal(iid, &PI_PLUGIN_IID_PLUGIN_FACTORY))
    {
        *out = self_ptr;
        pi_refcounted_add_ref(self_ptr);
        return PI_OK;
    }
    *out = NULL;
    return PI_E_NOINTERFACE;
}

static PiPluginDescriptor const* PI_CALL Factory_Desc(void* self)
{
    (void)self;
    return &s_desc;
}
static uint32_t PI_CALL Factory_Count(void* self)
{
    (void)self;
    return 1;
}

static PiResult PI_CALL Factory_Guid(void* self, uint32_t index, PiGuid* guid)
{
    (void)self;
    if (index != 0 || !guid)
    {
        return PI_E_INVALIDARG;
    }
    *guid = EXAMPLE_WIN32_CLASS_GUID;
    return PI_OK;
}

static PiResult PI_CALL Factory_Create(void* self, PiGuid const* guid, IPiPluginHostServices* host,
                                       IPiPluginBase** out)
{
    Win32Plugin* plugin;
    (void)self;
    if (!guid || !out)
    {
        return PI_E_INVALIDARG;
    }
    *out = NULL;
    if (!pi_guid_equal(guid, &EXAMPLE_WIN32_CLASS_GUID))
    {
        return PI_E_NOINTERFACE;
    }

    plugin = (Win32Plugin*)calloc(1, sizeof(Win32Plugin));
    if (!plugin)
    {
        return PI_E_OUTOFMEMORY;
    }
    pi_refcounted_init_with_destroy(&plugin->base, (IPiUnknownVtbl const*)&s_plugin_vtbl,
                                    &Plugin_Destroy);
    Plugin_Init(plugin, host);
    *out = (IPiPluginBase*)&plugin->base;
    return PI_OK;
}

static IPiPluginFactoryVtbl const s_factory_vtbl = {
    {&Factory_Qi, &Factory_AddRef, &Factory_Release},
    &Factory_Desc,
    &Factory_Count,
    &Factory_Guid,
    &Factory_Create
};

PI_PLUGIN_ENTRY_DECL
{
    if (!out_factory)
    {
        return PI_E_INVALIDARG;
    }
    *out_factory = NULL;

    if (!s_initialized)
    {
        pi_refcounted_init(&s_factory.base, (IPiUnknownVtbl const*)&s_factory_vtbl);

        s_caps[0].iid   = PI_PLUGIN_IID_PLUGIN_VIEW;
        s_caps[0].flags = PI_PLUGIN_CAP_PROVIDES;
        s_caps[1].iid   = PI_PLUGIN_IID_HOST_UI;
        s_caps[1].flags = PI_PLUGIN_CAP_OPTIONAL;

        pi_plugin_descriptor_init(&s_desc);
        s_desc.name             = "Example Win32 Plugin";
        s_desc.vendor           = "piplugin examples";
        s_desc.version          = "1.0.0";
        s_desc.category         = "Example/UI";
        s_desc.api_version      = PI_PLUGIN_API_VERSION;
        s_desc.capabilities     = s_caps;
        s_desc.capability_count = 2;

        s_props[0].key        = "com.example.kind";
        s_props[0].value      = "win32-plugin";
        s_desc.properties     = s_props;
        s_desc.property_count = 1;

        s_initialized = 1;
    }

    pi_refcounted_add_ref(&s_factory.base);
    *out_factory = (IPiPluginFactory*)&s_factory.base;
    return PI_OK;
}
