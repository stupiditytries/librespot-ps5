# GPL loader sources

These files preserve the upstream copyright notices and are covered by GPL-3.0-or-later.
The new bridge sources in the parent directory use the same license.

- https://github.com/erickdavestech/ps5-dualsense-overlay (Common FPS/SimpleFPS derivatives)
- https://github.com/ps5-payload-dev/shsrv (John Tornblom)

Pinned commits: {'pad-overlay-ref': 'e1de419707860a8da01d3604b0b5632771159756', 'shsrv-ref': '6f320637d56d344a0e7797753099e33238bbf146'}

The bridge's `prologue.hpp` is the reference's restrictive instruction decoder.
It rejects relative branches and RIP-relative instructions instead of relocating them.
No artwork, controller overlay, toast, monitor or autoload code is included.

Local change to `pt.c`: attempt restoration of both saved credentials on failure,
and check their readback after a ptrace call. The upstream blocking ptrace waits
remain; this is not a guaranteed bounded launch or recovery path.
