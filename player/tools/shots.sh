#!/bin/bash
# Renders the mock-up set into out/ (each shot is its own run so animations start settled).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
out=${1:-out}
p() { bash tools/preview.sh "$out" "$@" 2>&1 | grep -v "gamepad db" || true; }
p --sample playing wait=900 shot=01-tint-playing
p --sample paused  wait=900 shot=02-tint-paused
p --sample playing --bg canvas wait=900 shot=03-canvas-playing
p --sample long wait=900 shot=04-long-text
p --sample shuffle-on wait=900 shot=05-shuffle-repeat-pending
p --sample nolyrics wait=900 shot=06-no-lyrics-no-canvas
# focus states, both looks
p --sample playing --focus fill right wait=700 shot=10-focus-fill-next
p --sample playing --focus fill left left left wait=700 shot=11-focus-fill-heart
p --sample playing --focus fill right right right right wait=700 shot=12-focus-fill-queue-pill
p --sample playing --focus fill up wait=700 shot=13-focus-fill-scrub
p --sample playing --focus fill up right right right wait=700 shot=14-scrubbing
p --sample playing --focus ring right wait=700 shot=15-focus-ring-next
p --sample playing --focus ring wait=700 shot=16-focus-ring-play
p --sample playing --focus ring right right right right wait=700 shot=17-focus-ring-queue-pill
p --sample playing --bg canvas --focus fill right right right right wait=700 shot=18-canvas-focus-queue-pill
# queue card
p --sample playing --bg canvas right right right right a wait=900 shot=20-queue-canvas
p --sample playing --focus fill right right right right a down down down wait=900 shot=21-queue-selected-fill
p --sample playing --focus ring right right right right a down down down wait=900 shot=22-queue-selected-ring
# lyrics
p --sample playing y wait=1200 shot=30-lyrics-start
p --sample playing y wait=1200 shot=31-lyrics-synced --live
p --sample playing --bg canvas y wait=1200 shot=32-lyrics-from-canvas
# status screens
p --sample idle wait=600 shot=40-idle
p --sample kabuki wait=600 shot=41-waiting-kabuki
p --sample offline wait=600 shot=42-offline
# controls auto-hide (hide-after 2 s so the shots are quick): mid-fade, then hidden
p --sample playing --bg canvas --hide-after 2 wait=2400 shot=50-hide-mid-fade
p --sample playing --bg canvas --hide-after 2 wait=4500 shot=51-hidden-canvas
p --sample playing --hide-after 2 wait=4500 shot=52-hidden-tint
p --sample long --hide-after 2 wait=4500 shot=53-hidden-long-title
p --sample playing --hide-after 2 y wait=4500 shot=54-hidden-lyrics
p --sample playing --hide-after 2 wait=4500 right wait=700 shot=55-woken-by-dpad
p --sample playing wait=600 shot=62-heart-liked
p --sample nolyrics wait=600 shot=63-heart-unliked
# the library page under the player: down from the controls scrolls to it
p --sample playing down wait=1200 shot=70-library-playlists
p --sample playing down right right wait=1200 shot=71-library-playlists-focus3
p --sample playing down down wait=1200 shot=72-library-albums
p --sample playing down right right right right right right right right right right right wait=1200 shot=74-library-more-tile
p --sample playing --bg canvas down wait=300 shot=75-library-scrolling
p --sample playing y down wait=1200 shot=76-library-from-lyrics
# search bar and the song select screen
p --sample playing down up wait=1200 shot=80-search-focus
p --sample playing down right a wait=1500 shot=81-playlist-open
p --sample playing down right a right down down wait=1500 shot=82-playlist-song-focus
p --sample playing down down a wait=1500 shot=83-album-open
p --sample playing down down a right right down down down wait=1500 shot=84-album-song-focus
p --sample playing down a wait=1500 shot=85-liked-songs-open
# the now-playing chip is selectable: from the library via Search, from an open list via Up
p --sample playing down up left wait=1200 shot=90-chip-focus-library
p --sample playing down right a up wait=1500 shot=91-chip-focus-detail
p --sample playing down right a up a wait=1500 shot=92-chip-back-to-player
# search: open it from the top bar, then the results, an album from them, and the top bar with a title kept
p --sample playing down up right a wait=1500 shot=94-search-results
p --sample playing down up right a down down wait=1500 shot=95-search-results-song-focus
p --sample playing down up right a right wait=1500 shot=96-search-results-album-focus
p --sample playing down up right a right a wait=1500 shot=97-search-album-open
p --sample playing down right a up right wait=1500 shot=98-search-focus-in-album
p --sample playing down right up wait=1500 shot=99-chip-keeps-heading
# search: the greyed loading screen with a wheel, then results; scrolled songs and albums
p --sample playing down up right a shot=100-search-loading wait=2000
p --sample playing down up right a wait=1700 down down down down down down down down wait=1800 shot=101-results-songs-scrolled
p --sample playing down up right a wait=1700 right down down down wait=1800 shot=102-results-albums-scrolled
