// What the player screens show and what they ask for.
//
// On the console this is the control client's snapshot of librespot-ps5 (plan section 5,
// link/). The PC preview fills it with invented sample data. Nothing here knows about
// drawing.

#pragma once

#include <functional>
#include <cstdint>
#include <string>
#include <vector>

namespace player
{

enum class Repeat { off, all, one };

// librespot's link, as the app sees it. `waiting_kabuki` is librespot's
// `"waiting_for":"kabuki"` state.
enum class Link { connected, idle, waiting_kabuki, offline };

struct Rgb
{
    float r = 0, g = 0, b = 0; // 0..1
};

struct Track
{
    std::string title;
    std::string artist;
    std::string cover; // image file (cover_large); empty: the preview paints a sample cover
    Rgb cover_colour;  // dominant colour of the cover, for the fallback background
    int duration_ms = 0;
    bool explicit_ = false;
    std::string uri;
    std::string background; // the artist's wide picture; empty: the cover stands in
};

struct QueueEntry
{
    std::string title;
    std::string artist;
    Rgb cover_colour;
    std::string cover;
    std::string uri; int index=0;
};

struct LyricLine
{
    int start_ms = 0;
    std::string words;
};

// Spotify's lyrics response, as librespot would pass it on (plan section 4).
struct Lyrics
{
    enum class Sync { none, line, syllable } sync = Sync::none;
    std::vector<LyricLine> lines;
    Rgb background;  // the colour Spotify pairs with the track
    Rgb text;        // lines not yet sung
    Rgb highlight;   // lines sung and the line being sung
    std::string provider;
};

// A saved playlist or album in the library page (a "More" tile is added to each row).
struct Tile
{
    enum class Kind { playlist, album, liked } kind = Kind::playlist;
    std::string name;
    Rgb colour; // the cover's colour, for the painted sample cover
    std::string cover;
    std::string sub;
    int year=0;
    std::string uri; // a line under the tile in search results, such as "The Nightjars - 2019"
};

// A track in an open album or playlist.
struct Song
{
    std::string title;
    std::string artist;
    int duration_ms = 0;
    bool explicit_ = false;
    Rgb colour;
    std::string uri,cover; // for the painted sample cover in a playlist row
};

// The open album or playlist (the song select screen). librespot would fill this on open_item.
struct Detail
{
    Tile::Kind kind = Tile::Kind::playlist;
    std::string title;
    std::string owner; // the creator or the artist
    std::string meta;  // "Playlist - 24 songs"
    std::string description;
    Rgb colour;
    std::string cover;
    bool saved = true; // in the library
    int playing = -1;  // the song of this list that is playing now
    std::vector<Song> songs;
    std::string uri;
    bool loading=false,more_loading=false;
    std::string error;
    int count=0;std::string metadata_error;
};

// What a search found: songs and albums (librespot would fetch these).
struct SearchResults
{
    std::string query;
    std::vector<Song> songs;
    std::vector<Tile> albums;
    bool complete=false; std::string error;
};

struct Model
{
    Link link = Link::connected;
    std::string device; // the Connect name shown on the idle screen

    Track track;
    std::string context_kind; // "ALBUM", "PLAYLIST", "ARTIST"...
    std::string context_name;
    int position_ms = 0; // heard position
    bool playing = false;
    bool pending = false; // a seek or a start not heard yet: buffering light
    bool shuffle = false;
    Repeat repeat = Repeat::off;
    bool liked = false;

    bool has_canvas = false; // the track has a Canvas video (the preview draws a still)
    std::string canvas_still; // an image standing in for one frame of it

    std::vector<QueueEntry> queue;
    // The library page under the player: Liked Songs and playlists, albums.
    std::vector<Tile> playlists, albums;
    Detail detail;
    SearchResults search;
    bool lyrics_supported=false,canvas_supported=false;
    std::string lyrics_status="unknown",lyrics_error,canvas_status="unknown",canvas_error,canvas_path,canvas_kind;
    bool has_lyrics = false;
    Lyrics lyrics;
    std::string auth_status,storage_status;
    bool live=false;
    bool moving=false,library_loading=true;
    std::string library_error,notice;
    std::uint64_t revision=0;
    std::uint64_t lyrics_revision=0,media_session=0,media_generation=0;
};

enum class Cmd { play_pause, previous, next, shuffle, repeat, like, seek, play_queued,
           open_item /* section * 1000 + column */, play_song /* index in the open list */, play_all, save_item,
           load_more, retry, refresh_library, request_lyrics, open_search, play_found /* index in the song results */, open_found_album /* index in the album results */, close_search };

struct App // commands go out through librespot only (hard rule 1)
{
    Model model;
    std::function<void(Cmd, int)> send;
    std::function<void()> update;
};

} // namespace player
