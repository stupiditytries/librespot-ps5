// Measurements and colours for the player screens, in 1080p pixels (borealis is told the
// screen is 1920 x 1080; 4K is the same drawing scaled).
//
// Sources, until the full reference set exists (plan section 3.1):
//   [R1] reference/playerexample2.webp  1920x1080 capture: Now Playing, layout and type sizes
//   [R2] reference/playerexample3 ...jpg  photo of a TV: the control pill, the Lyrics / Queue
//        pills, the focused look, the queue card. A photo: proportions only, estimated to
//        about +-6 px
//   [R3] reference/queu.png, playerexample.png  small renders: queue card contents
// A number marked (est) is a design choice made without a capture.

#pragma once

namespace player::theme
{

constexpr float kW = 1920.0f;
constexpr float kH = 1080.0f;
constexpr float kMargin = 112.0f; // [R1] cover and left time start here; right edge is kW - this

// Header: "PLAYING FROM ..."
constexpr float kMarkX = 148.0f, kMarkY = 104.0f, kMarkR = 32.0f; // [R1]
constexpr float kContextX = 204.0f;                                 // [R1]
constexpr float kContextLabelBase = 96.0f, kContextLabelSize = 18.0f, kContextLabelTrack = 2.2f;
constexpr float kContextNameBase = 129.0f, kContextNameSize = 22.0f;

// Track block
constexpr float kCoverX = 112.0f, kCoverY = 600.0f, kCoverSize = 176.0f, kCoverRadius = 4.0f; // [R1]
constexpr float kTitleX = 336.0f, kTitleBase = 702.0f, kTitleSize = 64.0f;                    // [R1]
constexpr float kArtistBase = 745.0f, kArtistSize = 24.0f;                                    // [R1]

// Scrub bar
constexpr float kBarX0 = 196.0f, kBarX1 = 1724.0f, kBarY = 840.0f;                            // [R1]
constexpr float kBarH = 4.0f, kBarHFocus = 8.0f, kThumbR = 11.0f;
constexpr float kTimeBase = 849.0f, kTimeSize = 24.0f;                                         // [R1]
constexpr float kTimeLeftX = 110.0f;                                                           // [R1]

// Control row
constexpr float kRowY = 936.0f;                                                                // [R1]
constexpr float kPillHalfW = 236.0f, kPillH = 104.0f, kPillRadius = 18.0f;                     // [R2]
constexpr float kStep = 90.0f;                 // centre to centre inside the pill [R2]
constexpr float kPlayR = 32.0f;                // [R2]; [R1] shows 40 without the pill
constexpr float kIcon = 36.0f;                 // glyph box [R2]
constexpr float kHeartX = 166.0f, kHeartR = 32.0f; // [R1] centre 200 w/o pill; [R2] disc
constexpr float kPillBtnH = 64.0f, kPillBtnGap = 12.0f, kPillBtnIcon = 30.0f, kPillBtnFont = 22.0f; // [R2]
constexpr float kPillBtnPad = 22.0f, kPillBtnInner = 10.0f;

// Queue panel: the whole right side of the screen, full height ([R3] shows the
// same full-height treatment). The scrub bar and total time pull in to its left edge.
constexpr float kQueueX = 1240.0f, kQueuePad = 72.0f, kQueueHead = 32.0f;
constexpr float kQueueRow = 108.0f, kQueueArt = 84.0f, kQueueTop = 96.0f;

// Controls hide after a while: only a small cover, title and artist stay in the bottom-left corner.
constexpr float kCornerMargin = 72.0f, kCornerCover = 128.0f;
constexpr float kCornerTitleSize = 40.0f, kCornerArtistSize = 28.0f;

// Lyrics view (est): no capture yet
constexpr float kLyricsX = 112.0f, kLyricsW = 1696.0f, kLyricsTop = 150.0f, kLyricsBottom = 570.0f;
constexpr float kLyricsSize = 66.0f, kLyricsLine = 90.0f, kLyricsAnchor = 290.0f;
// With the controls hidden the lyrics claim the room the track block gave up.
constexpr float kLyricsExtraBottom = 280.0f, kLyricsExtraAnchor = 150.0f;

// Library page, below the player; [R4] is the tvOS Spotify app's Your Library screen (1920x1080
// from a 578 px capture, so positions are +-6 px): a small section label, the focused item's
// name as a big heading, a row of square tiles, the focused tile 15% larger with a white ring,
// and a "More" tile ending each row.
constexpr float kLibX = 112.0f;                          // the page margin: label, heading and first tile line up with the now-playing chip
constexpr float kTile = 230.0f, kTilePitch = 266.0f;     // [R4] 69 px tiles at 11 px gaps, x3.32
constexpr float kLibLabel = 1062.0f;                     // page y of the first label's baseline; peeks under the player
constexpr float kLibPitch = 595.0f;                      // [R4] label to label, row to row
constexpr float kLibStop = kLibLabel - 335.0f;           // scroll that puts the first label at y 335 [R4]
constexpr float kLibHeadingDy = 120.0f, kLibTilesDy = 209.0f; // [R4] label to heading baseline / tile tops
constexpr float kLibLabelSize = 34.0f, kLibHeadingSize = 72.0f;
constexpr float kTileRadius = 10.0f;

// Colours (sRGB 0..255)
constexpr int kGrey = 0xB3;      // secondary text: Spotify's #B3B3B3
constexpr int kGreen[3] = {0x1E, 0xD7, 0x60}; // active states only
} // namespace player::theme
