#!/usr/bin/env bash
# The X11 probe behind docs/todo/platform.md #5: is `unsigned long` the right
# Linux definition of PiNativeWindow, given that X11's `Window` is really an XID?
#
# It compiles a probe with the project's own C flags against the real Xlib
# headers of the machine it runs on, and asserts both directions of the
# conversion plus the size match. The conclusion is recorded in platform.md #5;
# this script is here so the next person who touches PiNativeWindow (FUT-01,
# X11 XEmbed) can re-run it instead of trusting a number in a document.
#
#   scripts/probe_x11_native_window.sh            # gcc
#   CC=clang scripts/probe_x11_native_window.sh   # any other compiler
#
# Exit code 0 = the current definition holds. 2 = no usable Xlib here (skip).
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")/.." && pwd)"
cc="${CC:-gcc}"

if ! printf '#include <X11/Xlib.h>\nint main(void) { return 0; }\n' \
        | "$cc" -x c - -o /dev/null 2>/dev/null; then
    echo "SKIP: '$cc' cannot use Xlib headers (install libx11-dev / libX11-devel)"
    exit 2
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat > "$work/probe.c" <<'EOF'
#include <X11/Xlib.h>
#include "piplugin/pi_plugin_types.h"
#include <stdio.h>

int main(void)
{
    Window xid = (Window)0x1234;
    PiNativeWindow w = (PiNativeWindow)xid; /* X11 -> framework */
    Window back = (Window)w;                /* framework -> X11 */

    printf("sizeof(Window)=%zu sizeof(XID)=%zu sizeof(PiNativeWindow)=%zu\n",
           sizeof(Window), sizeof(XID), sizeof(PiNativeWindow));
    printf("round trip: %s\n", (back == xid) ? "OK" : "BROKEN");
    printf("PI_INVALID_WINDOW=%lu  PI_IS_VALID_WINDOW(5)=%d\n",
           (unsigned long)PI_INVALID_WINDOW, PI_IS_VALID_WINDOW((PiNativeWindow)5));

    return (back == xid && sizeof(Window) == sizeof(PiNativeWindow)) ? 0 : 1;
}
EOF

echo "=== compiler ==="
"$cc" --version | head -1

echo "=== compile (the project's own flags) ==="
"$cc" -std=c11 -Wall -Wextra --pedantic-errors \
      -I "$root/include" "$work/probe.c" -o "$work/probe"
echo "compile exit=$?"

echo "=== run ==="
"$work/probe"
status=$?
echo "probe exit=$status"
exit $status
