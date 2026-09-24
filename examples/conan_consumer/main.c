/*
 * piplugin example - a consumer of the PACKAGED framework (roadmap ECO-04)
 *
 * It includes headers from several different parts of the package and links them,
 * so a successful build proves the package carries: the public headers, the
 * imported targets, the static kit libraries, and (in the install-tree case)
 * third-party dependency propagation.
 *
 * Built by scripts/verify_package.ps1 against both a `cmake --install` tree and a
 * Conan package - "the package works for someone else" is exactly the claim unit
 * tests cannot make. See CMakeLists.txt for why the imgui kit is optional here.
 */
#include "pi_event_router.h"
#include "pi_host_session.h"
#include "piplugin/pi_plugin.h"
#ifdef PI_PLUGIN_CONSUMER_WITH_IMGUI
    #include "pi_imgui_view.h"
#endif

#include <stdio.h>

int main(void)
{
    PiPluginEventRouter* router = NULL;

    printf("== piplugin packaged consumer ==\n");
    printf("PI_PLUGIN_API_VERSION = %u.%u (0x%08X)\n",
           (unsigned)PI_PLUGIN_API_VERSION_MAJOR(PI_PLUGIN_API_VERSION),
           (unsigned)PI_PLUGIN_API_VERSION_MINOR(PI_PLUGIN_API_VERSION),
           (unsigned)PI_PLUGIN_API_VERSION);

    /* Touch each linked part so the linker cannot drop it: the core rejects a
     * NULL out-parameter, the router can be created/destroyed. */
    if (pi_plugin_host_services_create_default(NULL, NULL, PI_INVALID_WINDOW, NULL) != PI_E_INVALIDARG)
    {
        printf("unexpected: host services accepted a NULL out-parameter\n");
        return 1;
    }
    if (pi_plugin_event_router_create(&router) != PI_OK || router == NULL)
    {
        printf("event router could not be created\n");
        return 1;
    }
    printf("core + host kit L0 + event router: OK\n");
    pi_plugin_event_router_destroy(router);

#ifdef PI_PLUGIN_CONSUMER_WITH_IMGUI
    printf("imgui adapter kit linked: pi_plugin_imgui_view_create = %p\n",
           (void*)(uintptr_t)&pi_plugin_imgui_view_create);
#else
    printf("imgui adapter kit: not linked in this configuration\n");
#endif

    printf("RESULT: PASS\n");
    return 0;
}
