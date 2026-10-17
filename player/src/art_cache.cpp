// SPDX-License-Identifier: GPL-3.0-or-later
#include "art_cache.hpp"
#include "still_blur.hpp"
#include "perf.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <stb_image.h>
#include <thread>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CLOSE closesocket
using Socket = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE close
using Socket = int;
#endif
namespace player::art {
namespace {
using Clock = std::chrono::steady_clock;
struct Image {
  int id = 0, w = 0, h = 0;
  Rgb colour{};
  Clock::time_point tried{}, uploaded{};
  std::uint64_t used = 0;
  bool queued = false;
};
struct Decoded {
  std::string path;
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;
  Rgb colour{};
};
// Art proxy accepts only the three receiver caches. No app request reaches
// Spotify.
std::vector<unsigned char> fetch(const std::string &path) {
  constexpr const char *prefix = "/user/data/spotifyps5/";
  if (path.compare(0, std::char_traits<char>::length(prefix), prefix))
    return {};
  Socket fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == Socket(-1))
    return {};
#ifdef _WIN32
  DWORD timeout = 2000;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeout), sizeof timeout);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char *>(&timeout), sizeof timeout);
#else
  timeval timeout{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#ifdef SO_NOSIGPIPE
  int yes = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof yes);
#endif
#endif
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(38577);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof a)) {
    CLOSE(fd);
    return {};
  }
  const auto request =
      "GET /" + path.substr(std::char_traits<char>::length(prefix)) +
      " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  if (send(fd, request.data(), int(request.size()), 0) != int(request.size())) {
    CLOSE(fd);
    return {};
  }
  std::vector<unsigned char> bytes;
  char buffer[16384];
  int n;
  while ((n = recv(fd, buffer, sizeof buffer, 0)) > 0 &&
         bytes.size() < 2 * 1024 * 1024 + 4096)
    bytes.insert(bytes.end(), buffer, buffer + n);
  CLOSE(fd);
  std::string head(bytes.begin(), bytes.end());
  const auto end = head.find("\r\n\r\n");
  if (end == std::string::npos || head.compare(0, 13, "HTTP/1.1 200 ") ||
      bytes.size() - end - 4 > 2 * 1024 * 1024)
    return {};
  return {bytes.begin() + std::ptrdiff_t(end + 4), bytes.end()};
}
class Cache {
public:
  std::mutex mutex;
  std::condition_variable wake;
  std::map<std::string, Image> images;
  std::deque<std::string> requests;
  std::deque<Decoded> decoded;
  bool stopping = false;
  std::thread worker;
  std::uint64_t frame = 0;
  std::size_t gpu = 0;
  bool live = false;
  Cache() : worker([this] { run(); }) {}
  ~Cache() {
    {
      std::lock_guard lock(mutex);
      stopping = true;
    }
    wake.notify_all();
    worker.join();
  }
  void request(const std::string &path) {
    if (path.empty())
      return;
    std::lock_guard lock(mutex);
    auto &image = images[path];
    image.used = frame;
    if (image.id || image.queued || requests.size() >= 128 ||
        Clock::now() - image.tried < std::chrono::seconds(3))
      return;
    image.queued = true;
    image.tried = Clock::now();
    requests.push_back(path);
    wake.notify_one();
  }
  void run() {
#ifdef _WIN32
    WSADATA data{};
    WSAStartup(MAKEWORD(2, 2), &data);
#endif
    for (;;) {
      std::string path;
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] {
          return stopping || (!requests.empty() && decoded.size() < 8);
        });
        if (stopping)
          break;
        path = requests.front();
        requests.pop_front();
      }
      const bool blurred=path.rfind("still-blur:",0)==0;
      const std::string source=blurred?path.substr(11):path;
      auto bytes = fetch(source);
      perf::Scope decode_time(perf::ArtDecode);
      int w = 0, h = 0, channels = 0;
      unsigned char *pixels = nullptr;
      if (!bytes.empty() &&
          stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h,
                                &channels) &&
          w > 0 && h > 0 && w <= 4096 && h <= 4096)
        pixels = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h,
                                       &channels, 4);
      if (!pixels && source.compare(0, 6, "/user/"))
        pixels = stbi_load(source.c_str(), &w, &h, &channels, 4);
      Decoded result;
      result.path = path;
      if (pixels && w > 0 && h > 0 && w <= 4096 && h <= 4096) {
        const float longest = path.find("/backgrounds/") != std::string::npos ? 1024.f : 640.f;
        const float scale = std::min(1.f, longest / std::max(w, h));
        result.w = std::max(1, int(w * scale));
        result.h = std::max(1, int(h * scale));
        result.rgba.resize(std::size_t(result.w) * result.h * 4);
        double r = 0, g = 0, b = 0;
        for (int y = 0; y < result.h; ++y)
          for (int x = 0; x < result.w; ++x) {
            const auto src =
                (std::size_t(y * h / result.h) * w + x * w / result.w) * 4;
            const auto dst = (std::size_t(y) * result.w + x) * 4;
            for (int c = 0; c < 4; ++c)
              result.rgba[dst + c] = pixels[src + c];
            r += pixels[src];
            g += pixels[src + 1];
            b += pixels[src + 2];
          }
        const auto count = double(result.w) * result.h * 255;
        result.colour = {float(r / count), float(g / count), float(b / count)};
      }
      stbi_image_free(pixels);
      if(blurred&&!result.rgba.empty()){perf::Scope blur_time(perf::Blur);soften_still(result.rgba,result.w,result.h,2);}
      {
        std::lock_guard lock(mutex);
        if (!result.rgba.empty())
          decoded.push_back(std::move(result));
        else
          images[path].queued = false;
      }
    }
#ifdef _WIN32
    WSACleanup();
#endif
  }
};
Cache &cache() {
  static Cache cache;
  return cache;
}
} // namespace
void prefetch(const Model &m) {
  auto &c = cache();
  c.live = m.live;
  c.request(m.track.cover);
  c.request(backdrop_key(m.track.background.empty()?m.track.cover:m.track.background));
  c.request(m.detail.cover);
  for (const auto *list : {&m.playlists, &m.albums, &m.search.albums})
    for (std::size_t i = 0; i < std::min<std::size_t>(list->size(), 12); ++i)
      c.request((*list)[i].cover);
  for (std::size_t i = 0; i < std::min<std::size_t>(m.queue.size(), 8); ++i)
    c.request(m.queue[i].cover);
}
void upload(NVGcontext *vg) {
  perf::Scope upload_time(perf::ArtUpload);
  auto &c = cache();
  ++c.frame;
  for (int count = 0; count < 2; ++count) {
    Decoded ready;
    {
      std::lock_guard lock(c.mutex);
      if (c.decoded.empty())
        break;
      ready = std::move(c.decoded.front());
      c.decoded.pop_front();
      c.wake.notify_one();
    }
    const int id =
        nvgCreateImageRGBA(vg, ready.w, ready.h, 0, ready.rgba.data());
    std::lock_guard lock(c.mutex);
    auto &image = c.images[ready.path];
    image.id = id;
    image.uploaded = Clock::now();
    image.w = ready.w;
    image.h = ready.h;
    image.colour = ready.colour;
    image.queued = false;
    image.used = c.frame;
    if (id)
      c.gpu += ready.rgba.size();
  }
  std::lock_guard lock(c.mutex);
  perf::art_bytes=c.gpu;perf::art_pending=c.requests.size()+c.decoded.size();
  while (c.gpu > 64 * 1024 * 1024) {
    auto oldest = c.images.end();
    for (auto it = c.images.begin(); it != c.images.end(); ++it)
      if (it->second.id && it->second.used + 120 < c.frame &&
          (oldest == c.images.end() || it->second.used < oldest->second.used))
        oldest = it;
    if (oldest == c.images.end())
      break;
    nvgDeleteImage(vg, oldest->second.id);
    c.gpu -= std::size_t(oldest->second.w) * oldest->second.h * 4;
    oldest->second.id = 0;
  }
}
int image(NVGcontext *, const std::string &path) {
  auto &c = cache();
  c.request(path);
  std::lock_guard lock(c.mutex);
  auto it = c.images.find(path);
  if (it == c.images.end())
    return 0;
  it->second.used = c.frame;
  return it->second.id;
}
bool live() { return cache().live; }
float opacity(const std::string &path) {
  auto &c = cache();
  std::lock_guard lock(c.mutex);
  auto it = c.images.find(path);
  return it == c.images.end() || !it->second.id
             ? 0
             : std::clamp(std::chrono::duration<float>(Clock::now() -
                                                       it->second.uploaded)
                                  .count() /
                              0.15f,
                          0.f, 1.f);
}
Rgb colour(const std::string &path, Rgb fallback) {
  auto &c = cache();
  std::lock_guard lock(c.mutex);
  auto it = c.images.find(path);
  return it != c.images.end() && it->second.id ? it->second.colour : fallback;
}
} // namespace player::art
