// SPDX-License-Identifier: GPL-3.0-or-later
#include "link.hpp"
#include "perf.hpp"
#include <algorithm>
#include <cstring>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
static void close_socket(Socket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
static void close_socket(Socket s) { close(s); }
#endif
namespace player {
namespace {
using json = nlohmann::json;
std::string text(const json &j, const char *key) {
  auto it = j.find(key);
  return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}
int number(const json &j, const char *key, int fallback = 0) {
  auto it = j.find(key);
  return it != j.end() && it->is_number() ? it->get<int>() : fallback;
}
bool flag(const json &j, const char *key) {
  auto it = j.find(key);
  return it != j.end() && it->is_boolean() && it->get<bool>();
}
const json &array(const json &j, const char *key) {
  static const json empty = json::array();
  auto it = j.find(key);
  return it != j.end() && it->is_array() ? *it : empty;
}
Tile tile(const json &j) {
  Tile t;
  t.uri = text(j, "uri");
  t.year = number(j, "year");
  t.name = text(j, "name");
  t.cover = text(j, "cover");
  t.sub = text(j, "subtitle");
  if (t.sub.empty())
    t.sub = text(j, "artist");
  const auto kind = text(j, "kind");
  t.kind = kind == "album" || t.uri.rfind("spotify:album:",0)==0 ? Tile::Kind::album
           : kind == "collection" || t.uri.ends_with(":collection") ? Tile::Kind::liked
                                  : Tile::Kind::playlist;
  t.colour = {.18f, .20f, .23f};
  return t;
}
Song song(const json &j) {
  Song s;
  s.uri = text(j, "uri");
  s.title = text(j, "title");
  s.artist = text(j, "artist");
  s.duration_ms = number(j, "duration_ms");
  s.explicit_ = flag(j, "explicit");
  s.cover = text(j, "cover");
  s.colour = {.18f, .20f, .23f};
  return s;
}
} // namespace
Client::Client(bool connect) {
  client_tag_="app-"+std::to_string(Clock::now().time_since_epoch().count());
  model_.link = Link::offline;
  model_.live = true;
  model_.revision=1;
  if (connect)
    thread_ = std::thread([this] { run(); });
}
Client::~Client() {
  stop_ = true;
  if (thread_.joinable())
    thread_.join();
}
Model Client::snapshot() {
  Model out;snapshot_into(out);return out;
}
void Client::snapshot_into(Model& out) {
  perf::Scope snapshot_time(perf::Snapshot);
  std::unique_lock lock(mutex_);
  const auto now = Clock::now();
  if(detail_pending_&&now-detail_started_>std::chrono::seconds(15)){
    detail_pending_=false;model_.detail.loading=model_.detail.more_loading=false;
    model_.detail.error="This list took too long to load. Press Cross to retry.";++model_.revision;
  }
  if(!model_.search.complete&&!model_.search.query.empty()&&now-search_started_>std::chrono::seconds(15)){
    model_.search.complete=true;model_.search.error="Search took too long. Try again.";++model_.revision;
  }
  if(model_.lyrics_status=="loading"&&now-feature_started_>std::chrono::seconds(12)){
    model_.lyrics_status="error";model_.lyrics_error="Lyrics took too long. Close and reopen Lyrics to retry.";++model_.revision;
  }
  if(out.revision!=model_.revision)out=model_;
  if (!points_.empty()) {
    Point p = points_.front();
    for (const auto &point : points_)
      if (point.at <= now)
        p = point;
    const auto elapsed =
        p.moving ? std::max<std::int64_t>(
                       0, std::chrono::duration_cast<std::chrono::milliseconds>(
                              now - p.at)
                              .count())
                 : 0;
    out.moving=p.moving;
    out.position_ms = int(
        std::clamp<std::int64_t>(std::int64_t(p.position) + elapsed, 0,
                                 std::max(out.track.duration_ms, p.position)));
  }
}
void Client::command(json j) {
  std::lock_guard lock(mutex_); ++model_.revision;
  if (out_.size() < 128)
    out_.push_back(j.dump() + "\n");
}
void Client::search(const std::string &query) {
  if (query.find_first_not_of(" \t\r\n") == std::string::npos) {
    std::lock_guard lock(mutex_); ++model_.revision;
    model_.search.complete = true;
    model_.search.error.clear();
    return;
  }
  std::uint64_t id;
  {
    std::lock_guard lock(mutex_); ++model_.revision;
    id = ++search_id_;search_started_=Clock::now();
    model_.search.query = query;
    model_.search.complete = false;
    model_.search.error.clear();
  }
  command({{"v", 2}, {"cmd", "search"}, {"query", query}, {"id", id}});
}
void Client::send(Cmd cmd, int value, const Model &m) {
  json j = {{"v", 1}};
  switch (cmd) {
  case Cmd::play_pause:
    j["cmd"] = "play_pause";
    break;
  case Cmd::previous:
    j["cmd"] = "previous";
    break;
  case Cmd::next:
    j["cmd"] = "next";
    break;
  case Cmd::shuffle:
    j["cmd"] = "shuffle";
    j["on"] = value ? value == 2 : !m.shuffle; // 1 = off, 2 = on, 0 = toggle
    break;
  case Cmd::repeat:
    j["cmd"] = "repeat";
    j["mode"] = value ? (value == 2 ? "all" : value == 3 ? "one" : "off") // 1 off, 2 all, 3 one
                : m.repeat == Repeat::off   ? "all"
                : m.repeat == Repeat::all ? "one"
                                          : "off";
    break;
  case Cmd::seek:
    j["cmd"] = "seek";
    j["ms"] = std::max(value, 0);
    break;
  case Cmd::like:
    j = {{"v", 2}, {"cmd", "like"}, {"uri", m.track.uri}, {"on", !m.liked}};
    break;
  case Cmd::save_item:
    j = {{"v", 2},
         {"cmd", "save"},
         {"uri", m.detail.uri},
         {"on", !m.detail.saved}};
    break;
  case Cmd::play_queued:
    if (value < 0 || value >= int(m.queue.size()))
      return;
    j["cmd"] = "play_queued";
    j["uri"] = m.queue[std::size_t(value)].uri;
    j["index"] = m.queue[std::size_t(value)].index;
    break;
  case Cmd::refresh_library: {
    std::lock_guard lock(mutex_); ++model_.revision;model_.library_loading=true;model_.library_error.clear();
    out_.push_back("{\"v\":1,\"cmd\":\"library\"}\n");return;
  }
  case Cmd::request_lyrics: {
    std::lock_guard lock(mutex_); ++model_.revision;
    if(!model_.lyrics_supported||model_.track.uri.empty())return;
    model_.lyrics_status="loading";model_.lyrics_error.clear();model_.has_lyrics=false;lyric_pending_.clear();feature_revision_.clear();feature_started_=Clock::now();
    out_.push_back(json({{"v",2},{"cmd","lyrics"},{"client",client_tag_},{"id",++feature_id_},{"uri",model_.track.uri},
        {"session",media_session_},{"generation",media_generation_}}).dump()+"\n");return;
  }
  case Cmd::retry: {
    std::lock_guard lock(mutex_); ++model_.revision;
    detail_pending_=true;detail_started_=Clock::now();
    model_.detail.loading=model_.detail.songs.empty();model_.detail.more_loading=!model_.detail.songs.empty();
    model_.detail.error.clear();
    out_.push_back(json({{"v",2},{"cmd","open_list"},{"client",client_tag_},{"id",++detail_id_},{"uri",model_.detail.uri},{"offset",model_.detail.songs.empty()?0:detail_next_}}).dump()+"\n");
    return;
  }
  case Cmd::load_more: {
    std::lock_guard lock(mutex_); ++model_.revision;
    if (!detail_more_ || detail_pending_)
      return;
    detail_pending_ = true;detail_started_=Clock::now();model_.detail.more_loading=true;
    out_.push_back(json({{"v", 2},
                         {"cmd", "open_list"},{"client",client_tag_},{"id",++detail_id_},
                         {"uri", model_.detail.uri},
                         {"offset", detail_next_}})
                       .dump() +
                   "\n");
    return;
  }
  case Cmd::open_item:
  case Cmd::open_found_album: {
    const auto &items = cmd == Cmd::open_found_album ? m.search.albums
                        : value / 1000 == 0          ? m.playlists
                                                     : m.albums;
    const int at = cmd == Cmd::open_found_album ? value : value % 1000;
    if (at < 0 || at >= int(items.size()))
      return;
    const auto &item = items[std::size_t(at)];
    j["v"] = 2;
    j["cmd"] = "open_list";
    j["uri"] = item.uri;
    j["offset"] = 0;
    {
      std::lock_guard lock(mutex_); ++model_.revision;
      j["client"]=client_tag_;j["id"]=++detail_id_;
      detail_next_ = 0;
      detail_more_ = false;
      detail_pending_ = true;detail_started_=Clock::now();
      model_.detail = {};model_.detail.loading=true;
      model_.detail.uri = item.uri;
      model_.detail.title = item.name;
      model_.detail.cover = item.cover;
      model_.detail.kind = item.kind;
    }
    break;
  }
  case Cmd::play_song:
    if (value < 0 || value >= int(m.detail.songs.size()))
      return;
    j = {{"v", 2},
         {"cmd", "play"},
         {"context", m.detail.uri},
         {"track", m.detail.songs[std::size_t(value)].uri}};
    break;
  case Cmd::play_all:
    j = {{"v", 2}, {"cmd", "play"}, {"context", m.detail.uri}};
    break;
  case Cmd::play_found:
    if (value < 0 || value >= int(m.search.songs.size()))
      return;
    j = {{"v", 2},
         {"cmd", "play"},
         {"track", m.search.songs[std::size_t(value)].uri}};
    break;
  case Cmd::open_search:
  case Cmd::close_search: {
    // Nothing typed is remembered: opening Search, or leaving its results, starts clean. The
    // platform's keyboard supplies the next text to search(), so no search runs by itself.
    std::lock_guard lock(mutex_); ++model_.revision;
    ++search_id_; // a reply to the old query can no longer land
    model_.search.query.clear();
    model_.search.songs.clear();
    model_.search.albums.clear();
    model_.search.error.clear();
    model_.search.complete = cmd == Cmd::close_search;
    return;
  }
  }
  command(j);
}
void Client::line(const std::string &line) {
  const auto j = json::parse(line, nullptr, false);
  if (!j.is_object())
    return;
  std::lock_guard lock(mutex_); ++model_.revision;
  auto &m = model_;
  if(!j.contains("state")&&j.contains("media_session")&&j.value("media_session",std::uint64_t(0))!=media_session_)return;
  if (auto it = j.find("state"); it != j.end() && it->is_object()) {
    const bool v2 = number(j, "v") == 2;
    if (state_v2_ && !v2)
      return;
    state_v2_ |= v2;
    const auto &s = *it;
    if (flag(s, "connected") && !authenticated_)
      out_.push_back("{\"v\":1,\"cmd\":\"library\"}\n");
    authenticated_ = flag(s, "connected");
    m.device = text(s, "device");
    if(s.contains("auth")){m.auth_status=text(s["auth"],"status");m.storage_status=text(s["auth"],"storage");}
    m.link = text(s, "waiting_for") == "kabuki" ? Link::waiting_kabuki
             : !flag(s,"connected")            ? Link::idle
                                                : Link::connected;
    const auto epoch=s.value("media_session",std::uint64_t(0)),generation=s.value("media_generation",std::uint64_t(0));
    const bool new_track=text(s,"uri")!=m.track.uri||epoch!=media_session_||generation!=media_generation_;
    if(media_session_!=0&&epoch!=media_session_){
      m.playlists.clear();m.albums.clear();m.detail={};m.search={};++search_id_;detail_pending_=false;library_v2_=false;m.library_loading=true;
      out_.push_back("{\"v\":1,\"cmd\":\"library\"}\n");
    }
    media_session_=epoch;media_generation_=generation;
    m.media_session=epoch;m.media_generation=generation;
    m.lyrics_supported=s.contains("features")&&flag(s["features"],"lyrics");
    m.track.background=text(s,"background");
  m.canvas_supported=s.contains("features")&&flag(s["features"],"canvas");
    if(new_track){
      m.has_lyrics=m.has_canvas=false;m.lyrics={};m.lyrics_status=m.canvas_status="unknown";m.lyrics_error.clear();m.canvas_error.clear();m.canvas_path.clear();
      lyric_pending_.clear();feature_revision_.clear();
      if(m.canvas_supported&&!text(s,"uri").empty()){
        m.canvas_status="loading";out_.push_back(json({{"v",2},{"cmd","canvas"},{"client",client_tag_},{"id",0},{"uri",text(s,"uri")},
            {"session",epoch},{"generation",generation}}).dump()+"\n");
      }
    }
    m.track.title = text(s, "title");
    m.track.artist = text(s, "artist");
    m.track.uri = text(s, "uri");
    m.track.cover = text(s, "cover_large");
    if (m.track.cover.empty())
      m.track.cover = text(s, "cover");
    m.track.duration_ms = number(s, "duration_ms");
    m.track.explicit_ = flag(s, "explicit");
    m.track.cover_colour = {.18f, .20f, .23f};
    m.playing = flag(s, "playing");
    m.pending = flag(s, "pending");
    m.liked = flag(s, "liked");
    m.shuffle = flag(s, "shuffle");
    const auto mode = text(s, "repeat");
    m.repeat = mode == "one"   ? Repeat::one
               : mode == "all" ? Repeat::all
                               : Repeat::off;
    if (s.contains("context") && s["context"].is_object()) {
      m.context_kind = text(s["context"], "kind");
      for(auto& c:m.context_kind)if(c>='a'&&c<='z')c=char(c-'a'+'A');
      m.context_name = text(s["context"], "name");
    }
    const auto now = Clock::now();
    points_.clear();
    points_.push_back({now, number(s, "position_ms"), flag(s, "moving")});
    for (const auto &c : array(s, "changes"))
      points_.push_back({now + std::chrono::milliseconds(number(c, "in_ms")),
                         number(c, "position_ms"), flag(c, "moving")});
    std::stable_sort(
        points_.begin(), points_.end(),
        [](const Point &a, const Point &b) { return a.at < b.at; });
    m.detail.playing = -1;
    m.queue.clear();
    for (const auto &q : array(s, "queue")) {
      QueueEntry entry;
      entry.title = text(q, "title");
      entry.artist = text(q, "artist");
      entry.cover = text(q, "cover");
      entry.uri = text(q, "uri");
      entry.index = number(q, "index");
      entry.cover_colour = {.18f, .20f, .23f};
      m.queue.push_back(std::move(entry));
    }
    for (std::size_t i = 0; i < m.detail.songs.size(); ++i)
      if (m.detail.songs[i].uri == m.track.uri)
        m.detail.playing = int(i);
  } else if(j.contains("lyrics")||j.contains("canvas")) {
    if(text(j,"client")!=client_tag_||j.value("session",std::uint64_t(0))!=media_session_||
       j.value("generation",std::uint64_t(0))!=media_generation_)return;
    const bool lyrics=j.contains("lyrics");const auto& feature=j[lyrics?"lyrics":"canvas"];
    if(text(feature,"uri")!=m.track.uri)return;
    if(lyrics){
      if(j.value("id",std::uint64_t(0))!=feature_id_)return;
      m.lyrics_status=text(feature,"status");m.lyrics_error=text(feature,"reason");
      if(m.lyrics_status!="ready"){m.has_lyrics=false;return;}
      // Reject older cached replies that still advertise manually scrolled lyrics.
      if(text(feature,"sync")!="line"){
        m.has_lyrics=false;m.lyrics={};lyric_pending_.clear();m.lyrics_status="none";m.lyrics_error.clear();
        ++m.revision;++m.lyrics_revision;return;
      }
      const auto revision=text(feature,"revision");const auto offset=std::max(0,number(feature,"offset")),total=std::max(0,number(feature,"total"));
      if(total>1000||offset!=int(lyric_pending_.size())||(offset>0&&revision!=feature_revision_))return;
      if(offset==0){lyric_pending_.clear();feature_revision_=revision;}
      for(const auto& row:array(feature,"lines")){if(text(row,"words").size()>8192)return;lyric_pending_.push_back({std::max(0,number(row,"start_ms")),text(row,"words")});}
      if(int(lyric_pending_.size())>total)return;
      if(flag(feature,"complete")&&int(lyric_pending_.size())==total){
        m.lyrics.lines=std::move(lyric_pending_);m.lyrics.sync=text(feature,"sync")=="line"?Lyrics::Sync::line:Lyrics::Sync::none;
        const auto colour=[&](const char* key,Rgb fallback){const auto colors=feature.find("colors");if(colors==feature.end()||!colors->is_object())return fallback;
            const auto component=colors->find(key);if(component==colors->end())return fallback;const auto& c=*component;
            if(!c.is_array()||c.size()!=3||!c[0].is_number()||!c[1].is_number()||!c[2].is_number())return fallback;return Rgb{std::clamp(c[0].get<float>(),0.f,1.f),std::clamp(c[1].get<float>(),0.f,1.f),std::clamp(c[2].get<float>(),0.f,1.f)};};
        m.lyrics.background=colour("background",{.13f,.15f,.2f});m.lyrics.text=colour("text",{.55f,.58f,.65f});m.lyrics.highlight=colour("highlight",{1,1,1});
        m.lyrics.provider=text(feature,"provider");m.has_lyrics=true;++m.revision;++m.lyrics_revision;
      }else m.lyrics_status="loading";
    }else{
      if(j.value("id",std::uint64_t(1))!=0)return;
      m.canvas_status=text(feature,"status");m.canvas_error=text(feature,"reason");m.canvas_path=text(feature,"asset_path");m.canvas_kind=text(feature,"kind");
      m.has_canvas=m.canvas_status=="ready"&&m.canvas_path.rfind("/media/canvas/",0)==0;
    }
  } else if (auto it = j.find("library"); it != j.end() && it->is_object()) {
    const bool v2 = number(j, "v") == 2;
    if (library_v2_ && !v2)
      return;
    library_v2_ |= v2;
    std::vector<Tile> playlists, albums;
    for (const auto &i : array(*it, "items")) {
      auto t = tile(i);
      if (t.kind == Tile::Kind::album)
        albums.push_back(t);
      else if (t.kind == Tile::Kind::liked)
        playlists.insert(playlists.begin(), t);
      else
        playlists.push_back(t);
    }
    m.library_loading=false;m.library_error=text(*it,"error");
    if(playlists.empty()&&albums.empty()&&!m.library_error.empty()&&(!m.playlists.empty()||!m.albums.empty()))return;
    m.playlists = std::move(playlists);
    m.albums = std::move(albums);
    perf::playlists=m.playlists.size();perf::albums=m.albums.size();
  } else if (auto it = j.find("cover_ready");
             it != j.end() && it->is_object()) {
    const auto uri = text(*it, "uri"), cover = text(*it, "cover");
    for (auto *list : {&m.playlists, &m.albums, &m.search.albums})
      for (auto &t : *list)
        if (t.uri == uri)
          t.cover = cover;
    for (auto *list : {&m.detail.songs, &m.search.songs})
      for (auto &t : *list)
        if (t.uri == uri)
          t.cover = cover;
  } else if (auto it = j.find("list"); it != j.end() && it->is_object()) {
    const auto &d = *it;
    if(text(j,"client")!=client_tag_||j.value("id",std::uint64_t(0))!=detail_id_)return;
    if (text(d, "uri") != m.detail.uri)
      return;
    const auto offset = std::size_t(std::max(0, number(d, "offset")));
    if (offset != 0 && offset != detail_next_)
      return;
    detail_pending_ = false;m.detail.loading=false;m.detail.more_loading=false;
    m.detail.error=text(d,"error");
    if(!m.detail.error.empty())return;
    m.detail.metadata_error=text(d,"metadata_error");
    m.detail.count=std::max(0,number(d,"count"));
    detail_next_ = std::size_t(std::max(0, number(d, "next")));
    detail_more_ = detail_next_ < std::size_t(std::max(0, number(d, "count")));
    if (!text(d, "name").empty())
      m.detail.title = text(d, "name");
    if(offset==0||!text(d,"owner").empty())m.detail.owner = text(d, "owner");
    m.detail.description = text(d, "description");
    m.detail.saved = flag(d, "saved");
    const auto kind = text(d, "kind");
    m.detail.kind = kind == "album" ? Tile::Kind::album : Tile::Kind::playlist;
    m.detail.meta = kind == "album" ? "Album" : "Playlist";
    if(m.detail.count>0)m.detail.meta+=" · "+std::to_string(m.detail.count)+" songs";
    const auto year = number(d, "year");
    if (year > 0)
      m.detail.meta += " · " + std::to_string(year);
    if (!text(d, "cover").empty())
      m.detail.cover = text(d, "cover");
    if (offset == 0) {
      m.detail.songs.clear();
      m.detail.playing = -1;
    }
    for (const auto &t : array(d, "items")) {
      m.detail.songs.push_back(song(t));
      if (text(t, "uri") == m.track.uri)
        m.detail.playing = int(m.detail.songs.size() - 1);
    }
  } else if (auto it = j.find("search"); it != j.end() && it->is_object()) {
    const auto &s = *it;
    if (!s.contains("id") || s["id"].get<std::uint64_t>() != search_id_)
      return;
    m.search.query = text(s, "query");
    m.search.complete = flag(s, "complete");
    m.search.error = text(s, "error");
    m.search.songs.clear();
    m.search.albums.clear();
    for (const auto &t : array(s, "songs"))
      m.search.songs.push_back(song(t));
    for (const auto &a : array(s, "albums"))
      m.search.albums.push_back(tile(a));
  } else if (auto it = j.find("saved");it != j.end() && it->is_object()) {
    if(!text(*it,"error").empty()){m.notice="Could not save this item. Try again.";return;}
    m.notice.clear();
    if (text(*it, "uri") == m.track.uri)
      m.liked = flag(*it, "on");
    if (text(*it, "uri") == m.detail.uri)
      m.detail.saved = flag(*it, "on");
  }
}
void Client::run() {
#ifdef _WIN32
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data))
    return;
#endif
  while (!stop_) {
    Socket fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(38574);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd == Socket(-1) ||
        connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address)) {
      if (fd != Socket(-1))
        close_socket(fd);
      for (int i = 0; i < 20 && !stop_; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      continue;
    }
    {
      std::lock_guard lock(mutex_); ++model_.revision;
      model_.link = Link::idle;
      state_v2_ = false;
      library_v2_ = false;
      authenticated_ = false;
      out_.clear();
      out_.push_back("{\"v\":1,\"cmd\":\"library\"}\n");
    }

#ifdef SO_NOSIGPIPE
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
    std::string pending, input;
    bool alive = true;
    while (!stop_ && alive) {
      {
        std::lock_guard lock(mutex_); ++model_.revision;
        while (!out_.empty()) {
          pending += out_.front();
          out_.pop_front();
        }
      }
      fd_set reads, writes;
      FD_ZERO(&reads);
      FD_ZERO(&writes);
      FD_SET(fd, &reads);
      if (!pending.empty())
        FD_SET(fd, &writes);
      timeval timeout{0, 100000};
      if (select(int(fd) + 1, &reads, &writes, nullptr, &timeout) < 0)
        break;
      if (FD_ISSET(fd, &writes)) {
        const int n = ::send(fd, pending.data(), int(pending.size()), 0);
        if (n <= 0)
          break;
        pending.erase(0, std::size_t(n));
      }
      if (FD_ISSET(fd, &reads)) {
        char buffer[8192];
        const int n = recv(fd, buffer, sizeof buffer, 0);
        if (n <= 0)
          break;
        input.append(buffer, std::size_t(n));
        if (input.size() > 1024 * 1024)
          break;
        std::size_t end;
        while ((end = input.find('\n')) != std::string::npos) {
          try {
            line(input.substr(0, end));
          } catch (...) {
          }
          input.erase(0, end + 1);
        }
      }
    }
    close_socket(fd);
    {
      std::lock_guard lock(mutex_); ++model_.revision;
      client_tag_="app-"+std::to_string(Clock::now().time_since_epoch().count());
  model_.link = Link::offline;
      model_.playing = false;
      points_.clear();
    }
  }
#ifdef _WIN32
  WSACleanup();
#endif
}
} // namespace player
