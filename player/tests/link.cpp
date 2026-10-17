// SPDX-License-Identifier: GPL-3.0-or-later
#include "link.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
int main() {
    player::Client client(false);
    auto list=[&](const std::string& line,std::uint64_t id=1){auto j=nlohmann::json::parse(line);j["client"]=client.client_id();j["id"]=id;client.line(j.dump());};
    client.line(R"({"v":2,"state":{"connected":true,"title":"One","uri":"spotify:track:a","cover":"small","cover_large":"large","duration_ms":200000,"position_ms":1000,"moving":false,"playing":true,"liked":false,"queue":[{"uri":"spotify:track:b","index":17,"title":"Two"}]}})");
    auto m=client.snapshot();
    assert(m.live&&m.track.uri=="spotify:track:a"&&m.track.cover=="large");
    assert(m.position_ms==1000&&m.queue[0].index==17);
    client.line(R"({"v":1,"state":{"title":"stale","position_ms":99999}})");
    assert(client.snapshot().track.title=="One");
    client.line(R"({"v":2,"state":{"connected":true,"title":"One","uri":"spotify:track:a","duration_ms":200000,"position_ms":1000,"moving":true,"playing":false,"changes":[{"in_ms":40,"position_ms":5000,"moving":false}]}})");
    std::this_thread::sleep_for(std::chrono::milliseconds(65));
    assert(client.snapshot().position_ms==5000);
    client.search("first");client.search("second");
    client.line(R"({"v":2,"search":{"id":1,"complete":true,"songs":[{"title":"stale"}]}})");
    assert(!client.snapshot().search.complete);
    client.line(R"({"v":2,"search":{"id":2,"query":"second","complete":true,"songs":[{"uri":"spotify:track:b","title":"Two","duration_ms":125000}],"albums":[]}})");
    assert(client.snapshot().search.complete&&client.snapshot().search.songs[0].duration_ms==125000);
    client.line(R"({"v":1,"library":{"items":[{"kind":"album","uri":"spotify:album:a","name":"Album"},{"kind":"playlist","uri":"spotify:playlist:p","name":"Playlist"},{"kind":"collection","uri":"spotify:user:u:collection","name":"Liked Songs"}]}})");
    m=client.snapshot();assert(m.albums.size()==1&&m.playlists.size()==2&&m.playlists[0].kind==player::Tile::Kind::liked);
    client.line(R"({"v":2,"library":{"items":[{"kind":"album","uri":"spotify:album:a","name":"Album","year":2024},{"kind":"playlist","uri":"spotify:playlist:p","name":"Playlist"},{"kind":"collection","uri":"spotify:user:u:collection","name":"Liked Songs"}]}})");
    client.line(R"({"v":1,"library":{"items":[]}})");
    m=client.snapshot();assert(m.albums.size()==1&&m.albums[0].year==2024);
    player::Model reused;client.snapshot_into(reused);const auto* storage=reused.playlists.data();
    client.snapshot_into(reused);assert(reused.playlists.data()==storage);
    client.send(player::Cmd::open_item,1000,m);
    list(R"({"v":2,"list":{"uri":"spotify:album:stale","name":"Wrong","items":[]}})");
    assert(client.snapshot().detail.title=="Album");
    list(R"({"v":2,"list":{"uri":"spotify:album:a","kind":"album","name":"Album","owner":"Artist","year":2024,"saved":false,"count":2,"next":1,"offset":0,"items":[{"uri":"spotify:track:a","title":"One","duration_ms":200000,"explicit":true}]}})");
    m=client.snapshot();assert(m.detail.owner=="Artist"&&m.detail.playing==0&&m.detail.songs[0].explicit_);
    client.line(R"({"v":2,"cover_ready":{"uri":"spotify:track:a","cover":"cached.jpg"}})");
    assert(client.snapshot().detail.songs[0].cover=="cached.jpg");
    client.line(R"({"v":2,"saved":{"uri":"spotify:track:a","on":true,"error":"rejected"}})");
    assert(!client.snapshot().liked);
    client.line(R"({"v":2,"saved":{"uri":"spotify:track:a","on":true,"error":""}})");
    assert(client.snapshot().liked);
    client.send(player::Cmd::load_more,0,client.snapshot());
    list(R"({"v":2,"list":{"uri":"spotify:album:a","kind":"album","name":"Album","offset":1,"next":2,"count":2,"items":[{"uri":"spotify:track:b","title":"Two"}]}})",2);
    assert(client.snapshot().detail.songs.size()==2);
    list(R"({"v":2,"list":{"uri":"spotify:album:a","offset":1,"items":[{"title":"duplicate"}]}})",2);
    assert(client.snapshot().detail.songs.size()==2);
    // Same URI reopened: an old reply cannot overwrite the new selection.
    client.send(player::Cmd::open_item,1000,client.snapshot());
    list(R"({"v":2,"list":{"uri":"spotify:album:a","name":"Stale title","items":[]}})",1);
    assert(client.snapshot().detail.loading);
    list(R"({"v":2,"list":{"uri":"spotify:album:a","name":"Fresh title","owner":"Owner","count":0,"items":[]}})",3);
    assert(!client.snapshot().detail.loading&&client.snapshot().detail.title=="Fresh title");
    client.search("");assert(client.snapshot().search.complete);
    // Search remembers nothing: opening it, or leaving its results, clears the query and results.
    client.search("remembered query");assert(client.snapshot().search.query=="remembered query");
    client.send(player::Cmd::open_search,0,client.snapshot());
    {auto m=client.snapshot();assert(m.search.query.empty()&&m.search.songs.empty()&&m.search.albums.empty()&&!m.search.complete);}
    client.search("another");client.send(player::Cmd::close_search,0,client.snapshot());
    {auto m=client.snapshot();assert(m.search.query.empty()&&m.search.complete);}
    client.line(R"({"v":2,"state":{"connected":true,"uri":"spotify:track:c","media_session":7,"media_generation":8,"features":{"lyrics":true,"canvas":true}}})");
    m=client.snapshot();assert(m.link==player::Link::connected&&m.lyrics_supported);
    client.send(player::Cmd::request_lyrics,0,m);
    auto feature=[&](int offset,bool complete,const char* words,int gen=8,const char* sync="line"){
        nlohmann::json j={{"v",2},{"client",client.client_id()},{"id",1},{"session",7},{"generation",gen},
            {"lyrics",{{"uri","spotify:track:c"},{"status","ready"},{"revision","fixture"},{"offset",offset},{"total",2},{"complete",complete},{"sync",sync},
                {"colors",{{"background",{0.,0.,0.}},{"text",{.4,.4,.4}},{"highlight",{1.,1.,1.}}}},
                {"lines",nlohmann::json::array({{{"start_ms",offset*1000},{"words",words}}})}}}};client.line(j.dump());};
    feature(0,false,"Old",7);assert(!client.snapshot().has_lyrics);
    feature(0,false,"First");assert(!client.snapshot().has_lyrics);
    feature(1,true,"Second");m=client.snapshot();assert(m.has_lyrics&&m.lyrics.lines.size()==2&&m.lyrics.lines[0].words=="First"&&m.lyrics.background.r==0);
    const auto lyric_revision=m.lyrics_revision;
    client.line(R"({"v":2,"state":{"connected":true,"uri":"spotify:track:c","media_session":7,"media_generation":8,"playing":false,"features":{"lyrics":true,"canvas":true}}})");
    m=client.snapshot();assert(m.has_lyrics&&m.lyrics_revision==lyric_revision); // Pause is not a new lyric document.
    feature(0,true,"Unsynced cache",8,"none");
    m=client.snapshot();assert(!m.has_lyrics&&m.lyrics.lines.empty()&&m.lyrics_status=="none");

    client.line(R"({"v":2,"state":{"connected":true,"uri":"spotify:track:d","media_session":7,"media_generation":9,"features":{"lyrics":true,"canvas":true}}})");
    assert(!client.snapshot().has_lyrics&&client.snapshot().lyrics.lines.empty());
    feature(0,true,"Stale after pause");assert(!client.snapshot().has_lyrics);

    nlohmann::json big=nlohmann::json::array();
    for(int i=0;i<671;++i)big.push_back({{"kind",i<87?"playlist":"album"},{"uri",std::string(i<87?"spotify:playlist:":"spotify:album:")+std::to_string(i)},{"name","Synthetic library item"}});
    client.line(nlohmann::json({{"v",2},{"library",{{"items",big},{"error",""}}}}).dump());
    client.snapshot_into(reused);assert(reused.playlists.size()==87&&reused.albums.size()==584);
    client.line(R"({"v":1,"library":{"items":[]}})");client.snapshot_into(reused);assert(reused.playlists.size()==87);
    client.line(R"({"v":2,"state":{"connected":true,"uri":"spotify:track:e","media_session":8,"media_generation":1}})");
    client.line(nlohmann::json({{"v",2},{"media_session",7},{"library",{{"items",big},{"error",""}}}}).dump());
    client.snapshot_into(reused);assert(reused.playlists.empty()&&reused.library_loading);
    std::cout<<"Live client identity, heard clock, stale replies, 671-item library and saved-state checks passed\n";
}
