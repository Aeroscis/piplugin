/*
 * piplugin - C++ RAII layer (header-only, OPTIONAL)
 *
 * The framework's ABI is the C surface in pi_plugin.h; this header is sugar for
 * C++ host and plugin authors who are tired of hand-writing AddRef/Release
 * pairs, and of the leaks that come with missing one. It adds no ABI, no
 * exported symbol and no runtime dependency: everything below is a template
 * that calls the same inline helpers the C API already exposes.
 *
 * It is deliberately NOT included by pi_plugin.h. The C umbrella stays pure C
 * (FFI bindings and .c files include it and must not trip over templates), so
 * the C++ layer is one explicit include:
 *
 *     #include "piplugin/pi_cpp.h"
 *
 * Nothing else changes: every C call still works, and you can mix the two
 * freely. This header is C++11 and later; the project itself builds C++17.
 */
#ifndef PI_PLUGIN_CPP_H
#define PI_PLUGIN_CPP_H

#if !defined(__cplusplus)
#  error "piplugin/pi_cpp.h is the C++ layer; C code wants piplugin/pi_plugin.h"
#endif
#if __cplusplus < 201103L
#  error "piplugin/pi_cpp.h needs C++11 (move semantics); the project builds C++17"
#endif

#include "pi_plugin.h"

/* --------------------------------------------------------------------------
 * pi_cpp_destroy<T> - destroy callback for C++ objects
 *
 * Pass it to pi_refcounted_init_with_destroy() so that the destructor runs when
 * the refcount reaches zero:
 *
 *     pi_refcounted_init_with_destroy(&m_base,
 *                                     (const IPiUnknownVtbl*)&s_vtbl,
 *                                     &pi_cpp_destroy<MyPlugin>);
 * -------------------------------------------------------------------------- */
template <typename T>
void pi_cpp_destroy(void* self_ptr)
{
    delete static_cast<T*>(self_ptr);
}

/* --------------------------------------------------------------------------
 * PiIidOf<T> - the framework IID of an interface type
 *
 * Lets qi_to<T>() work without spelling the GUID out at every call site (and
 * without getting it wrong). App-defined interfaces get their own specialisation
 * in the app's header:
 *
 *     template <> struct PiIidOf<IMyService> {
 *         static const PiGuid& get() { return MY_SERVICE_IID; }
 *     };
 * -------------------------------------------------------------------------- */
template <typename T> struct PiIidOf;

#define PI_CPP_IID_SPEC(type, iid)                       \
    template <> struct PiIidOf<type> {                   \
        static const PiGuid& get() { return (iid); }     \
    }

PI_CPP_IID_SPEC(IPiUnknown,       PI_IID_UNKNOWN);
PI_CPP_IID_SPEC(IPiHostServices,  PI_IID_HOST_SERVICES);
PI_CPP_IID_SPEC(IPiHostUI,        PI_IID_HOST_UI);
PI_CPP_IID_SPEC(IPiPluginFactory, PI_IID_PLUGIN_FACTORY);
PI_CPP_IID_SPEC(IPiPluginBase,    PI_IID_PLUGIN_BASE);
PI_CPP_IID_SPEC(IPiPluginView,    PI_IID_PLUGIN_VIEW);
PI_CPP_IID_SPEC(IPiService,       PI_IID_SERVICE);

#undef PI_CPP_IID_SPEC

/* --------------------------------------------------------------------------
 * PiPtr<T> - owning handle to a reference-counted interface
 *
 *      IPiHostUI* raw = NULL;
 *      PiPtr<IPiHostUI> ui;                       // empty
 *      if (PI_SUCCEEDED(pi_iunknown_query_interface((IPiUnknown*)host,
 *                                                   &PI_IID_HOST_UI, (void**)ui.put())))
 *          ... ui->pi_host_get_parent_window(...)
 *      // ui's destructor releases it
 *
 * Ownership rules (the same ones interfaces.md 2.3 freezes):
 *   - every interface pointer the framework RETURNS is already AddRef'd for the
 *     caller, so PiPtr ADOPTS it (no extra AddRef) - see the constructor;
 *   - borrowed pointers (descriptor, native window) must NOT be wrapped: their
 *     lifetime is not the caller's, and releasing one is undefined behaviour;
 *   - pi_factory_create_instance()'s `host` argument is not AddRef'd by that
 *     call, so wrapping the value you passed in is your own reference, not a new
 *     one.
 *
 * Copying is deleted on purpose: copying a refcounted handle has to AddRef, and
 * an implicit copy is the classic way to end up with an unbalanced pair. Say
 * what you mean with PiPtr<T>::add_ref(p) for a second owning reference.
 * -------------------------------------------------------------------------- */
template <typename T>
class PiPtr {
public:
    PiPtr() noexcept : m_ptr(nullptr) {}

    /* ADOPT one reference (does NOT AddRef).
     *
     * Pairing form for the framework contract: the pointer you just received
     * from pi_query_interface / pi_module_get_factory / pi_factory_create_instance
     * / pi_plugin_get_view / pi_host_services_create_default is already a
     * reference of your own. Handing it over here transfers that reference, so
     * there is nothing left to release by hand.
     *
     * If the constructor AddRef'd instead, every call site would need a matching
     * manual release of the raw pointer - which is exactly the bug this header
     * exists to remove. */
    explicit PiPtr(T* ptr) noexcept : m_ptr(ptr) {}

    PiPtr(PiPtr&& other) noexcept : m_ptr(other.m_ptr) { other.m_ptr = nullptr; }

    PiPtr& operator=(PiPtr&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_ptr = other.m_ptr;
            other.m_ptr = nullptr;
        }
        return *this;
    }

    PiPtr(const PiPtr&) = delete;
    PiPtr& operator=(const PiPtr&) = delete;

    ~PiPtr() { reset(); }

    /* Take a SECOND owning reference to an interface you only borrowed. */
    static PiPtr add_ref(T* ptr) noexcept
    {
        if (ptr) pi_iunknown_add_ref((IPiUnknown*)ptr);
        return PiPtr(ptr);
    }

    T* get() const noexcept { return m_ptr; }
    T* operator->() const noexcept { return m_ptr; }
    explicit operator bool() const noexcept { return m_ptr != nullptr; }
    bool is_valid() const noexcept { return m_ptr != nullptr; }

    /* Out-parameter slot for the C API ("receive"): releases what we hold first,
     * then hands over the address. The frozen out-parameter rule says *out is
     * NULL on failure, so a failed call leaves this empty rather than dangling. */
    T** put() noexcept
    {
        reset();
        return &m_ptr;
    }

    /* Give the reference back to the caller; it must release it. */
    T* detach() noexcept
    {
        T* ptr = m_ptr;
        m_ptr = nullptr;
        return ptr;
    }

    void reset() noexcept
    {
        if (m_ptr) {
            pi_iunknown_release((IPiUnknown*)m_ptr);
            m_ptr = nullptr;
        }
    }

    /* QueryInterface to another interface; the returned handle owns the
     * reference the framework hands out, or is empty when the query fails:
     *
     *     PiPtr<IPiHostUI> ui = host.qi_to<IPiHostUI>();
     *     if (ui) { ... }
     *
     * The no-argument form uses PiIidOf<U>; the other takes the IID explicitly
     * (app-defined interfaces that have no trait specialisation yet). */
    template <typename U>
    PiPtr<U> qi_to() const noexcept
    {
        return qi_to<U>(PiIidOf<U>::get());
    }

    template <typename U>
    PiPtr<U> qi_to(const PiGuid& iid) const noexcept
    {
        void* out = nullptr;
        if (!m_ptr) return PiPtr<U>();
        if (PI_FAILED(pi_iunknown_query_interface((IPiUnknown*)m_ptr, &iid, &out))) return PiPtr<U>();
        return PiPtr<U>(static_cast<U*>(out));
    }

private:
    T* m_ptr;
};

/* --------------------------------------------------------------------------
 * PiUniqueModule - RAII for a loaded plugin module
 *
 *      PiUniqueModule module;
 *      if (PI_FAILED(PiUniqueModule::load("my_plugin.dll", module))) { ... }
 *      PiPtr<IPiPluginFactory> factory = module.factory();
 *
 * The order still matters and this class cannot enforce it for you: every
 * plugin instance, view and factory must be gone BEFORE the module is unloaded,
 * or their vtables point into unmapped code. Member declaration order in your
 * host is usually enough (declare the module before the things loaded from it).
 * -------------------------------------------------------------------------- */
class PiUniqueModule {
public:
    PiUniqueModule() noexcept : m_module(nullptr) {}

    /* ADOPT a module returned by pi_module_load(). */
    explicit PiUniqueModule(PiPluginModule* module) noexcept : m_module(module) {}

    ~PiUniqueModule() { reset(); }

    PiUniqueModule(PiUniqueModule&& other) noexcept : m_module(other.m_module)
    {
        other.m_module = nullptr;
    }

    PiUniqueModule& operator=(PiUniqueModule&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_module = other.m_module;
            other.m_module = nullptr;
        }
        return *this;
    }

    PiUniqueModule(const PiUniqueModule&) = delete;
    PiUniqueModule& operator=(const PiUniqueModule&) = delete;

    /* Load + adopt in one step. Returns the framework result code instead of
     * throwing, so it stays usable in code that is built without exceptions. */
    static PiResult load(const char* path, PiUniqueModule& out) noexcept
    {
        out.reset();
        PiPluginModule* module = pi_module_load(path);
        if (!module) return PI_E_NOTFOUND;
        out.m_module = module;
        return PI_OK;
    }

    PiPluginModule* get() const noexcept { return m_module; }
    explicit operator bool() const noexcept { return m_module != nullptr; }

    /* Module factory, or an empty handle. */
    PiPtr<IPiPluginFactory> factory() const noexcept
    {
        IPiPluginFactory* f = nullptr;
        if (!m_module) return PiPtr<IPiPluginFactory>();
        if (PI_FAILED(pi_module_get_factory(m_module, &f))) return PiPtr<IPiPluginFactory>();
        return PiPtr<IPiPluginFactory>(f);
    }

    /* Unload the module we hold and adopt `module` instead (default: nothing). */
    void reset(PiPluginModule* module = nullptr) noexcept
    {
        if (m_module) {
            pi_module_unload(m_module);
            m_module = nullptr;
        }
        m_module = module;
    }

    /* Give the module back to the caller; it must unload it. */
    PiPluginModule* detach() noexcept
    {
        PiPluginModule* module = m_module;
        m_module = nullptr;
        return module;
    }

private:
    PiPluginModule* m_module;
};

#endif /* PI_PLUGIN_CPP_H */
