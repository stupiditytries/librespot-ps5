# Firmware support

librespot-ps5 v2 is tested on **FW 13.20**. Other firmware versions are untested.

To play over games, the receiver reads a few values from inside the console's own music service, at positions found in the 13.20 version of it. Another firmware can move those around, so if the music service isn't the one it knows, the receiver safely refuses to play instead of risking a crash. Adding a firmware means finding those positions again on a console running it.

## Want your firmware supported?

You'll need:

- A PS5 on the firmware version you want support for, able to load payloads.
- FTP access to that console from a PC.

Then contact me ([stupiditytries](https://github.com/stupiditytries)) with your firmware version, and I'll walk you through what's needed. Once it works, it works for everyone on that firmware :)
