// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "model.hpp"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <thread>
namespace player {
// One socket worker; rendering consumes snapshots and never performs I/O.
class Client {
public:
  explicit Client(bool connect = true);
  ~Client();
  Model snapshot();
  void snapshot_into(Model&);
  std::string client_id() { std::lock_guard lock(mutex_);return client_tag_; }
  void send(Cmd, int, const Model &);
  void search(const std::string &);
  void
  line(const std::string &); // also used by deterministic recorded-stream tests
private:
  using Clock = std::chrono::steady_clock;
  struct Point {
    Clock::time_point at;
    int position;
    bool moving;
  };
  std::mutex mutex_;
  Model model_;
  std::vector<Point> points_;
  std::deque<std::string> out_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::uint64_t search_id_ = 0;
  std::string client_tag_,feature_revision_;std::uint64_t feature_id_=0,media_session_=0,media_generation_=0;
  std::vector<LyricLine> lyric_pending_;
  Clock::time_point feature_started_{};
  Clock::time_point detail_started_{},search_started_{};
  bool state_v2_ = false, library_v2_ = false, authenticated_ = false;
  std::size_t detail_next_ = 0;
  std::uint64_t detail_id_=0;
  bool detail_more_ = false, detail_pending_ = false;
  void run();
  void command(nlohmann::json);
};
} // namespace player
