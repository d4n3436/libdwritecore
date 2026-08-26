#!/usr/bin/env python3
"""
A window manager with exactly the two behaviors a headless capture needs.

    Xvfb :99 -screen 0 2560x1440x24 &
    tools/testing/minwm.py :99 &

Why this exists: on a bare X server with no window manager, Firefox's window is
never mapped and nothing is painted, so a root-window screenshot comes back a
uniform rectangle - and WebDriver:SetWindowRect is a resize *request* that
something has to honor, so the viewport never reaches the size the comparison
was asked for. Installing a real desktop environment to get those two
behaviors is a large dependency for forty lines of X protocol.

So: map what asks to be mapped, grant every configure request verbatim, and
announce a window manager so GTK stops waiting for one. Nothing else - no
decorations, no focus policy, no stacking rules.

Two more things the same capture needs, which belong in the launch environment
rather than here:

    env -u WAYLAND_DISPLAY GDK_BACKEND=x11 MOZ_ENABLE_WAYLAND=0 DISPLAY=:99 ...

Without them, a Firefox started from inside a Wayland session connects to that
session instead of the X display, and the screenshots come back empty while a
window nobody asked for opens on the real desktop.

Needs python-xlib.
"""

import sys

from Xlib import X, Xatom, display


def main():
    d = display.Display(sys.argv[1] if len(sys.argv) > 1 else None)
    root = d.screen().root
    atom = d.get_atom

    # Announce a window manager. GTK looks for this before it will treat the
    # display as managed.
    check = root.create_window(-100, -100, 1, 1, 0, X.CopyFromParent,
                               X.InputOnly, X.CopyFromParent)
    check.change_property(atom("_NET_SUPPORTING_WM_CHECK"), Xatom.WINDOW, 32, [check.id])
    check.change_property(atom("_NET_WM_NAME"), atom("UTF8_STRING"), 8, b"minwm")
    root.change_property(atom("_NET_SUPPORTING_WM_CHECK"), Xatom.WINDOW, 32, [check.id])
    root.change_property(atom("_NET_SUPPORTED"), Xatom.ATOM, 32,
                         [atom(n) for n in ("_NET_SUPPORTING_WM_CHECK", "_NET_WM_NAME",
                                            "_NET_WM_STATE", "_NET_ACTIVE_WINDOW",
                                            "_NET_WM_STATE_FULLSCREEN")])
    if d.get_selection_owner(atom("WM_S0")) == X.NONE:
        check.set_selection_owner(atom("WM_S0"), X.CurrentTime)

    root.change_attributes(event_mask=X.SubstructureRedirectMask | X.SubstructureNotifyMask)
    d.sync()

    while True:
        event = d.next_event()
        if event.type == X.MapRequest:
            event.window.map()
            try:
                event.window.set_input_focus(X.RevertToParent, X.CurrentTime)
                root.change_property(atom("_NET_ACTIVE_WINDOW"), Xatom.WINDOW, 32,
                                     [event.window.id])
            except Exception:
                pass                      # a window that vanished mid-map
        elif event.type == X.ConfigureRequest:
            # Granted as asked. A window manager is entitled to override a
            # client's geometry; one used for measurement is not.
            fields = {}
            mask = event.value_mask
            if mask & X.CWX: fields["x"] = event.x
            if mask & X.CWY: fields["y"] = event.y
            if mask & X.CWWidth: fields["width"] = event.width
            if mask & X.CWHeight: fields["height"] = event.height
            if mask & X.CWBorderWidth: fields["border_width"] = event.border_width
            if mask & X.CWSibling: fields["sibling"] = event.above
            if mask & X.CWStackMode: fields["stack_mode"] = event.stack_mode
            try:
                event.window.configure(**fields)
            except Exception:
                pass
        d.flush()


if __name__ == "__main__":
    main()
