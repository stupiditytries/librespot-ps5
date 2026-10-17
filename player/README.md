# Music Player

Borealis folder app for the modular receiver. The app only renders and sends commands;
Spotify authentication, metadata, caches and playback remain in the receiver.
The PS5 app does not import AudioOut.

## Build

Set these paths in the environment or the ignored receiver `.local/build-env.sh`:

- `PS5_TEMPLATE`: ps5-native-app-boilerplate checkout
- `PS5_PAYLOAD_SDK`: its native ps5-payload-sdk
- `GL_SDK`: PS5 OpenGL SDK `sdk` directory
- `PLAYER_BOREALIS_DIR`: Borealis `library` directory
- `PS5_FIRMWARE_MODULES`: private firmware modules used to verify the PCM ownership plan

Run `bash build-modular.sh` from the receiver root. It builds and signs folder app
`PPSA99730`, embeds its files, then builds `build/librespot-ps5-modular.elf`.
`PLAYER_WORK` and `WORK` optionally select ignored build directories.
The payload installs/updates only `/data/homebrew/PPSA99730`. The app and installer
share a file lock. A running app can be updated through individual atomic file
replacements; close and reopen it to load the new app and assets.
A matching manifest is accepted only if its files still exist with their expected sizes.
Launch the folder app through the existing ShadowMountPlus workflow.

For the Windows preview, also set `PLAYER_GLFW_DIR` and `PLAYER_STB_DIR`, then use
CMake with Ninja. Run `player_preview <output-directory> --mock` for sample data
or `--live` for the loopback receiver. The PS5 app always uses the real channel.
Search typing uses the console IME dialog.

## Control and art

The socket worker connects to `127.0.0.1:38574`; rendering consumes snapshots.
Version 1 remains available. Additive version 2 supplies track/context identities,
large art, explicit/liked flags, richer list headers and rows, optional play targets,
search, save acknowledgements and small `cover_ready` updates.

`open_list{uri,offset}` in version 2 returns 30 rows with `offset,next,count`.
The picker requests another page when selection approaches the last ten loaded rows.
Old responses for another list or search id are ignored. The displayed position
uses the receiver's heard-time timeline, including delayed changes.

Art is fetched only from the receiver's loopback cache proxy on port 38577, decoded
off the render thread, resized, then uploaded at most twice per frame. Text
measurements are cached. GPU art has a 64 MiB eviction budget and a 150 ms reveal.
The supplied green music-note icon has a black margin; `tools/render-icon.py` reproduces it deterministically from `assets/icon-source.png`.

## Verification and current limits

`tests/link.cpp` exercises real protocol messages without a receiver, including
heard-time changes, stale replies, paging, queue identity and save errors.
The built app and receiver still require manual console testing together.
In particular, opening a foreground app must be checked for uninterrupted background
PCM, loopback reachability and IME behavior. Search query hashes can expire; errors
are surfaced and the existing shared quota gate protects the fallback.

Lyrics and silent Canvas playback are implemented. Canvas keeps its last texture
across library/lyrics transitions; only still backgrounds use a cached blur.
Long Now Playing titles wrap to two lines. Search song rows are text-only.
Track rows in both search and detail views provide a short press highlight.

The PS5 build writes bounded performance summaries every five seconds to
`/download0/player-perf.log` (rotated at 2 MiB). Frame percentiles, late frames,
input-to-present delay, model snapshots, UI CPU work, image/Canvas work and present
wait are recorded without account names or track text. These are CPU/wall timers,
not GPU timestamps. The summaries overlap and should not be added together.
Console performance still needs a listening/scrolling test of each candidate.
Playlist creation year is omitted when the client protocol supplies no reliable date.
Long lists can page only through tracks returned by the client context; upstream
context continuation beyond those returned tracks still needs validation.
The app prefers v2 library metadata and small cover updates; full snapshot progress
messages are retained only for legacy clients. Library window paging and upstream
context continuation remain follow-up work.

## Credits

PS5 display, keyboard, native runtime and compatibility support are adapted from
[StremiBrew](https://github.com/stupiditytries/StremiBrew).
Its MIT license is retained in `ps5/STREMIBREW-LICENSE.md`.
Borealis and its dependencies retain their upstream licenses.
Plus Jakarta Sans fonts are bundled under the SIL Open Font License in assets/fonts.
