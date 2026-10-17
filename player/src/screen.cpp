#include "screen.hpp"

#include "draw.hpp"
#include "art_cache.hpp"
#include "presentation.hpp"
#include "canvas_video.hpp"
#include "still_blur.hpp"
#include "perf.hpp"
#include "theme.hpp"

#include <borealis/core/application.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

namespace player
{

using namespace draw;
namespace t = theme;

namespace
{

using Clock = std::chrono::steady_clock;

enum Target { kScrub, kHeart, kShuffle, kPrev, kPlay, kNext, kRepeat, kLyrics, kQueue, kLib, kSearch, kDetail, kChip, kResults, kTargets };

struct Box
{
    float cx = 0, cy = 0, w = 0, h = 0, r = 0;
};

float approach(float v, float target, float dt, float rate = 18.0f)
{
    return v + (target - v) * (1.0f - std::exp(-dt * rate));
}

std::string clock_text(int ms)
{
    const int s = std::max(ms, 0) / 1000;
    char buf[16];
    if (s >= 3600)
        std::snprintf(buf, sizeof buf, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else
        std::snprintf(buf, sizeof buf, "%02d:%02d", s / 60, s % 60);
    return buf;
}

float smooth(float a, float b, float x)
{
    const float u = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

const Rgb kBlack{0.04f, 0.04f, 0.05f};

} // namespace

class Screen final : public brls::View
{
  public:
    Screen(App *app, Look look, std::function<void()> quit) : app_(app), look_(look), quit_(std::move(quit))
    {
        setFocusable(true);
        setHideHighlight(true);
        setHideClickAnimation(true);
        setDimensions(brls::Application::contentWidth, brls::Application::contentHeight);
        last_ = Clock::now();

        // `direct` actions (the media shortcuts) work at once while the controls are hidden; the
        // rest only wake the controls, so a first press never moves something unseen.
        auto act = [this](brls::ControllerButton b, bool repeat, bool direct, std::function<void()> fn)
        {
            registerAction("", b, [this, direct, fn = std::move(fn)](brls::View *)
            {
                const bool was_hidden = hidden_;
                idle_ = 0;
                if (was_hidden && !direct)
                    return true;
                fn();
                return true;
            }, true, repeat);
        };
        act(brls::BUTTON_NAV_LEFT, true, false, [this] { nav(-1, 0); });
        act(brls::BUTTON_NAV_RIGHT, true, false, [this] { nav(1, 0); });
        act(brls::BUTTON_NAV_UP, true, false, [this] { nav(0, -1); });
        act(brls::BUTTON_NAV_DOWN, true, false, [this] { nav(0, 1); });
        act(brls::BUTTON_A, false, false, [this] { press(); });
        act(brls::BUTTON_B, false, false, [this] { back(); });
        act(brls::BUTTON_X, false, true, [this] {
            // With the controls hidden, X wakes them on the play/pause button and presses it.
            if (hidden_ && !in_page() && !queue_open_ && !detail_shown() && !results_shown()) focus_ = kPlay;
            app_->send(Cmd::play_pause, 0);
        });
        act(brls::BUTTON_Y, false, true, [this] { toggle_lyrics(); });
        act(brls::BUTTON_LB, false, true, [this] { app_->send(Cmd::previous, 0); });
        act(brls::BUTTON_RB, false, true, [this] { app_->send(Cmd::next, 0); });
    }

    bool clock_running = false;

    void draw(NVGcontext *vg, float x, float y, float, float, brls::Style, brls::FrameContext *) override
    {
        const auto now = Clock::now();
        perf::Scope ui_time(perf::Ui);
        const float dt = std::min(std::chrono::duration<float>(now - last_).count(), 0.1f);
        last_ = now;
        clock_ += dt;
        if(app_->update)app_->update();
        Model &m = app_->model;
        perf::songs=m.detail.songs.size();
        art::prefetch(m);art::upload(vg);
        if(m.live&&m.link==Link::connected&&m.track.uri.empty()&&!idle_library_){enter_library();idle_library_=true;}
        if(!m.track.uri.empty())idle_library_=false;
        canvas::update(vg,m,m.live&&m.link==Link::connected&&scroll_px_<600.f&&lyr_mix_<.99f);
        perf::page=int(focus_);perf::selection=focus_==kDetail?det_sel_:focus_==kResults?res_sel_:lib_col_[lib_row_];
        const auto target=art::colour(m.track.cover,m.track.cover_colour);
        tint_.r=approach(tint_.r,target.r,dt,3);tint_.g=approach(tint_.g,target.g,dt,3);tint_.b=approach(tint_.b,target.b,dt,3);
        m.track.cover_colour=tint_;
        CardData incoming{m.track,m.context_kind,m.context_name};
        // The song is the identity. Its cover and context arrive a moment after the title, and keying on
        // them faded the card out and in twice on one skip; they now update in place.
        const auto key=m.track.uri.empty()?m.track.title+"|"+m.track.artist:m.track.uri;
        card_.update(key,incoming,dt,!m.track.title.empty());
        const auto lyric_track=m.track.uri+"|"+std::to_string(m.media_session)+"|"+std::to_string(m.media_generation);
        if(lyric_track_!=lyric_track){lyric_track_=lyric_track;lyric_hold_until_=clock_+.25f;}
        if(track_uri_!=m.track.uri){track_uri_=m.track.uri;bar_first_=true;scrub_=seek_target_=-1;
            // Lyrics stay the user's choice across songs: they fade out to the picture
            // now and back in only if the new track has them.
            lyric_asked_=false;lyr_first_=true;}
        if(lyrics_on_&&m.lyrics_supported&&!m.has_lyrics&&m.lyrics_status=="unknown"&&!m.track.uri.empty())app_->send(Cmd::request_lyrics,0);
        if (clock_running && m.playing && m.position_ms < m.track.duration_ms)
            m.position_ms = std::min(m.track.duration_ms, m.position_ms + static_cast<int>(dt * 1000.0f));
        if(seek_target_>=0){
            if(seek_uri_!=m.track.uri||(!m.pending&&std::abs(m.position_ms-seek_target_)<2000))seek_target_=-1;
            else if(now-seek_sent_>std::chrono::seconds(8)){seek_target_=-1;ui_notice_="Seek was not confirmed. Showing the current playback position.";notice_until_=clock_+6;}
        }
        if (scrub_ >= 0 && now - scrub_input_ > std::chrono::milliseconds(700))
            commit_scrub();

        // The controls fade away after a quiet spell, unless something is in use.
        idle_ += dt;
        hidden_ = look_.hide_after > 0 && idle_ > look_.hide_after && !queue_open_ && scrub_ < 0 && !in_page() &&
                  m.link == Link::connected;
        ui_ = approach(ui_, hidden_ ? 0.0f : 1.0f, dt, hidden_ ? 3.2f : 14.0f);

        nvgSave(vg);
        nvgTranslate(vg, x, y);
        if (m.link == Link::connected)
            draw_player(vg, m, dt);
        else
            draw_status(vg, m);
        nvgRestore(vg);
    }

    int scrub_ = -1; // the scrub point not sent yet, ms

  private:
    // ---- input ---------------------------------------------------------------------------

    // On the page under the player (the library, its search bar, an open album or playlist).
    bool in_page() const
    {
        return focus_ == kLib || focus_ == kSearch || focus_ == kDetail || focus_ == kChip || focus_ == kResults;
    }

    // The song picker and the search results stay on screen while the top bar (the now-playing chip
    // and Search) has the focus, and so does the library heading.
    bool top_bar() const
    {
        return focus_ == kChip || focus_ == kSearch;
    }
    bool detail_shown() const
    {
        return focus_ == kDetail || (top_bar() && chip_return_ == kDetail);
    }
    bool results_shown() const
    {
        return focus_ == kResults || (top_bar() && chip_return_ == kResults);
    }

    bool available(Target tg) const
    {
        return tg != kLyrics || app_->model.has_lyrics || app_->model.lyrics_supported;
    }

    void move_focus(int d)
    {
        // The order along the control row; the scrub bar is its own row above.
        static const Target order[] = {kHeart, kShuffle, kPrev, kPlay, kNext, kRepeat, kLyrics, kQueue};
        int at = 0;
        for (int i = 0; i < 8; ++i)
            if (order[i] == focus_)
                at = i;
        for (int i = at + d; i >= 0 && i < 8; i += d)
            if (available(order[i]))
            {
                focus_ = order[i];
                return;
            }
    }

    void nav(int dx, int dy)
    {
        perf::input();
        if (app_->model.link != Link::connected || loading_)
            return;
        if (focus_ == kChip)
        {
            if (dx > 0)
                focus_ = kSearch;
            else if (dy > 0)
                focus_ = chip_return_;
            return;
        }
        if (focus_ == kSearch)
        {
            if (dy > 0)
                focus_ = chip_return_; // back down to what you came from, not the menu
            else if (dx < 0)
                focus_ = kChip;
            return;
        }
        if (focus_ == kResults)
        {
            results_nav(dx, dy);
            return;
        }
        if (focus_ == kDetail)
        {
            detail_nav(dx, dy);
            return;
        }
        if (focus_ == kLib)
        {
            lib_nav(dx, dy);
            return;
        }
        if (queue_open_)
        {
            const int n = static_cast<int>(app_->model.queue.size());
            if (dy != 0 && n > 0)
                queue_sel_ = std::clamp(queue_sel_ + dy, 0, n - 1);
            if (dx < 0)
                queue_open_ = false;
            return;
        }
        if (focus_ == kScrub)
        {
            if (dx != 0)
                step_scrub(dx);
            else if (dy > 0)
            {
                commit_scrub();
                focus_ = kPlay;
            }
            return;
        }
        if (dx != 0)
            move_focus(dx);
        else if (dy < 0)
        {
            focus_ = kScrub;
            held_ = 0;
        }
        else if (dy > 0 && !lib_rows().empty())
            enter_library();
    }

    // ---- the library page under the player ----

    struct Row
    {
        int id; // 0 playlists, 1 albums, 2 artists: what open_item reports
        const char *label;
        const std::vector<Tile> *tiles;
    };

    std::vector<Row> lib_rows() const
    {
        const Model &m = app_->model;
        std::vector<Row> rows;
        if (!m.playlists.empty())
            rows.push_back({0, "Playlists", &m.playlists});
        if (!m.albums.empty())
            rows.push_back({1, "Albums", &m.albums});
        return rows;
    }

    void enter_library()
    {
        focus_ = kLib;
        chip_return_ = kLib;
        lib_row_ = 0;
    }

    // ---- search results: songs on the left, albums on the right ----

    void results_nav(int dx, int dy)
    {
        const SearchResults &r = app_->model.search;
        const int ns = static_cast<int>(r.songs.size());
        const int na = static_cast<int>(r.albums.size());
        if (res_zone_ == 0)
        {
            if (dy < 0)
            {
                if (res_sel_ == 0)
                {
                    chip_return_ = kResults; // up from the first song reaches the top bar
                    focus_ = kChip;
                }
                else
                    --res_sel_;
            }
            else if (dy > 0)
                res_sel_ = std::min(res_sel_ + 1, std::max(ns - 1, 0));
            else if (dx > 0 && na > 0)
            {
                res_zone_ = 1;
                res_alb_ = 0;
            }
        }
        else
        {
            const int col = res_alb_ % 2, row = res_alb_ / 2;
            if (dx < 0)
            {
                if (col == 0)
                    res_zone_ = 0;
                else
                    --res_alb_;
            }
            else if (dx > 0)
            {
                if (col == 0 && res_alb_ + 1 < na)
                    ++res_alb_;
            }
            else if (dy < 0)
            {
                if (row == 0)
                {
                    chip_return_ = kResults;
                    focus_ = kChip;
                }
                else
                    res_alb_ -= 2;
            }
            else if (dy > 0 && res_alb_ + 2 < na)
                res_alb_ += 2;
        }
    }

    void results_press()
    {
        if (res_zone_ == 0)
        {
            if(res_sel_>=0&&res_sel_<int(app_->model.search.songs.size())){pressed_uri_=app_->model.search.songs[res_sel_].uri;pressed_at_=clock_;}
            app_->send(Cmd::play_found, res_sel_);
        }
        else
        {
            app_->send(Cmd::open_found_album, res_alb_);
            focus_ = kDetail;
            detail_back_ = kResults;
            det_zone_ = 2;
            det_sel_ = 0;
            det_scroll_ = 0;
        }
    }

    // ---- an open album or playlist: the song select screen ----

    void detail_nav(int dx, int dy)
    {
        const int n = static_cast<int>(app_->model.detail.songs.size());
        if (dy < 0 && det_sel_ == 0)
        {
            chip_return_ = kDetail; // up from the first song reaches the chip
            focus_ = kChip;
            return;
        }
        if (dy != 0 && n > 0)
        {
            det_sel_ = std::clamp(det_sel_ + dy, 0, n - 1);
            if(det_sel_>=n-10)app_->send(Cmd::load_more,0);
        }
        else if (dx < 0)
            focus_ = detail_back_; // Left goes back, as Circle does
    }

    void detail_press()
    {
        if(app_->model.detail.loading)return;
        if(!app_->model.detail.error.empty()){app_->send(Cmd::retry,0);return;}
        if(det_sel_>=0&&det_sel_<int(app_->model.detail.songs.size())){pressed_uri_=app_->model.detail.songs[det_sel_].uri;pressed_at_=clock_;}
        app_->send(Cmd::play_song, det_sel_);
    }

    void leave_library()
    {
        focus_ = kPlay;
    }

    void lib_nav(int dx, int dy)
    {
        const auto rows = lib_rows();
        if(rows.empty()){
            if(dy<0){chip_return_=kLib;focus_=kSearch;}return;
        }
        lib_row_ = std::clamp(lib_row_, 0, static_cast<int>(rows.size()) - 1);
        if (dx != 0)
        {
            const int n = static_cast<int>(rows[lib_row_].tiles->size());
            lib_col_[lib_row_] = std::clamp(lib_col_[lib_row_] + dx, 0, n - 1);
        }
        else if (dy < 0)
        {
            if (lib_row_ == 0)
            {
                chip_return_ = kLib; // up from the first row reaches the now-playing chip, then Search
                focus_ = kChip;
            }
            else
                --lib_row_;
        }
        else if (dy > 0)
            lib_row_ = std::min(lib_row_ + 1, static_cast<int>(rows.size()) - 1);
    }

    void step_scrub(int dir)
    {
        const auto now = Clock::now();
        held_ = now - scrub_input_ < std::chrono::milliseconds(250) ? held_ + 1 : 0;
        scrub_input_ = now;
        // 5 s at first, 15 s after a while held, 60 s after that.
        const int step = held_ < 9 ? 5000 : held_ < 30 ? 15000 : 60000;
        if (scrub_ < 0)
            scrub_ = seek_target_>=0?seek_target_:app_->model.position_ms;
        scrub_ = std::clamp(scrub_ + dir * step, 0, app_->model.track.duration_ms);
    }

    void commit_scrub()
    {
        if (scrub_ < 0)
            return;
        const int to = scrub_;
        scrub_ = -1;seek_target_=to;seek_uri_=app_->model.track.uri;seek_sent_=Clock::now();
        app_->send(Cmd::seek, to);
    }

    void toggle_lyrics()
    {
        if(!app_->model.has_lyrics&&!app_->model.lyrics_supported)return;
        lyrics_on_ = !lyrics_on_;lyric_asked_=lyrics_on_;lyr_first_=true;
        if(lyrics_on_&&!app_->model.has_lyrics)app_->send(Cmd::request_lyrics,0);
        if (lyrics_on_)
            queue_open_ = false;
    }

    void press()
    {
        perf::input();
        if (loading_)
            return;
        Model &m = app_->model;
        if (queue_open_)
        {
            app_->send(Cmd::play_queued, queue_sel_);
            return;
        }
        switch (focus_)
        {
        case kScrub:
            if (scrub_ >= 0)
                commit_scrub();
            else
                scrub_ = m.position_ms, scrub_input_ = Clock::now();
            break;
        case kLib:
        {
            const auto rows = lib_rows();
            if(rows.empty()){
                if(!m.library_error.empty())app_->send(Cmd::refresh_library,0);
                else{chip_return_=kLib;focus_=kSearch;}
            }
            if (lib_row_ < static_cast<int>(rows.size()))
            {
                const int col = lib_col_[lib_row_];
                app_->send(Cmd::open_item, rows[lib_row_].id * 1000 + col);
                if (col < static_cast<int>(rows[lib_row_].tiles->size()))
                {
                    focus_ = kDetail;
                    detail_back_ = kLib;
                    det_zone_ = 2;
                    det_sel_ = 0;
                    det_scroll_ = 0;
                }
            }
            break;
        }
        case kSearch:
            app_->send(Cmd::open_search, 0); // the query is typed elsewhere; the results come back in the model
            focus_ = kResults;
            loading_ = true; // a grey screen and a wheel while the results come in
            load_start_ = clock_;
            res_zone_ = 0;
            res_sel_ = 0;
            res_alb_ = 0;
            break;
        case kResults: results_press(); break;
        case kChip: leave_library(); break; // back to the player
        case kDetail: detail_press(); break;
        case kHeart: app_->send(Cmd::like, 0); break;
        case kShuffle:
        {
            const bool next = !shuffle_shown(m);
            opt_shuffle_ = next; opt_shuffle_on_ = true; opt_shuffle_at_ = Clock::now();
            app_->send(Cmd::shuffle, next ? 2 : 1);
            break;
        }
        case kPrev: app_->send(Cmd::previous, 0); break;
        case kPlay: app_->send(Cmd::play_pause, 0); break;
        case kNext: app_->send(Cmd::next, 0); break;
        case kRepeat:
        {
            const Repeat cur = repeat_shown(m);
            const Repeat next = cur == Repeat::off ? Repeat::all : cur == Repeat::all ? Repeat::one : Repeat::off;
            opt_repeat_ = next; opt_repeat_on_ = true; opt_repeat_at_ = Clock::now();
            app_->send(Cmd::repeat, next == Repeat::off ? 1 : next == Repeat::all ? 2 : 3);
            break;
        }
        case kLyrics: toggle_lyrics(); break;
        case kQueue:
            queue_open_ = true;
            queue_sel_ = 0;
            break;
        default: break;
        }
    }

    // Shuffle and repeat show the press at once. They go back to librespot's answer once it
    // agrees, or after five seconds if it never registered.
    static constexpr auto kOptimisticFor = std::chrono::seconds(5);
    bool shuffle_shown(const Model &m)
    {
        if (opt_shuffle_on_)
        {
            if (m.shuffle == opt_shuffle_ || Clock::now() - opt_shuffle_at_ > kOptimisticFor) opt_shuffle_on_ = false;
            else return opt_shuffle_;
        }
        return m.shuffle;
    }
    Repeat repeat_shown(const Model &m)
    {
        if (opt_repeat_on_)
        {
            if (m.repeat == opt_repeat_ || Clock::now() - opt_repeat_at_ > kOptimisticFor) opt_repeat_on_ = false;
            else return opt_repeat_;
        }
        return m.repeat;
    }

    void back()
    {
        if (loading_)
        {
            loading_ = false; // Circle cancels the search
            focus_ = kSearch;
            chip_return_ = kLib;
            app_->send(Cmd::close_search,0);
            return;
        }
        if (queue_open_)
            queue_open_ = false;
        else if (focus_ == kDetail)
            focus_ = detail_back_;
        else if (focus_ == kResults)
        {
            focus_ = kLib;
            app_->send(Cmd::close_search, 0);
        }
        else if (focus_ == kLib || focus_ == kSearch || focus_ == kChip)
            leave_library();
        else if (scrub_ >= 0)
            scrub_ = -1;
        else if (lyrics_on_)
            lyrics_on_ = false;
        else if (app_->model.live)
            enter_library(); // nothing left to close: down to the library, never out of the app
        else
            quit_();
    }

    // ---- geometry ------------------------------------------------------------------------

    Box box_of(Target tg) const
    {
        switch (tg)
        {
        case kHeart: return {t::kHeartX, t::kRowY, 2 * t::kHeartR, 2 * t::kHeartR, t::kHeartR};
        case kShuffle:
        case kPrev:
        case kPlay:
        case kNext:
        case kRepeat:
        {
            const float cx = t::kW / 2 + (tg - kPlay) * t::kStep;
            if (tg == kPlay)
                return {cx, t::kRowY, 2 * 40.0f, 2 * 40.0f, 40.0f};
            return {cx, t::kRowY, 72, 72, 36};
        }
        case kLyrics: return lyrics_box_;
        case kQueue: return queue_box_;
        default: return {t::kW / 2, t::kBarY, t::kBarX1 - t::kBarX0, 28, 14};
        }
    }

    void measure_pills(NVGcontext *vg)
    {
        const float wq = t::kPillBtnPad * 2 + t::kPillBtnIcon + t::kPillBtnInner +
                         text_width(vg, Weight::semibold, t::kPillBtnFont, "Queue");
        const float wl = t::kPillBtnPad * 2 + t::kPillBtnIcon + t::kPillBtnInner +
                         text_width(vg, Weight::semibold, t::kPillBtnFont, "Lyrics");
        const float right = t::kW - t::kMargin;
        queue_box_ = {right - wq / 2, t::kRowY, wq, t::kPillBtnH, t::kPillBtnH / 2};
        const float lr = right - wq - t::kPillBtnGap;
        lyrics_box_ = {lr - wl / 2, t::kRowY, wl, t::kPillBtnH, t::kPillBtnH / 2};
    }

    // ---- drawing -------------------------------------------------------------------------

    void rrect(NVGcontext *vg, float cx, float cy, float w, float h, float r, NVGcolor c)
    {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, cx - w / 2, cy - h / 2, w, h, r);
        nvgFillColor(vg, c);
        nvgFill(vg);
    }

    // The picture behind the song: the artist's wide one if it arrived, else the album cover.
    static const std::string &backdrop(const Model &m)
    {
        return m.track.background.empty() ? m.track.cover : m.track.background;
    }

    void draw_background(NVGcontext *vg, const Model &m, float lyr)
    {
        const bool canvas = m.has_canvas && (m.live||look_.background==Look::Background::canvas);
        const Rgb tint = dark_tint(m.track.cover_colour);
        if (canvas)
        {
            nvgBeginPath(vg);nvgRect(vg,0,0,t::kW,t::kH);nvgFillColor(vg,rgb(tint));nvgFill(vg);
            // The cover is always the still underneath; the video fades in over it
            // when it can play, so a Canvas that fails or is slow just stays a picture.
            if(m.live){
                if(!image_fill(vg,backdrop_key(backdrop(m)),0,0,t::kW,t::kH,1.f,!m.track.background.empty()))canvas_stand_in(vg,m.track.cover_colour,0,0,t::kW,t::kH);
                canvas::paint(vg,0,0,t::kW,t::kH);
            }
            else if(!image_fill(vg,m.canvas_still,0,0,t::kW,t::kH,1.f))canvas_stand_in(vg,m.track.cover_colour,0,0,t::kW,t::kH);
            // The scrim: text sits over video, so the lower and left parts darken.
            nvgBeginPath(vg);
            nvgRect(vg, 0, 0, t::kW, t::kH);
            nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, t::kH, nvgRGBAf(0, 0, 0, 0.18f), nvgRGBAf(0, 0, 0, 0.62f)));
            nvgFill(vg);
            nvgBeginPath(vg);
            nvgRect(vg, 0, 0, t::kW, t::kH);
            nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 1100, 0, nvgRGBAf(0, 0, 0, 0.34f), nvgRGBAf(0, 0, 0, 0)));
            nvgFill(vg);
        }
        else
        {
            const Rgb top = {std::min(1.0f, tint.r * 1.12f), std::min(1.0f, tint.g * 1.12f),
                             std::min(1.0f, tint.b * 1.12f)};
            nvgBeginPath(vg);
            nvgRect(vg, 0, 0, t::kW, t::kH);
            nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, t::kH, rgb(top), rgb(tint)));
            nvgFill(vg);
            // The album cover, large and faint, behind everything: the picture the colour came from.
            // Spotify's wide artist picture when it has one, else the album cover.
            const std::string &back = backdrop(m);
            const bool wide = !m.track.background.empty();
            const std::string softened=backdrop_key(back);
            const float shown = art::opacity(softened);
            if (m.live && shown > 0.01f)
                image_fill(vg, softened, 0, 0, t::kW, t::kH, (wide ? 0.55f : 0.30f) * shown,wide);
            else if (m.live)
                image_fill(vg, backdrop_key(m.track.cover), 0, 0, t::kW, t::kH, 0.30f * art::opacity(backdrop_key(m.track.cover)));
            // Text sits over the picture: darken the lower and left parts, as over a Canvas.
            nvgBeginPath(vg);
            nvgRect(vg, 0, 0, t::kW, t::kH);
            nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, t::kH, nvgRGBAf(0, 0, 0, 0.05f), nvgRGBAf(0, 0, 0, 0.55f)));
            nvgFill(vg);
        }
        if (lyr > 0.001f)
        {
            const Rgb lc = m.has_lyrics?m.lyrics.background:lyrics_colour(m.track.cover_colour);
            nvgBeginPath(vg);
            nvgRect(vg, 0, 0, t::kW, t::kH);
            nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, t::kH, rgb(lc, lyr),
                                               rgb(mix(lc, kBlack, 0.35f), lyr)));
            nvgFill(vg);
        }
    }

    void draw_header(NVGcontext *vg, const Model &m)
    {
        nvgBeginPath(vg);
        nvgCircle(vg, t::kMarkX, t::kMarkY, t::kMarkR);
        nvgFillColor(vg, rgba(0xB3, 0xB3, 0xB3, 0.92f));
        nvgFill(vg);
        icon(vg, Icon::note, t::kMarkX, t::kMarkY, 34.0f, rgba(20, 24, 30));
        if (!card_.value.context.empty())
        {
            text(vg, Weight::medium, t::kContextLabelSize, grey(0.95f), t::kContextX, t::kContextLabelBase,
                 "PLAYING FROM " + card_.value.kind, Align::left, t::kContextLabelTrack);
            text(vg, Weight::bold, t::kContextNameSize, white(0.82f), t::kContextX, t::kContextNameBase,
                 fit(vg, Weight::bold, t::kContextNameSize, card_.value.context, 900.0f));
        }
    }

    // The track block, shrinking into the bottom-left corner as the controls go.
    void draw_track(NVGcontext *vg, const Model &m)
    {
        const float u = smooth(0.0f, 1.0f, 1.0f - ui_); // 0 full size, 1 the corner
        auto lerp = [u](float a, float b) { return a + (b - a) * u; };
        const float size = lerp(t::kCoverSize, t::kCornerCover);
        const float cx = lerp(t::kCoverX, t::kCornerMargin);
        const float cy = lerp(t::kCoverY, t::kH - t::kCornerMargin - t::kCornerCover);
        cover(vg, card_.value.track.cover, card_.value.track.cover_colour, cx, cy, size, lerp(t::kCoverRadius, 3.0f));
        const float tx = cx + size + lerp(t::kTitleX - t::kCoverX - t::kCoverSize, 28.0f);
        const float room = lerp(t::kW - t::kMargin - tx, 820.0f);
        const float ts = lerp(t::kTitleSize, t::kCornerTitleSize), as = lerp(t::kArtistSize, t::kCornerArtistSize);
        const float title_room=std::min(room,lerp(queue_open_?t::kQueueX-t::kQueuePad-tx:t::kW-t::kMargin-t::kTitleX,820.f));
        const auto lines=title_lines(vg,Weight::bold,ts,card_.value.track.title,std::max(120.f,title_room));
        const float baseline=cy+lerp(t::kTitleBase-t::kCoverY,62.f),leading=ts*1.08f;
        for(std::size_t i=0;i<lines.size();++i)
            text(vg,Weight::bold,ts,white(),tx,baseline-(lines.size()-1-i)*leading,lines[i]);
        text(vg, Weight::semibold, as, grey(), tx, cy + lerp(t::kArtistBase - t::kCoverY, 104.0f),
             fit(vg, Weight::semibold, as, card_.value.track.artist, room));
    }

    void draw_scrub(NVGcontext *vg, const Model &m, float dt)
    {
        const bool focus = focus_ == kScrub && !queue_open_;
        scrub_focus_ = approach(scrub_focus_, focus ? 1.0f : 0.0f, dt);
        const int shown = scrub_ >= 0 ? scrub_ : seek_target_>=0 ? seek_target_ : m.position_ms;
        const float frac = m.track.duration_ms > 0 ? std::clamp(shown / static_cast<float>(m.track.duration_ms), 0.0f, 1.0f) : 0.0f;
        // the fill and thumb glide to a new point in a blink instead of jumping
        shown_frac_ = bar_first_ ? frac : approach(shown_frac_, frac, dt, 26.0f);
        bar_first_ = false;
        const float bar = t::kBarH + (t::kBarHFocus - t::kBarH) * scrub_focus_;
        const float len = bar_x1_ - t::kBarX0;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, t::kBarX0, t::kBarY - bar / 2, len, bar, bar / 2);
        nvgFillColor(vg, white(0.24f));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, t::kBarX0, t::kBarY - bar / 2, std::max(len * shown_frac_, bar), bar, bar / 2);
        nvgFillColor(vg, white());
        nvgFill(vg);
        if (m.pending)
        {
            // buffering: a soft light sweeps along the track
            const float u = std::fmod(clock_ * 0.8f, 1.0f);
            const float sx = t::kBarX0 + len * u;
            nvgBeginPath(vg);
            nvgRect(vg, t::kBarX0, t::kBarY - bar / 2 - 1, len, bar + 2);
            nvgFillPaint(vg, nvgLinearGradient(vg, sx - 120, 0, sx + 120, 0, white(0), white(0.35f)));
            nvgFill(vg);
        }
        if (scrub_focus_ > 0.01f)
        {
            const float r = t::kThumbR * scrub_focus_;
            nvgBeginPath(vg);
            nvgCircle(vg, t::kBarX0 + len * shown_frac_, t::kBarY, r);
            nvgFillColor(vg, white());
            nvgFill(vg);
            if (look_.focus == Look::Focus::ring)
            {
                nvgBeginPath(vg);
                nvgCircle(vg, t::kBarX0 + len * shown_frac_, t::kBarY, r + 6.0f * scrub_focus_);
                nvgStrokeColor(vg, white(0.9f * scrub_focus_));
                nvgStrokeWidth(vg, 2.5f);
                nvgStroke(vg);
            }
        }
        const NVGcolor tc = scrub_ >= 0 ? white() : white(0.72f);
        text(vg, Weight::regular, t::kTimeSize, tc, t::kTimeLeftX, t::kTimeBase, clock_text(shown));
        text(vg, Weight::regular, t::kTimeSize, white(0.72f), bar_x1_ + (t::kW - t::kMargin - t::kBarX1), t::kTimeBase,
             clock_text(m.track.duration_ms), Align::right);
    }

    // The glyph colour of a control: white, going dark as its fill grows in.
    NVGcolor glyph(Target tg, NVGcolor base) const
    {
        if (look_.focus != Look::Focus::fill)
            return base;
        return mix(base, rgba(18, 18, 20), focus_mix_[tg]);
    }

    void draw_controls(NVGcontext *vg, const Model &m, float dt)
    {
        measure_pills(vg);
        const bool canvas = m.has_canvas && (m.live||look_.background==Look::Background::canvas);
        const bool active = !queue_open_;
        for (int i = 0; i < kTargets; ++i)
            focus_mix_[i] = approach(focus_mix_[i], (active && focus_ == i) ? 1.0f : 0.0f, dt);
        const NVGcolor panel = canvas ? rgba(32, 32, 34, 0.70f) : white(0.075f);
        const NVGcolor green = rgba(t::kGreen[0], t::kGreen[1], t::kGreen[2]);

        // The pill behind shuffle..repeat.
        if (look_.pill)
            rrect(vg, t::kW / 2, t::kRowY, 2 * t::kPillHalfW, t::kPillH, t::kPillRadius, panel);

        // The moving highlight, one shape that glides from control to control.
        const bool on_control = active && focus_ != kScrub && !in_page();
        const Box tb = box_of(focus_);
        if (hl_first_)
        {
            hl_ = tb;
            hl_first_ = false;
        }
        hl_.cx = approach(hl_.cx, tb.cx, dt, 24.0f);
        hl_.cy = approach(hl_.cy, tb.cy, dt, 24.0f);
        hl_.w = approach(hl_.w, tb.w, dt, 24.0f);
        hl_.h = approach(hl_.h, tb.h, dt, 24.0f);
        hl_.r = approach(hl_.r, tb.r, dt, 24.0f);
        hl_alpha_ = approach(hl_alpha_, on_control ? 1.0f : 0.0f, dt, 20.0f);
        const bool fill = look_.focus == Look::Focus::fill;
        // A focused play button is already a white disc; in fill mode it just grows.
        if (hl_alpha_ > 0.01f && !(fill && focus_ == kPlay && hl_.w < 80.0f + 6.0f))
        {
            nvgBeginPath(vg);
            if (fill)
            {
                nvgRoundedRect(vg, hl_.cx - hl_.w / 2, hl_.cy - hl_.h / 2, hl_.w, hl_.h, hl_.r);
                nvgFillColor(vg, white(hl_alpha_));
                nvgFill(vg);
            }
            else
            {
                const float g = 5.0f; // the ring stands off the control
                nvgRoundedRect(vg, hl_.cx - hl_.w / 2 - g, hl_.cy - hl_.h / 2 - g, hl_.w + 2 * g, hl_.h + 2 * g, hl_.r + g);
                nvgStrokeColor(vg, white(0.95f * hl_alpha_));
                nvgStrokeWidth(vg, 2.5f);
                nvgStroke(vg);
            }
        }

        // Heart: a dark disc behind it, green and filled once saved (green is "on").
        {
            const Box b = box_of(kHeart);
            if (!(fill && focus_mix_[kHeart] > 0.5f))
                rrect(vg, b.cx, b.cy, b.w, b.h, b.r, canvas ? rgba(32, 32, 34, 0.70f) : white(0.075f));
            icon(vg, m.liked ? Icon::heart_filled : Icon::heart, b.cx, b.cy, 34.0f,
                 m.liked ? green : glyph(kHeart, white()));
        }

        // Transport.
        const NVGcolor idle = white(0.92f);
        const Repeat rp = repeat_shown(m);
        const bool shuf = shuffle_shown(m);
        const float cx = t::kW / 2;
        icon(vg, Icon::shuffle, cx - 2 * t::kStep, t::kRowY, t::kIcon, shuf ? green : glyph(kShuffle, idle));
        icon(vg, Icon::previous, cx - t::kStep, t::kRowY, t::kIcon, glyph(kPrev, idle));
        icon(vg, Icon::next, cx + t::kStep, t::kRowY, t::kIcon, glyph(kNext, idle));
        icon(vg, rp == Repeat::one ? Icon::repeat_one : Icon::repeat, cx + 2 * t::kStep, t::kRowY, t::kIcon,
             rp != Repeat::off ? green : glyph(kRepeat, idle));
        // the "on" dot under shuffle and repeat
        auto dot = [&](float x)
        {
            nvgBeginPath(vg);
            nvgCircle(vg, x, t::kRowY + 28.0f, 2.6f);
            nvgFillColor(vg, green);
            nvgFill(vg);
        };
        if (shuf) dot(cx - 2 * t::kStep);
        if (rp != Repeat::off) dot(cx + 2 * t::kStep);

        // Play/pause: a white disc, black glyph; it swells when focused.
        {
            const float f = focus_mix_[kPlay];
            const float r = t::kPlayR + (fill ? 6.0f : 3.0f) * f;
            nvgBeginPath(vg);
            nvgCircle(vg, cx, t::kRowY, r);
            nvgFillColor(vg, white());
            nvgFill(vg);
            icon(vg, m.playing ? Icon::pause : Icon::play, cx + (m.playing ? 0.0f : 1.5f), t::kRowY, 34.0f + 4.0f * f,
                 rgba(18, 18, 20));
        }

        // Lyrics and Queue pills.
        auto pill = [&](Target tg, const Box &b, Icon ic, const char *label, bool on)
        {
            const bool focused = focus_mix_[tg] > 0.5f;
            if (!(fill && focused))
                rrect(vg, b.cx, b.cy, b.w, b.h, b.r, panel);
            const NVGcolor base = white(0.95f);
            const NVGcolor col = fill ? mix(base, rgba(18, 18, 20), focus_mix_[tg]) : base;
            const float left = b.cx - b.w / 2 + t::kPillBtnPad;
            icon(vg, ic, left + t::kPillBtnIcon / 2, b.cy, t::kPillBtnIcon, col);
            text(vg, Weight::semibold, t::kPillBtnFont, col, left + t::kPillBtnIcon + t::kPillBtnInner,
                 b.cy + t::kPillBtnFont * 0.35f, label);
            if (on) // green is "on": the same dot as under shuffle and repeat
            {
                nvgBeginPath(vg);
                nvgCircle(vg, b.cx, b.cy + b.h / 2 + 12.0f, 2.6f);
                nvgFillColor(vg, green);
                nvgFill(vg);
            }
        };
        // nvgGlobalAlpha sets rather than multiplies: carry the toolbar's own fade in by hand.
        lyrics_pill_ = approach(lyrics_pill_, (m.has_lyrics || m.lyrics_supported) ? 1.0f : 0.0f, dt, 10.0f);
        if (queue_mix_ < 0.99f)
        {
            const float a = ctrl_alpha_ * (1.0f - queue_mix_);
            if (lyrics_pill_ > 0.01f)
            {
                nvgSave(vg);
                nvgGlobalAlpha(vg, a * lyrics_pill_);
                pill(kLyrics, lyrics_box_, Icon::lyrics, "Lyrics", lyrics_on_);
                nvgRestore(vg);
            }
            nvgSave(vg);
            nvgGlobalAlpha(vg, a);
            pill(kQueue, queue_box_, Icon::queue, "Queue", false);
            nvgRestore(vg);
        }
    }

    void draw_lyrics(NVGcontext *vg, const Model &m, float dt, float alpha)
    {
        // Keep the last lyrics so a change of song fades them out instead of cutting.
        if(m.has_lyrics&&(lyric_hold_uri_!=lyric_track_||lyric_hold_revision_!=m.lyrics_revision||lyric_hold_.lines.empty())){
            lyric_hold_=m.lyrics;lyric_hold_uri_=lyric_track_;lyric_hold_revision_=m.lyrics_revision;
        }
        if(!m.has_lyrics&&clock_>=lyric_hold_until_){lyric_hold_={};lyric_hold_uri_.clear();}
        if(alpha<.01f)return;
        if(m.has_lyrics&&m.lyrics.sync==Lyrics::Sync::none){
            nvgSave(vg);nvgGlobalAlpha(vg,alpha);
            text(vg,Weight::semibold,34,white(),t::kMargin,360,"Lyrics unavailable");
            nvgRestore(vg);return;
        }
        const bool holding=!m.has_lyrics&&!lyric_asked_&&!lyric_hold_.lines.empty()&&clock_<lyric_hold_until_;
        if(holding)alpha*=std::clamp((lyric_hold_until_-clock_)/.25f,0.f,1.f);
        if(!m.has_lyrics&&!holding){
            if(!lyric_asked_)return;
            nvgSave(vg);nvgGlobalAlpha(vg,alpha);
            // "Unavailable" only when the receiver says so. Unknown, loading, or ready with chunks
            // still arriving all read as loading.
            const bool settled=m.lyrics_status=="none"||m.lyrics_status=="error"||m.lyrics_status=="unsupported"||m.lyrics_status=="rate_limited";
            if(!settled){spinner(vg,t::kW/2,420,30,clock_,white(.8f));text(vg,Weight::medium,26,grey(),t::kW/2,490,"Loading lyrics...",Align::centre);}
            else{text(vg,Weight::semibold,34,white(),t::kMargin,360,"Lyrics unavailable");
                 text(vg,Weight::regular,24,grey(),t::kMargin,412,fit(vg,Weight::regular,24,m.lyrics_error,1500));}
            nvgRestore(vg);return;
        }
        const Lyrics &ly = m.has_lyrics ? m.lyrics : lyric_hold_;
        const NVGcolor hi=rgb(ly.highlight),lo=rgb(ly.text);
        const bool synced = ly.sync != Lyrics::Sync::none;
        int cur = -1;
        if (synced)
            for (size_t i = 0; i < ly.lines.size(); ++i)
                if (ly.lines[i].start_ms <= m.position_ms)
                    cur = static_cast<int>(i);

        const float room = 1.0f - ui_; // 1 once the controls are gone
        const float bottom = t::kLyricsBottom + t::kLyricsExtraBottom * smooth(0.0f, 1.0f, room);
        const float anchor = t::kLyricsAnchor + t::kLyricsExtraAnchor * smooth(0.0f, 1.0f, room);
        nvgSave(vg);
        nvgScissor(vg, 0, t::kLyricsTop, t::kW, bottom - t::kLyricsTop);
        nvgFontFaceId(vg, 0);
        // Lay the lines out as blocks (each wraps to the width), then glide the list so the
        // current block sits at the anchor.
        // Cache owned strings, not NanoVG pointers into a temporary snapshot.
        const auto layout_key=lyric_hold_uri_+"|"+std::to_string(lyric_hold_revision_)+"|"+std::to_string(ly.lines.size());
        if(lyric_layout_key_!=layout_key){
            lyric_layout_key_=layout_key;lyric_blocks_.clear();float y=0;
            text_width(vg,Weight::bold,t::kLyricsSize,"x");
            for(const auto& line:ly.lines){
                LyricBlock block;block.top=y;const char* p=line.words.c_str();
                while(*p&&block.rows.size()<128){NVGtextRow rows[8];int n=nvgTextBreakLines(vg,p,nullptr,t::kLyricsW,rows,8);if(n<=0)break;
                    for(int k=0;k<n;++k)block.rows.emplace_back(rows[k].start,rows[k].end);
                    if(rows[n-1].next<=p)break;p=rows[n-1].next;
                }
                if(block.rows.empty())block.rows.emplace_back("");
                block.h=block.rows.size()*t::kLyricsLine;y+=block.h+28;lyric_blocks_.push_back(std::move(block));
            }
            lyric_height_=y;lyr_first_=true;
        }
        const auto& blocks=lyric_blocks_;
        const float want = cur >= 0 ? blocks[cur].top : -t::kLyricsLine;
        if (lyr_first_)
        {
            lyr_scroll_ = want;
            lyr_first_ = false;
        }
        lyr_scroll_ = approach(lyr_scroll_, want, dt, 7.0f);
        for (size_t i = 0; i < ly.lines.size(); ++i)
        {
            const auto &b = blocks[i];
            const float top = anchor + b.top - lyr_scroll_;
            if (top > bottom + 40 || top + b.h < t::kLyricsTop - 40)
                continue;
            // sung and current lines are the highlight colour; a new line fades in as it starts
            float lit = !synced ? 1.0f : (static_cast<int>(i) < cur ? 1.0f : 0.0f);
            if (synced && static_cast<int>(i) == cur)
                lit = ease_out((m.position_ms - ly.lines[i].start_ms) / 260.0f);
            const NVGcolor col = mix(lo, hi, lit);
            const float edge = smooth(t::kLyricsTop, t::kLyricsTop + 110, top + t::kLyricsLine * 0.5f) *
                               (1.0f - smooth(bottom - 120, bottom, top + t::kLyricsLine * 0.5f));
            for (std::size_t r = 0; r < b.rows.size(); ++r)
            {
                const auto& s=b.rows[r];
                NVGcolor cc = col;
                cc.a *= edge * alpha;
                text(vg, Weight::bold, t::kLyricsSize, cc, t::kLyricsX, top + r * t::kLyricsLine + t::kLyricsSize * 0.95f, s);
            }
        }
        nvgRestore(vg);
        if(!ly.provider.empty())text(vg,Weight::regular,18,white(.55f*alpha),t::kW-t::kMargin,126,fit(vg,Weight::regular,18,ly.provider,380),Align::right);
    }

    // The queue takes the whole right side of the screen, top to bottom.
    void draw_queue(NVGcontext *vg, const Model &m, float dt)
    {
        const float a = queue_mix_;
        if (a < 0.01f)
            return;
        const float slide = (1.0f - ease_out(a)) * 90.0f;
        const float x = t::kQueueX + slide, w = t::kW - t::kQueueX;
        nvgSave(vg);
        nvgGlobalAlpha(vg, a);
        // a plain translucent rectangle with a hard edge, top to bottom
        nvgBeginPath(vg);
        nvgRect(vg, x, 0, w + 90.0f, t::kH);
        nvgFillColor(vg, nvgRGBAf(0.0f, 0.0f, 0.0f, 0.62f));
        nvgFill(vg);

        const float px = x + t::kQueuePad;
        const float inner = w - 2 * t::kQueuePad;
        const float tx = px + t::kQueueArt + 24.0f, tw = inner - t::kQueueArt - 24.0f;
        text(vg, Weight::bold, t::kQueueHead, white(), px, t::kQueueTop, "Now playing");
        const float ry = t::kQueueTop + 28.0f;
        cover(vg, m.track.cover, m.track.cover_colour, px, ry, t::kQueueArt, 4.0f);
        text(vg, Weight::bold, 26.0f, white(), tx, ry + 36.0f, fit(vg, Weight::bold, 26.0f, m.track.title, tw - 40.0f));
        text(vg, Weight::medium, 22.0f, grey(), tx, ry + 68.0f, fit(vg, Weight::medium, 22.0f, m.track.artist, tw - 40.0f));
        equaliser(vg, px + inner - 26.0f, ry + t::kQueueArt / 2 - 13.0f, 26.0f, m.playing ? clock_ : 0.0f,
                  rgba(t::kGreen[0], t::kGreen[1], t::kGreen[2]));
        const float head2 = ry + t::kQueueArt + 74.0f;
        text(vg, Weight::bold, t::kQueueHead, white(), px, head2,
             m.context_name.empty() ? std::string("Next in queue")
                                    : fit(vg, Weight::bold, t::kQueueHead, "Next from " + m.context_name, inner));

        // the list, scrolled to keep the selection in view
        const float list_top = head2 + 28.0f, list_bottom = t::kH - 24.0f;
        const float view = list_bottom - list_top;
        const float want = std::max(0.0f, (queue_sel_ + 1) * t::kQueueRow - view + 12.0f);
        queue_scroll_ = approach(queue_scroll_, want, dt, 12.0f);
        nvgScissor(vg, x, list_top - 8.0f, w + 90.0f, view + 8.0f);
        for (size_t i = 0; i < m.queue.size(); ++i)
        {
            const float top = list_top + i * t::kQueueRow - queue_scroll_;
            if (top > list_bottom || top + t::kQueueRow < list_top - 8.0f)
                continue;
            const bool sel = queue_open_ && static_cast<int>(i) == queue_sel_;
            const bool fill = look_.focus == Look::Focus::fill;
            if (sel)
            {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, px - 20.0f, top - 10.0f, inner + 40.0f, t::kQueueArt + 20.0f, 14.0f);
                if (fill)
                {
                    nvgFillColor(vg, white(0.95f));
                    nvgFill(vg);
                }
                else
                {
                    nvgStrokeColor(vg, white(0.95f));
                    nvgStrokeWidth(vg, 2.5f);
                    nvgStroke(vg);
                }
            }
            const bool dark = sel && fill;
            const QueueEntry &q = m.queue[i];
            cover(vg, q.cover, q.cover_colour, px, top, t::kQueueArt, 4.0f);
            text(vg, Weight::bold, 26.0f, dark ? rgba(18, 18, 20) : white(), tx, top + 36.0f,
                 fit(vg, Weight::bold, 26.0f, q.title, tw));
            text(vg, Weight::medium, 22.0f, dark ? rgba(60, 60, 64) : grey(), tx, top + 68.0f,
                 fit(vg, Weight::medium, 22.0f, q.artist, tw));
        }
        nvgRestore(vg);
    }

    void draw_player(NVGcontext *vg, const Model &m, float dt)
    {
        // Shown when there are lyrics, or when asked for on this track (loading or
        // unavailable). An automatic change of song never flashes "unavailable".
        lyr_mix_ = approach(lyr_mix_, (lyrics_on_ && (m.has_lyrics||(lyric_asked_&&m.lyrics_supported))) ? 1.0f : 0.0f, dt, 6.0f);
        queue_mix_ = approach(queue_mix_, queue_open_ ? 1.0f : 0.0f, dt, 6.0f); // unhurried: about half a second
        // The scrub bar and total time pull in to the queue panel's edge while it is open.
        const float full = t::kW - t::kMargin - 84.0f, pulled = t::kQueueX - 56.0f - 84.0f;
        bar_x1_ = full + (pulled - full) * ease_out(queue_mix_);
        // One page: the player on top, the library below. Scrolling under the player fades the player
        // out and the background to grey.
        lib_row_=std::clamp(lib_row_,0,std::max(0,int(lib_rows().size())-1));
        const float target = in_page() ? t::kLibStop + t::kLibPitch * lib_row_ : 0.0f;
        scroll_px_ = approach(scroll_px_, target, dt, 9.0f);
        const float pa = 1.0f - smooth(0.0f, 520.0f, scroll_px_);
        detail_mix_ = approach(detail_mix_, detail_shown() ? 1.0f : 0.0f, dt, 12.0f);
        // The keyboard was closed without a search: back to the search bar, not an empty results page.
        if (loading_ && m.live && m.search.complete && m.search.query.empty() && focus_ == kResults)
        {
            loading_ = false;
            focus_ = kSearch;
            chip_return_ = kLib;
        }
        else if (loading_ && clock_ - load_start_ > kLoadSeconds && (!m.live || m.search.complete))
            loading_ = false;
        // the results come up as the grey screen fades away
        const bool results_up = results_shown() && (!loading_ || (!m.live && clock_ - load_start_ > kLoadSeconds - 0.25f));
        results_mix_ = approach(results_mix_, results_up ? 1.0f : 0.0f, dt, 12.0f);
        // How far the library is covered by another page. One value that rises while either page is
        // up, not the larger of two crossfading ones, which dipped halfway and let the tiles show.
        cover_mix_ = approach(cover_mix_, (detail_shown() || results_shown() || loading_) ? 1.0f : 0.0f, dt, 14.0f);
        draw_background(vg, m, lyr_mix_);
        draw_grey(vg, mix(m.track.cover_colour, m.detail.colour, detail_mix_), smooth(60.0f, 600.0f, scroll_px_));
        if (pa > 0.01f)
        {
            nvgSave(vg);
            nvgTranslate(vg, 0, -scroll_px_);
            draw_lyrics(vg, m, dt, lyr_mix_ * pa);
            nvgSave(vg);
            nvgGlobalAlpha(vg, pa*card_.alpha);
            draw_track(vg, m);
            nvgRestore(vg);
            if (ui_ > 0.01f)
            {
                nvgSave(vg);
                nvgGlobalAlpha(vg, ui_ * pa);
                ctrl_alpha_ = ui_ * pa;
                nvgSave(vg);nvgGlobalAlpha(vg,ui_*pa*card_.alpha);draw_header(vg,m);nvgRestore(vg);
                draw_scrub(vg, m, dt);
                draw_controls(vg, m, dt);
                nvgRestore(vg);
            }
            draw_queue(vg, m, dt);
            nvgRestore(vg);
        }
        nvgSave(vg);
        nvgGlobalAlpha(vg, 1.0f - detail_mix_);
        draw_library(vg, m, dt);
        nvgRestore(vg);
        draw_detail(vg, m, dt);
        draw_results(vg, m, dt);
        draw_chip(vg, m, dt);
        draw_search(vg, m, dt);
        auto notice=clock_<notice_until_?ui_notice_:m.notice;
        if(notice.empty()&&detail_shown())notice=m.detail.error.empty()?m.detail.metadata_error:m.detail.error+" · Cross to retry";
        if(notice.empty()&&m.storage_status=="storage_error")notice="This login could not be remembered. Pair again after restarting the receiver.";
        if(!notice.empty()){
            rrect(vg,t::kW/2,1018,1600,72,18,rgba(10,10,12,.94f));
            text(vg,Weight::medium,23,white(),t::kW/2,1026,fit(vg,Weight::medium,23,notice,1510),Align::centre);
        }
        if (loading_)
        {
            // the screen greyed, with a wheel: fades in, holds, fades out
            const float lt = clock_ - load_start_;
            const float g = smooth(0.0f, 0.22f, lt) * (m.live&&!m.search.complete?1.0f:(1.0f - smooth(kLoadSeconds - 0.25f, kLoadSeconds, lt)));
            nvgBeginPath(vg);
            nvgRect(vg, 0, 0, t::kW, t::kH);
            nvgFillColor(vg, rgba(14, 15, 17, 0.84f * g));
            nvgFill(vg);
            spinner(vg, t::kW / 2, t::kH / 2, 40.0f, clock_, white(g));
        }
    }

    // The grey page under the player, with a faint glow of a song's colour in the top corner.
    void draw_grey(NVGcontext *vg, Rgb tint, float a)
    {
        if (a < 0.005f)
            return;
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, t::kW, t::kH);
        nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, t::kH, rgba(34, 36, 40, a), rgba(16, 16, 18, a)));
        nvgFill(vg);
        float h, sat, v;
        to_hsv(tint, &h, &sat, &v);
        const Rgb glow = from_hsv(h, std::clamp(sat, 0.45f, 0.85f), 0.62f);
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, t::kW, t::kH);
        nvgFillPaint(vg, nvgRadialGradient(vg, t::kW - 40.0f, -60.0f, 60.0f, 1150.0f, rgb(glow, 0.34f * a), rgb(glow, 0)));
        nvgFill(vg);
    }

    void draw_tile(NVGcontext *vg, const Tile &tl, float cx, float cy, float focus, float size = t::kTile)
    {
        const float s = size, x = cx - s / 2, y = cy - s / 2;
        const float k = s / t::kTile;
        if (tl.kind == Tile::Kind::liked)
        {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, s, s, t::kTileRadius);
            nvgFillPaint(vg, nvgLinearGradient(vg, x, y, x + s, y + s, rgba(80, 24, 230), rgba(150, 224, 196)));
            nvgFill(vg);
            icon(vg, Icon::heart_filled, cx, cy, s * 0.40f, white());
        }
        else
        {
            cover(vg, tl.cover, tl.colour, x, y, s, t::kTileRadius);
            if (tl.kind == Tile::Kind::playlist && size <= t::kTile + 1.0f)
            {
                // playlist covers carry their name, bottom-left
                nvgBeginPath(vg);
                nvgRoundedRect(vg, x, y + s * 0.55f, s, s * 0.45f, t::kTileRadius);
                nvgFillPaint(vg, nvgLinearGradient(vg, 0, y + s * 0.55f, 0, y + s, nvgRGBAf(0, 0, 0, 0), nvgRGBAf(0, 0, 0, 0.55f)));
                nvgFill(vg);
                text(vg, Weight::bold, 27.0f * k, white(), x + 18.0f * k, y + s - 20.0f * k,
                     fit(vg, Weight::bold, 27.0f * k, tl.name, s - 36.0f * k));
            }
        }
        if (focus > 0.02f)
        {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x - 6.0f, y - 6.0f, s + 12.0f, s + 12.0f, t::kTileRadius + 6.0f);
            nvgStrokeColor(vg, rgba(t::kGreen[0], t::kGreen[1], t::kGreen[2], std::min(1.0f, focus * 1.2f)));
            nvgStrokeWidth(vg, 5.0f);
            nvgStroke(vg);
        }
    }

    // The sections under the player: label, the focused item's name as a heading, a row of tiles.
    void draw_library(NVGcontext *vg, const Model &m, float dt)
    {
        const auto rows = lib_rows();
        if(rows.empty()&&in_page()&&!detail_shown()&&!results_shown()){
            text(vg,Weight::medium,30,white(),t::kMargin,400,m.library_loading?"Loading your library...":m.library_error.empty()?"Your library is empty":"Your library is unavailable");
            text(vg,Weight::regular,24,grey(),t::kMargin,450,fit(vg,Weight::regular,24,m.library_error,1500));
            if(!m.library_loading)text(vg,Weight::medium,24,white(.8f),t::kMargin,508,m.library_error.empty()?"Press Up to search for music":"Cross to retry · Up to search");
        }
        nvgSave(vg);
        nvgTranslate(vg, 0, -scroll_px_);
        for (size_t k = 0; k < rows.size() && k < 3; ++k)
        {
            const std::vector<Tile> &tiles = *rows[k].tiles;
            const int n = static_cast<int>(tiles.size());
            const float top = t::kLibLabel + k * t::kLibPitch;
            const bool act = focus_ == kLib && lib_row_ == static_cast<int>(k);
            lib_mix_[k] = approach(lib_mix_[k], act ? 1.0f : 0.0f, dt, 14.0f);
            // the heading of the row you were on stays up while the top bar has the focus
            const bool head = (act || (top_bar() && chip_return_ == kLib)) && lib_row_ == static_cast<int>(k);
            head_mix_[k] = approach(head_mix_[k], head ? 1.0f : 0.0f, dt, 14.0f);
            lib_pos_[k] = approach(lib_pos_[k], static_cast<float>(lib_col_[k]), dt, 18.0f);
            lib_col_[k]=std::clamp(lib_col_[k],0,n-1);
            lib_first_[k]=tile_window(lib_first_[k],lib_col_[k],n);
            lib_x_[k] = approach(lib_x_[k], lib_first_[k] * t::kTilePitch, dt, 12.0f);
            // Tiles are made ready a few at a time, a screenful plus a short run ahead of the focus,
            // so a long row costs nothing until the user heads that way. Each fades in as it is ready.
            const float want_ready = static_cast<float>(lib_col_[k]) + 10.0f;
            if (lib_ready_[k] < want_ready)
                lib_ready_[k] = std::min(want_ready, lib_ready_[k] + dt * 12.0f);
            if (top - scroll_px_ > t::kH + 40.0f || top + 560.0f - scroll_px_ < -40.0f)
                continue;
            // a row sliding up under the header fades out before it reaches it
            nvgSave(vg);
            const float row_alpha = smooth(200.0f, 400.0f, top + t::kLibTilesDy + t::kTile - scroll_px_) * (1.0f - cover_mix_);
            nvgGlobalAlpha(vg, row_alpha);
            // the first label peeks above the bottom edge of the player, going with its controls
            float label_a = 1.0f;
            if (k == 0)
                label_a = ui_ + (1.0f - ui_) * smooth(100.0f, 400.0f, scroll_px_);
            text(vg, Weight::semibold, t::kLibLabelSize, grey(0.95f * label_a), t::kLibX, top, rows[k].label);
            const int col = std::clamp(lib_col_[k], 0, n - 1);
            const std::string name = tiles[col].name;
            library_heading_[k].update(tiles[col].uri+name,name,dt);
            if (head_mix_[k] > 0.01f)
                text(vg, Weight::bold, t::kLibHeadingSize, white(head_mix_[k]*library_heading_[k].alpha), t::kLibX - 3.0f, top + t::kLibHeadingDy,
                     fit(vg, Weight::bold, t::kLibHeadingSize, library_heading_[k].value, t::kW - 2 * t::kLibX));
            const float cy = top + t::kLibTilesDy + t::kTile / 2;
            const int first=std::max(0,int((lib_x_[k]-t::kLibX-t::kTile*1.5f)/t::kTilePitch));
            const int last=std::min(n,int((lib_x_[k]+t::kW+t::kTile-t::kLibX)/t::kTilePitch)+1);
            for (int i = first; i < last; ++i)
            {
                const float cx = t::kLibX + t::kTile / 2 + i * t::kTilePitch - lib_x_[k];
                if (cx < -t::kTile || cx > t::kW + t::kTile)
                    continue;
                const float f = lib_mix_[k] * std::clamp(1.0f - std::fabs(i - lib_pos_[k]), 0.0f, 1.0f);
                const float ready = std::clamp(lib_ready_[k] - i, 0.0f, 1.0f);
                if (ready < 1.0f) // not ready yet: a quiet block where the cover will fade in
                {
                    nvgBeginPath(vg);
                    nvgRoundedRect(vg, cx - t::kTile / 2, cy - t::kTile / 2, t::kTile, t::kTile, t::kTileRadius);
                    nvgFillColor(vg, white(0.07f));
                    nvgFill(vg);
                }
                if (ready > 0.0f)
                {
                    nvgSave(vg);
                    nvgGlobalAlpha(vg, ready * row_alpha);
                    draw_tile(vg, tiles[i], cx, cy, f);
                    nvgRestore(vg);
                }
            }
            nvgRestore(vg);
        }
        nvgRestore(vg);
    }

    // The song select screen of an open album or playlist: its cover and details on the left,
    // the songs as a scrolling list on the right (the tvOS app's playlist screen).
    void draw_detail(NVGcontext *vg, const Model &m, float dt)
    {
        const float a = detail_mix_;
        if (a < 0.01f)
            return;
        const Detail &d = m.detail;
        if(d.uri!=detail_uri_){detail_uri_=d.uri;detail_reveal_=0;detail_ready_at_=0;}
        if(!d.loading&&detail_ready_at_==0)detail_ready_at_=clock_;
        bool art_wait=false;
        if(!d.cover.empty())art_wait=!art::image(vg,d.cover);
        for(std::size_t i=0;i<std::min<std::size_t>(6,d.songs.size());++i)if(!d.songs[i].cover.empty())art_wait=(!art::image(vg,d.songs[i].cover))||art_wait;
        art_wait=art_wait&&m.live&&detail_reveal_==0&&clock_-detail_ready_at_<1.2f;
        if(d.loading||art_wait||(!d.error.empty()&&d.songs.empty())){
            nvgSave(vg);nvgGlobalAlpha(vg,a);
            if(d.loading||art_wait){spinner(vg,t::kW/2,450,34,clock_,white(.8f));text(vg,Weight::medium,26,grey(),t::kW/2,530,"Loading playlist details…",Align::centre);}
            else {text(vg,Weight::bold,34,white(),t::kMargin,300,"Could not load this list");
                  text(vg,Weight::regular,24,grey(),t::kMargin,352,fit(vg,Weight::regular,24,d.error,1500));
                  text(vg,Weight::medium,24,white(.8f),t::kMargin,414,"Cross to retry · Circle to go back");}
            nvgRestore(vg);return;
        }
        detail_reveal_=std::min(1.f,detail_reveal_+dt/0.28f);
        const bool is_playlist = d.kind != Tile::Kind::album;
        nvgSave(vg);
        nvgGlobalAlpha(vg, a*ease_out(detail_reveal_));

        // left: cover, name, who, how many, a few words, then the buttons
        const float lx = t::kMargin, cs = 420.0f, cy = 196.0f; // lx: the now-playing chip's left edge
        Tile tl;
        tl.kind = d.kind;
        tl.name = d.title;
        tl.colour = d.colour;
        tl.cover = d.cover;
        draw_tile(vg, tl, lx + cs / 2, cy + cs / 2, 0.0f, cs);
        const float tw = 520.0f;
        text(vg, Weight::bold, 40.0f, white(), lx, cy + cs + 70.0f, fit(vg, Weight::bold, 40.0f, d.title, tw));
        // who made it, then the counts: small, regular, and left-aligned with the cover and title
        text(vg, Weight::regular, 22.0f, white(0.85f), lx, cy + cs + 118.0f, fit(vg, Weight::regular, 22.0f, d.owner, tw));
        text(vg, Weight::regular, 22.0f, grey(), lx, cy + cs + 154.0f, d.meta);
        if (!d.description.empty())
        {
            text_width(vg, Weight::regular, 22.0f, "x"); // sets the font for the line breaker
            NVGtextRow rows[3];
            const int n = nvgTextBreakLines(vg, d.description.c_str(), nullptr, tw, rows, 3);
            for (int i = 0; i < n; ++i)
                text(vg, Weight::regular, 22.0f, grey(0.85f), lx, cy + cs + 210.0f + i * 34.0f,
                     std::string(rows[i].start, rows[i].end));
        }
        const bool fill = look_.focus == Look::Focus::fill;

        // right: the songs
        const float x0 = 760.0f, x1 = 1808.0f, top = 190.0f, bottom = 1030.0f, pitch = 118.0f, rowh = 100.0f;
        const int n = static_cast<int>(d.songs.size());
        const float want = std::max(0.0f, (det_sel_ + 1) * pitch - (bottom - top) + 24.0f);
        det_scroll_ = approach(det_scroll_, det_zone_ == 2 ? want : det_scroll_, dt, 12.0f);
        nvgSave(vg);
        nvgScissor(vg, x0 - 24.0f, top - 12.0f, x1 - x0 + 48.0f, bottom - top + 12.0f);
        for (int i = std::max(0,int((det_scroll_-rowh-12)/pitch)); i < std::min(n,int((det_scroll_+bottom-top)/pitch)+2); ++i)
        {
            const float y = top + i * pitch - det_scroll_;
            if (y > bottom || y + rowh < top - 12.0f)
                continue;
            const Song &sg = d.songs[i];
            const bool sel = det_zone_ == 2 && focus_ == kDetail && i == det_sel_;
            const bool now = i == d.playing;
            const bool dark = sel && fill;
            if (sel)
            {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, x0 - 6.0f, y - 4.0f, x1 - x0 + 12.0f, rowh + 8.0f, 14.0f);
                if (fill)
                {
                    nvgFillColor(vg, white(0.95f));
                    nvgFill(vg);
                }
                else
                {
                    nvgStrokeColor(vg, white(0.95f));
                    nvgStrokeWidth(vg, 2.5f);
                    nvgStroke(vg);
                }
            }
            float tx = x0 + 22.0f;
            press_feedback(vg,sg.uri,x0-6,y-4,x1-x0+12,rowh+8,dark);
            if (is_playlist)
            {
                cover(vg, sg.cover, sg.colour, x0 + 14.0f, y + (rowh - 76.0f) / 2, 76.0f, 5.0f);
                tx = x0 + 14.0f + 76.0f + 26.0f;
            }
            else
            {
                char num[8];
                std::snprintf(num, sizeof num, "%d", i + 1);
                if (now)
                    equaliser(vg, x0 + 26.0f, y + rowh / 2 - 12.0f, 24.0f, m.playing ? clock_ : 0.0f,
                              rgba(t::kGreen[0], t::kGreen[1], t::kGreen[2]));
                else
                    text(vg, Weight::medium, 26.0f, dark ? rgba(60, 60, 64) : grey(), x0 + 38.0f, y + rowh / 2 + 9.0f, num, Align::centre);
                tx = x0 + 96.0f;
            }
            const NVGcolor tc = dark ? rgba(18, 18, 20) : (now ? rgba(t::kGreen[0], t::kGreen[1], t::kGreen[2]) : white());
            const NVGcolor sc = dark ? rgba(60, 60, 64) : grey();
            const float room = x1 - tx - 150.0f;
            // title and artist on every row, in albums too (the track number stands in for the cover)
            text(vg, Weight::medium, 27.0f, tc, tx, y + 42.0f, fit(vg, Weight::medium, 27.0f, sg.title, room));
            text(vg, Weight::medium, 22.0f, sc, tx, y + 76.0f, fit(vg, Weight::medium, 22.0f, sg.artist, room));
            char dur[16];
            const int secs = sg.duration_ms / 1000;
            std::snprintf(dur, sizeof dur, "%d:%02d", secs / 60, secs % 60);
            if (secs > 0) text(vg, Weight::medium, 26.0f, sc, x1 - 24.0f, y + rowh / 2 + 9.0f, dur, Align::right);
            if (is_playlist && now)
                equaliser(vg, x1 - 120.0f, y + rowh / 2 - 12.0f, 24.0f, m.playing ? clock_ : 0.0f,
                          rgba(t::kGreen[0], t::kGreen[1], t::kGreen[2]));
        }
        nvgRestore(vg);
        // a thin scroll bar for a long list
        if (n * pitch > bottom - top)
        {
            const float total = n * pitch, view = bottom - top;
            const float th = std::max(60.0f, view * view / total);
            const float ty = top + (view - th) * (det_scroll_ / (total - view));
            nvgBeginPath(vg);
            nvgRoundedRect(vg, 1846.0f, top, 4.0f, view, 2.0f);
            nvgFillColor(vg, white(0.14f));
            nvgFill(vg);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, 1846.0f, ty, 4.0f, th, 2.0f);
            nvgFillColor(vg, white(0.85f));
            nvgFill(vg);
        }
        nvgRestore(vg);
    }

    // The little search bar in the top right of the library page: a pill with a magnifier and a
    // label, white when focused like every other control.
    void draw_search(NVGcontext *vg, const Model &m, float dt)
    {
        focus_mix_[kSearch] = approach(focus_mix_[kSearch], focus_ == kSearch ? 1.0f : 0.0f, dt);
        const float a = smooth(450.0f, 700.0f, scroll_px_);
        if (a < 0.01f)
            return;
        const float f = focus_mix_[kSearch];
        const bool fill = look_.focus == Look::Focus::fill;
        const bool query = results_shown() && !m.search.query.empty();
        const float w = 250.0f, h = 62.0f, cx = t::kW - t::kMargin - w / 2, cy = 104.0f;
        nvgSave(vg);
        nvgGlobalAlpha(vg, a);
        rrect(vg, cx, cy, w, h, h / 2, fill ? mix(white(0.10f), white(1.0f), f) : white(0.10f));
        if (!fill && f > 0.01f)
        {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, cx - w / 2 - 5.0f, cy - h / 2 - 5.0f, w + 10.0f, h + 10.0f, h / 2 + 5.0f);
            nvgStrokeColor(vg, white(f));
            nvgStrokeWidth(vg, 2.5f);
            nvgStroke(vg);
        }
        const NVGcolor ink = fill ? mix(white(0.95f), rgba(18, 18, 20), f) : white(0.95f);
        icon(vg, Icon::search, cx - w / 2 + 42.0f, cy, 32.0f, ink);
        const NVGcolor label = query ? ink : (fill ? mix(grey(), rgba(18, 18, 20), f) : grey());
        text(vg, Weight::semibold, 26.0f, label, cx - w / 2 + 74.0f, cy + 9.0f,
             query ? fit(vg, Weight::semibold, 26.0f, m.search.query, w - 110.0f) : std::string("Search"));
        nvgRestore(vg);
    }

    // A thin scroll bar for a column that is longer than its window.
    void scroll_bar(NVGcontext *vg, float x, float top, float view, float total, float offset)
    {
        if (total <= view)
            return;
        const float th = std::max(60.0f, view * view / total);
        const float ty = top + (view - th) * (offset / (total - view));
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, top, 4.0f, view, 2.0f);
        nvgFillColor(vg, white(0.14f));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, ty, 4.0f, th, 2.0f);
        nvgFillColor(vg, white(0.85f));
        nvgFill(vg);
    }

    // What the search found: songs on the left, albums on the right. Both scroll.
    void draw_results(NVGcontext *vg, const Model &m, float dt)
    {
        const float a = results_mix_;
        if (a < 0.01f)
            return;
        const SearchResults &r = m.search;
        if(m.live&&r.complete&&!r.error.empty()){
            nvgSave(vg);nvgGlobalAlpha(vg,a);
            text(vg,Weight::semibold,34,white(),t::kMargin,282,"Search unavailable");
            text(vg,Weight::regular,23,grey(),t::kMargin,330,fit(vg,Weight::regular,23,r.error,t::kW-2*t::kMargin));
            text(vg,Weight::regular,22,grey(),t::kMargin,372,"Your music keeps playing. Use Search to try again.");
            nvgRestore(vg);return;
        }
        const bool fill = look_.focus == Look::Focus::fill;
        const bool here = focus_ == kResults;
        const float top = 296.0f, bottom = 1040.0f, view = bottom - top;
        nvgSave(vg);
        nvgGlobalAlpha(vg, a);

        // songs
        const float sx = t::kMargin, sw = 1040.0f, pitch = 110.0f, rowh = 96.0f;
        text(vg, Weight::semibold, t::kLibLabelSize, grey(0.95f), sx, 248.0f, "Songs");
        const int ns = static_cast<int>(r.songs.size());
        const float want_s = std::max(0.0f, (res_sel_ + 1) * pitch - view + 24.0f);
        res_scroll_s_ = approach(res_scroll_s_, res_zone_ == 0 ? want_s : res_scroll_s_, dt, 12.0f);
        nvgSave(vg);
        nvgScissor(vg, sx - 24.0f, top - 12.0f, sw + 48.0f, view + 12.0f);
        for (int i = std::max(0,int((res_scroll_s_-rowh-12)/pitch)); i < std::min(ns,int((res_scroll_s_+view)/pitch)+2); ++i)
        {
            const Song &sg = r.songs[i];
            const float y = top + i * pitch - res_scroll_s_;
            if (y > bottom || y + rowh < top - 12.0f)
                continue;
            const bool sel = here && res_zone_ == 0 && i == res_sel_;
            const bool dark = sel && fill;
            if (sel)
            {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, sx - 18.0f, y - 6.0f, sw + 36.0f, rowh + 12.0f, 14.0f);
                if (fill)
                {
                    nvgFillColor(vg, white(0.95f));
                    nvgFill(vg);
                }
                else
                {
                    nvgStrokeColor(vg, white(0.95f));
                    nvgStrokeWidth(vg, 2.5f);
                    nvgStroke(vg);
                }
            }
            press_feedback(vg,sg.uri,sx-18,y-6,sw+36,rowh+12,dark);
            const float tx = sx + 14.0f, room = sw - 14.0f - 110.0f;
            text(vg, Weight::medium, 27.0f, dark ? rgba(18, 18, 20) : white(), tx, y + 42.0f,
                 fit(vg, Weight::medium, 27.0f, sg.title, room));
            text(vg, Weight::medium, 22.0f, dark ? rgba(60, 60, 64) : grey(), tx, y + 76.0f,
                 fit(vg, Weight::medium, 22.0f, sg.artist, room));
            char dur[16];
            const int secs = sg.duration_ms / 1000;
            std::snprintf(dur, sizeof dur, "%d:%02d", secs / 60, secs % 60);
            if (secs > 0) text(vg, Weight::medium, 26.0f, dark ? rgba(60, 60, 64) : grey(), sx + sw - 6.0f, y + rowh / 2 + 9.0f, dur, Align::right);
        }
        nvgRestore(vg);
        scroll_bar(vg, sx + sw + 44.0f, top, view, ns * pitch, res_scroll_s_);

        // albums, two across
        const float tile = 240.0f, gapx = 56.0f, rowp = 368.0f;
        const float ax = t::kW - t::kMargin - (2 * tile + gapx); // the grid ends at the right margin
        text(vg, Weight::semibold, t::kLibLabelSize, grey(0.95f), ax, 248.0f, "Albums");
        const int na = static_cast<int>(r.albums.size());
        const float row_h = tile + 96.0f; // a tile with its two text lines
        const float want_a = std::max(0.0f, (res_alb_ / 2) * rowp + row_h - view + 12.0f);
        res_scroll_a_ = approach(res_scroll_a_, res_zone_ == 1 ? want_a : res_scroll_a_, dt, 12.0f);
        nvgSave(vg);
        nvgScissor(vg, ax - 24.0f, top - 16.0f, 2 * tile + gapx + 48.0f, view + 16.0f);
        for (int i = 0; i < na; ++i)
        {
            const Tile &tl = r.albums[i];
            const float x = ax + (i % 2) * (tile + gapx), y = top + (i / 2) * rowp - res_scroll_a_;
            if (y > bottom || y + row_h < top - 16.0f)
                continue;
            const int slot = i % 8;
            res_alb_f_[slot] = approach(res_alb_f_[slot], (here && res_zone_ == 1 && i == res_alb_) ? 1.0f : 0.0f, dt, 18.0f);
            draw_tile(vg, tl, x + tile / 2, y + tile / 2, res_alb_f_[slot], tile);
            text(vg, Weight::medium, 24.0f, white(), x, y + tile + 52.0f, fit(vg, Weight::medium, 24.0f, tl.name, tile));
            text(vg, Weight::regular, 20.0f, grey(), x, y + tile + 86.0f, fit(vg, Weight::regular, 20.0f, tl.sub, tile));
        }
        nvgRestore(vg);
        scroll_bar(vg, t::kW - t::kMargin + 38.0f, top, view, ((na + 1) / 2) * rowp, res_scroll_a_);
        nvgRestore(vg);
    }

    // Once the player has scrolled away: the track and "Your Library" stay at the top.
    void draw_chip(NVGcontext *vg, const Model &m, float dt)
    {
        focus_mix_[kChip] = approach(focus_mix_[kChip], focus_ == kChip ? 1.0f : 0.0f, dt);
        const float f = focus_mix_[kChip];
        const bool fill = look_.focus == Look::Focus::fill;
        const float a = smooth(450.0f, 700.0f, scroll_px_);
        if (a < 0.01f)
            return;
        nvgSave(vg);
        nvgGlobalAlpha(vg, a);
        const float x = t::kMargin, y = 60.0f, w = 470.0f, h = 88.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, w, h, 14.0f);
        nvgFillColor(vg, fill ? mix(rgba(46, 48, 52, 0.96f), white(), f) : rgba(46, 48, 52, 0.96f));
        nvgFill(vg);
        if (!fill && f > 0.01f)
        {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x - 5.0f, y - 5.0f, w + 10.0f, h + 10.0f, 19.0f);
            nvgStrokeColor(vg, white(f));
            nvgStrokeWidth(vg, 2.5f);
            nvgStroke(vg);
        }
        const NVGcolor ink = fill ? mix(white(), rgba(18, 18, 20), f) : white();
        if (card_.value.track.title.empty() && m.track.uri.empty())
        {
            // Nothing is playing: say so, with the app's note mark where the cover would be.
            const float tile = h - 24.0f;
            rrect(vg, x + 12.0f + tile / 2, y + 12.0f + tile / 2, tile, tile, 5.0f, fill ? mix(white(0.10f), white(0.0f), f) : white(0.10f));
            icon(vg, Icon::note, x + 12.0f + tile / 2, y + 12.0f + tile / 2, 30.0f, ink);
            text(vg, Weight::bold, 26.0f, ink, x + h + 4.0f, y + h / 2 + 9.0f, "Not Playing");
        }
        else
        {
        nvgSave(vg);nvgGlobalAlpha(vg,a*card_.alpha);
        cover(vg, card_.value.track.cover, card_.value.track.cover_colour, x + 12.0f, y + 12.0f, h - 24.0f, 5.0f);
        const float tx = x + h + 4.0f;
        text(vg, Weight::bold, 24.0f, ink, tx, y + 40.0f,
             fit(vg, Weight::bold, 24.0f, card_.value.track.title, w - h - 70.0f));
        text(vg, Weight::medium, 18.0f, fill ? mix(grey(), rgba(60, 60, 64), f) : grey(), tx, y + 66.0f,
             fit(vg, Weight::medium, 18.0f, card_.value.track.artist, w - h - 70.0f));
        equaliser(vg, x + w - 38.0f, y + h / 2 - 11.0f, 22.0f, m.playing ? clock_ : 0.0f,
                  rgba(t::kGreen[0], t::kGreen[1], t::kGreen[2]));
        nvgRestore(vg);
        }
        text(vg, Weight::medium, 30.0f, white(), t::kW / 2, y + 52.0f, results_shown() ? "Search results" : "Your Library", Align::centre);
        nvgRestore(vg);
    }

    // Idle (librespot running, nothing playing) and the not-connected screens: plain, same style.
    void draw_status(NVGcontext *vg, const Model &m)
    {
        nvgBeginPath(vg);
        nvgRect(vg, 0, 0, t::kW, t::kH);
        nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, t::kH, rgba(22, 26, 34), rgba(10, 12, 16)));
        nvgFill(vg);
        const float cx = t::kW / 2, cy = 470.0f;
        std::string head, sub1, sub2;
        bool busy = false;
        switch (m.link)
        {
        case Link::idle:
            if(m.auth_status=="connecting"||m.auth_status=="retrying"){
                head="Connecting to Spotify";sub1="Restoring your saved login...";busy=true;break;
            }
            head = m.auth_status=="reauth_required"?"Pair Spotify again":"Pair Spotify once";
            sub1 = "Open Spotify, tap the devices icon and choose";
            break;
        case Link::waiting_kabuki:
            head = "Kabuki isn't running";
            sub1 = "Start Kabuki on your PS5 and music will begin.";
            sub2 = "Reconnecting...";
            busy = true;
            break;
        default:
            head = "Can't reach librespot";
            sub1 = "Start librespot-ps5 on your PS5.";
            sub2 = "Reconnecting...";
            busy = true;
            break;
        }
        if (busy)
            spinner(vg, cx, cy - 120.0f, 34.0f, clock_, white(0.8f));
        else
        {
            nvgBeginPath(vg);
            nvgCircle(vg, cx, cy - 120.0f, 44.0f);
            nvgFillColor(vg, rgba(0xB3, 0xB3, 0xB3, 0.92f));
            nvgFill(vg);
            icon(vg, Icon::note, cx, cy - 120.0f, 50.0f, rgba(20, 24, 30));
        }
        text(vg, Weight::bold, 56.0f, white(), cx, cy, head, Align::centre);
        text(vg, Weight::medium, 28.0f, grey(), cx, cy + 56.0f, sub1, Align::centre);
        if (!sub2.empty())
            text(vg, Weight::medium, 24.0f, white(0.5f), cx, cy + 104.0f, sub2, Align::centre);
        if (m.link == Link::idle)
        {
            const float w = text_width(vg, Weight::medium, 30.0f, m.device) + 96.0f;
            rrect(vg, cx, cy + 128.0f, w, 64.0f, 32.0f, white(0.1f));
            icon(vg, Icon::note, cx - w / 2 + 40.0f, cy + 128.0f, 30.0f, white(0.9f));
            text(vg, Weight::medium, 30.0f, white(), cx - w / 2 + 68.0f, cy + 139.0f, m.device);
        }
    }

    void press_feedback(NVGcontext* vg,const std::string& uri,float x,float y,float w,float h,bool dark){
        const float elapsed=clock_-pressed_at_;
        if(uri.empty()||uri!=pressed_uri_||elapsed<0||elapsed>=.24f)return;
        const float pulse=std::sin(elapsed/.24f*3.14159265f);
        nvgBeginPath(vg);nvgRoundedRect(vg,x,y,w,h,14);
        nvgFillColor(vg,dark?rgba(0,0,0,.14f*pulse):white(.18f*pulse));nvgFill(vg);
    }
    std::string pressed_uri_;float pressed_at_=-1;
    App *app_;
    Look look_;
    std::function<void()> quit_;
    Clock::time_point last_;
    Clock::time_point scrub_input_{};
    float clock_ = 0;
    int held_ = 0;
    float idle_ = 0, ui_ = 1.0f, bar_x1_ = t::kBarX1; // seconds since input; how visible the controls are
    bool hidden_ = false;
    float scroll_px_ = 0;       // how far the page has scrolled under the player
    int det_zone_ = 1, det_sel_ = 0; // open album/playlist: 0 add, 1 play, 2 the song list; the selected song
    std::string detail_uri_;float detail_reveal_=0,detail_ready_at_=0;
    float det_scroll_ = 0, detail_mix_ = 0, play_focus_ = 0;
    bool loading_ = false;
    float load_start_ = 0;
    static constexpr float kLoadSeconds = 1.3f; // how long the preview pretends to search
    float res_scroll_s_ = 0, res_scroll_a_ = 0;
    Target chip_return_ = kLib; // where Down from the top bar goes back to: the library, a list or the results
    Target detail_back_ = kLib; // where Circle in an open list goes back to
    int res_zone_ = 0, res_sel_ = 0, res_alb_ = 0; // results: 0 songs, 1 albums; the selected song and album
    float res_alb_f_[8] = {0, 0, 0, 0, 0, 0, 0, 0}, results_mix_ = 0, cover_mix_ = 0, head_mix_[3] = {0, 0, 0};
    int lib_row_ = 0, lib_col_[3] = {0, 0, 0}, lib_first_[3]={0,0,0};
    float lib_ready_[3] = {10, 10, 10};
    float lib_pos_[3] = {0, 0, 0}, lib_mix_[3] = {0, 0, 0}, lib_x_[3] = {0, 0, 0};
    float shown_frac_ = 0;
    bool bar_first_ = true;

    Target focus_ = kPlay;
    struct CardData {Track track;std::string kind,context;};
    FadeSnapshot<CardData> card_;
    Rgb tint_{.08f,.1f,.13f};std::string track_uri_;
    int seek_target_=-1;std::string seek_uri_;Clock::time_point seek_sent_{};
    FadeSnapshot<std::string> library_heading_[3];
    struct LyricBlock{float top=0,h=0;std::vector<std::string> rows;};
    std::vector<LyricBlock> lyric_blocks_;std::string lyric_layout_key_;
    float lyric_height_=0;std::string ui_notice_;float notice_until_=0;
    bool idle_library_=false;
    bool opt_shuffle_ = false, opt_shuffle_on_ = false, opt_repeat_on_ = false;
    Repeat opt_repeat_ = Repeat::off;
    Clock::time_point opt_shuffle_at_{}, opt_repeat_at_{};
    float ctrl_alpha_ = 1.0f, lyrics_pill_ = 0.0f;
    bool lyrics_on_ = false,lyric_asked_=false;
    Lyrics lyric_hold_;std::string lyric_hold_uri_,lyric_track_;float lyric_hold_until_=0;
    std::uint64_t lyric_hold_revision_=0;
    bool queue_open_ = false;
    int queue_sel_ = 0;
    float focus_mix_[kTargets] = {};
    float scrub_focus_ = 0, lyr_mix_ = 0, queue_mix_ = 0, queue_scroll_ = 0, lyr_scroll_ = 0, hl_alpha_ = 0;
    bool hl_first_ = true, lyr_first_ = true;
    Box hl_{}, lyrics_box_{}, queue_box_{};
};

Screen *make_screen(App *app, Look look, std::function<void()> quit)
{
    return new Screen(app, look, std::move(quit));
}

brls::View *as_view(Screen *screen)
{
    return screen;
}

void give_focus(Screen *screen)
{
    brls::Application::giveFocus(screen);
}

void set_clock_running(Screen *screen, bool running)
{
    screen->clock_running = running;
}

} // namespace player
