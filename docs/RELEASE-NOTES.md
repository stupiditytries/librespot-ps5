librespot-ps5 2.0: Spotify Connect on a modified PS5, now without the USB drive, with a control centre over your games and an app for your library.

## Compatibility

**Tested on FW 13.20, other firmware untested.** To play over games, the receiver reads a few values from inside the console's own music service, at positions found in the 13.20 version of it. Another firmware can move those around, so before touching anything the receiver checks that the music service matches the one it knows. If it doesn't, it safely refuses to play instead of risking a crash, so it's safe to give it a go on other firmware.

Want your firmware supported? You'll need a PS5 on that firmware with FTP access; see [Firmware support](https://github.com/stupiditytries/librespot-ps5/blob/main/FIRMWARE-SUPPORT.md) and contact me.

## Download

Get **`librespot-ps5-v2.0.0.zip`** below. It has both programs you need:

- `kabuki-ps5.elf`: [Kabuki](https://github.com/stupiditytries/kabuki-ps5), the control centre (see its [releases](https://github.com/stupiditytries/kabuki-ps5/releases)).
- `librespot-ps5.elf`: the Spotify receiver.

## How to use it

1. Unzip and put both ELFs on a USB drive (or wherever your payload loader loads from).
2. Load **`kabuki-ps5.elf` first**, then **`librespot-ps5.elf`**.
3. In the Spotify app on your phone, pick **PS5 Connect**. The first time, this also signs the console in.
4. Hold **L3 + R3** to open Kabuki: Now Playing (with scrubbing), Queue and Library, over a game or the home screen.

The receiver also installs the **librespot-ps5** app (`PPSA99730`) into `/data/homebrew`. Mount it like your other folder apps to browse your library, search, and see lyrics and Canvas videos.

If you are coming from a development build, restart the console first.

## What's new

- No more USB drive, Streamer track or 14 seconds of silence: the music goes straight to the console's audio.
- Kabuki controls over your game: play/pause, skip, volume, scrubbing, queue and library.
- The librespot-ps5 app: library, albums, playlists, search, liked songs.
- Cast once and the console remembers the login; after that you can play from the console without your phone.
- Synced lyrics. WIP: songs without synced lyrics show *Lyrics unavailable*.
- Canvas videos, song notifications, correct song when tapping in a playlist on the phone, and better behaved library requests.

## Known issues

- Only tested on one console (FW 13.20).
- librespot-ps5 needs Kabuki running; it refuses to play without it.
- Close the librespot-ps5 app before loading a new receiver, and never run two receivers at once.

`SHA256SUMS.txt` lists the checksums of the ZIP and the files in it. `BUILD-INFO.txt` records the source revision the receiver was built from. The receiver was built by GitHub Actions from the same source as this release's tag; Kabuki is the unchanged file from its release.

Not affiliated with or endorsed by Spotify or Sony.
