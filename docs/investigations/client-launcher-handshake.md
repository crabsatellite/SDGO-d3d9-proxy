# Client exits before displaying a window

The reported quick-launch failure loaded all enabled Mods, then exited before NPC
and capsule data registration. No new Windows fault report was produced.

In the pinned client, WinMain at `0x004914D0` opens the named event `9YOU SDGO`
at `0x00491521`. If it is missing, `0x00491529` takes the early return with exit
code zero. The success path only signals the event and closes the native handle.
It is launcher coordination, not a login or account check.

A direct isolated launch without the event reproduced this early exit. With the
event held open, both the ordinary and enhanced-AI configurations registered the
owner-provided data and remained alive for 35 seconds. Those tests do not establish
the original launcher's exact timeout or prove a new gameplay session.

The loader now creates or retains this event before synchronous Mod initialization
for `GOnline-54BE1568-00C97000`, and releases its own handle when native WinMain has
opened its handle (or on shutdown if the native handoff never happens). Existing
event state, reset mode and security are preserved. The native executable/assets,
login protocol, UAC policy and server startup rules are unchanged. Other executable
build identities do not create an event.

The AI incremental package now carries and installs its matching loader and core,
preserving user configuration and rejecting a foreign loader before writing files.
This prevents an AI-only update from leaving the startup fix out of the install.

Validation includes real Win32 event lifetime/cleanup tests, loader forwarding,
installer preservation, pinned native instruction checks and isolated startup.
