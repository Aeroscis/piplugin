/*
 * piplugin - IPiUnknown (COM-style root interface)
 *
 * All framework interfaces extend IPiUnknown.
 * The vtable layout is a plain C struct of function pointers to guarantee
 * ABI stability across compilers and languages (Rust, C#, Java FFI).
 */
#ifndef PI_PLUGIN_UNKNOWN_H
#define PI_PLUGIN_UNKNOWN_H

#include "pi_plugin_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Calling convention: __stdcall on Windows for maximum FFI compatibility.
 * Must be defined BEFORE any vtable that uses it. */
#if defined(PI_PLATFORM_WINDOWS) && !defined(PI_CALL)
#  define PI_CALL __stdcall
#elif !defined(PI_CALL)
#  define PI_CALL
#endif

/* --------------------------------------------------------------------------
 * IPiUnknownVtbl - Virtual function table for IPiUnknown
 * --------------------------------------------------------------------------
 * C objects that implement IPiUnknown contain:
 *   struct { const IPiUnknownVtbl* lpVtbl; ... };
 *
 * This is the same layout that C++ compilers generate for single-inheritance
 * vtables, but expressed in plain C so it is stable across compilers and
 * usable from any language that can represent structs of function pointers.
 */
typedef struct IPiUnknownVtbl {
    PiResult (PI_CALL *pi_query_interface)(void* this_ptr, const PiGuid* iid, void** out);

    uint32_t (PI_CALL *pi_add_ref)(void* this_ptr);

    uint32_t (PI_CALL *pi_release)(void* this_ptr);
} IPiUnknownVtbl;

/* --------------------------------------------------------------------------
 * IPiUnknown - Base interface object (must be complete struct, not just
 *              forward-declared, because PiRefCountedBase embeds it by value)
 * -------------------------------------------------------------------------- */
typedef struct IPiUnknown {
    const IPiUnknownVtbl* lpVtbl;
} IPiUnknown;

/* --------------------------------------------------------------------------
 * Inline helpers - safe wrappers that check for NULL
 * -------------------------------------------------------------------------- */
static inline PiResult pi_iunknown_query_interface(IPiUnknown* self, const PiGuid* iid, void** out) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_query_interface) return PI_E_INVALIDARG;
    return self->lpVtbl->pi_query_interface((void*)self, iid, out);
}

static inline uint32_t pi_iunknown_add_ref(IPiUnknown* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_add_ref) return 0;
    return self->lpVtbl->pi_add_ref((void*)self);
}

static inline uint32_t pi_iunknown_release(IPiUnknown* self) {
    if (!self || !self->lpVtbl || !self->lpVtbl->pi_release) return 0;
    return self->lpVtbl->pi_release((void*)self);
}

/* --------------------------------------------------------------------------
 * Reference-counted base implementation (for plugin authors to reuse)
 * --------------------------------------------------------------------------
 * Embed PiRefCountedBase as the FIRST member of your object struct and
 * assign the vtable.
 *
 * Lifetime: when the refcount drops to zero, the optional `destroy`
 * callback is invoked with the object pointer. For plain C objects that
 * were allocated with the host allocator, set destroy to a function that
 * frees the object. For C++ classes, set it to a static thunk calling
 * `delete (MyClass*)ptr;` so destructors run. If destroy is NULL the
 * object is left alone (static/singleton-style objects).
 *
 * Example:
 *   typedef struct { PiRefCountedBase base; ... } MyPlugin;
 */
typedef void (*PiDestroyProc)(void* self);

typedef struct PiRefCountedBase {
    IPiUnknown          unk;
    volatile uint32_t   ref_count;
    PiDestroyProc       destroy;   /* called (once) when refcount hits 0 */
} PiRefCountedBase;

/* Initialize the ref-counted base (destroy = NULL). Call from your
 * constructor. `vtbl` must have its pi_add_ref / pi_release slots pointed
 * at pi_refcounted_add_ref / pi_refcounted_release. */
PI_EXPORT void pi_refcounted_init(PiRefCountedBase* base, const IPiUnknownVtbl* vtbl);

/* Same, with a destroy callback for when the refcount reaches zero. */
PI_EXPORT void pi_refcounted_init_with_destroy(PiRefCountedBase* base,
                                                const IPiUnknownVtbl* vtbl,
                                                PiDestroyProc destroy);

/* Default pi_add_ref / pi_release implementations for PiRefCountedBase */
PI_EXPORT uint32_t PI_CALL pi_refcounted_add_ref(void* this_ptr);
PI_EXPORT uint32_t PI_CALL pi_refcounted_release(void* this_ptr);

#ifdef __cplusplus
}
#endif

#endif /* PI_PLUGIN_UNKNOWN_H */
