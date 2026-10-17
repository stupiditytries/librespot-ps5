// The player screens on a PC: the real views and drawing in a GLFW window (or as PNGs), with
// invented sample data, for mock-ups. The host is borealis' own, as in the overlay preview.
//
// usage: player_preview <output folder> [options] [steps...]
//
//   --size W H        the window (1920 1080; 3840 2160 for the 4K look)
//   --sample NAME     playing (default), paused, long, nolyrics, queue-long, shuffle-on,
//                     idle, kabuki, offline
//   --bg tint|canvas  no-Canvas fallback (the cover's colour) or a Canvas-style still
//   --focus fill|ring the focused-control look (fill: white shape, dark glyph; ring)
//   --no-pill         no pill behind the transport controls
//   --hide-after S    seconds idle before the controls fade away (6; 0 keeps them)
//   --cover FILE      a cover picture for the current track (the colour is taken from it)
//   --canvas FILE     a picture standing in for one frame of the Canvas
//   --fonts DIR       Plus Jakarta Sans files (default: assets/fonts beside the source)
//   --live            the heard position runs (a window); scripted shots hold it still
//
// Steps run in order and end the preview: a button (a b x y up down left right lb rb; x is
// Square, y Triangle, a Cross, b Circle), wait=MS, or shot=NAME (saves <output folder>/NAME.png).
// Without steps the window stays open: arrows, Enter (Cross), Escape (Circle), S (Square), Y
// (Triangle), Q and E (L1, R1).

#include "draw.hpp"
#include "screen.hpp"
#include "link.hpp"

#include <borealis/core/activity.hpp>
#include <borealis/core/application.hpp>
#include <borealis/core/platform.hpp>
#include <borealis/core/theme.hpp>
#include <borealis/platforms/glfw/glfw_input.hpp>
#include <borealis/platforms/glfw/glfw_video.hpp>
#include <glad/glad.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using Clock = std::chrono::steady_clock;

struct Step
{
    enum Kind
    {
        press,
        wait,
        shot,
    } kind = wait;
    int button = 0;
    int ms = 0;
    std::string name;
};

struct Preview
{
    std::string output;
    int width = 1920;
    int height = 1080;
    std::vector<Step> script;
    std::size_t next = 0;
    int held = -1; // the button a step holds down now, as a controller would
    bool failed = false;
    std::string fonts;
} preview;

class Video final : public brls::GLFWVideoContext
{
  public:
    using brls::GLFWVideoContext::GLFWVideoContext;

    std::string capture; // the file the next frame is saved to

    void endFrame() override
    {
        if (!capture.empty())
        {
            int width = 0, height = 0;
            glfwGetFramebufferSize(this->getGLFWWindow(), &width, &height);
            std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * height * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            for (std::size_t i = 3; i < pixels.size(); i += 4)
                pixels[i] = 255;
            stbi_flip_vertically_on_write(1);
            if (!stbi_write_png(capture.c_str(), width, height, 4, pixels.data(), width * 4))
            {
                std::fprintf(stderr, "cannot write %s\n", capture.c_str());
                preview.failed = true;
            }
            capture.clear();
        }
        brls::GLFWVideoContext::endFrame();
    }
};

class Input final : public brls::GLFWInputManager
{
  public:
    explicit Input(GLFWwindow *window) : brls::GLFWInputManager(window), window_(window)
    {
    }

    void updateUnifiedControllerState(brls::ControllerState *state) override
    {
        brls::GLFWInputManager::updateUnifiedControllerState(state);
        const auto key = [this](int code) { return glfwGetKey(window_, code) == GLFW_PRESS; };
        state->buttons[brls::BUTTON_X] |= key(GLFW_KEY_S);
        state->buttons[brls::BUTTON_Y] |= key(GLFW_KEY_Y);
        state->buttons[brls::BUTTON_LB] |= key(GLFW_KEY_Q);
        state->buttons[brls::BUTTON_RB] |= key(GLFW_KEY_E);
        if (preview.held >= 0 && preview.held < brls::_BUTTON_MAX)
            state->buttons[preview.held] = true;
    }

  private:
    GLFWwindow *window_;
};

class Fonts final : public brls::FontLoader
{
  public:
    void loadFonts() override
    {
        player::draw::load_fonts(preview.fonts);
    }
};

class NoKeyboard final : public brls::ImeManager
{
  public:
    bool openForText(std::function<void(std::string)>, std::string, std::string, int, std::string, int) override
    {
        return false;
    }
    bool openForNumber(std::function<void(long)>, std::string, std::string, int, std::string, std::string,
                       std::string, int) override
    {
        return false;
    }
};

class Platform final : public brls::Platform
{
  public:
    Platform()
    {
        glfwInit();
    }

    void createWindow(std::string title, uint32_t, uint32_t, float, float) override
    {
        VideoContext::sizeW = static_cast<uint32_t>(preview.width);
        VideoContext::sizeH = static_cast<uint32_t>(preview.height);
        video_ = std::make_unique<Video>(title, static_cast<uint32_t>(preview.width),
                                         static_cast<uint32_t>(preview.height));
        glfwSetWindowSize(video_->getGLFWWindow(), preview.width, preview.height);
        input_ = std::make_unique<Input>(video_->getGLFWWindow());
        next_ = Clock::now() + std::chrono::milliseconds(700);
    }

    bool mainLoopIteration() override
    {
        glfwPollEvents();
        if (preview.held >= 0 && Clock::now() >= release_)
            preview.held = -1;
        if (!preview.script.empty() && Clock::now() >= next_ && video_->capture.empty())
        {
            if (preview.next >= preview.script.size())
                return false;
            const Step &step = preview.script[preview.next++];
            int pause = 450;
            switch (step.kind)
            {
            case Step::press:
                preview.held = step.button;
                release_ = Clock::now() + std::chrono::milliseconds(80);
                break;
            case Step::wait:
                pause = step.ms;
                break;
            case Step::shot:
                video_->capture = preview.output + "/" + step.name + ".png";
                pause = 0;
                break;
            }
            next_ = Clock::now() + std::chrono::milliseconds(pause);
        }
        return !glfwWindowShouldClose(video_->getGLFWWindow());
    }

    std::string getName() override
    {
        return "Player preview";
    }
    VideoContext *getVideoContext() override
    {
        return video_.get();
    }
    brls::InputManager *getInputManager() override
    {
        return input_.get();
    }
    brls::AudioPlayer *getAudioPlayer() override
    {
        return &audio_;
    }
    brls::ImeManager *getImeManager() override
    {
        return &keyboard_;
    }
    brls::FontLoader *getFontLoader() override
    {
        return &fonts_;
    }
    brls::ThemeVariant getThemeVariant() override
    {
        return brls::ThemeVariant::DARK;
    }
    void setThemeVariant(brls::ThemeVariant) override
    {
    }
    std::string getLocale() override
    {
        return brls::LOCALE_EN_US;
    }
    bool canShowBatteryLevel() override
    {
        return false;
    }
    bool canShowWirelessLevel() override
    {
        return false;
    }
    int getBatteryLevel() override
    {
        return 100;
    }
    bool isBatteryCharging() override
    {
        return false;
    }
    int getWirelessLevel() override
    {
        return 0;
    }
    std::string getIpAddress() override
    {
        return {};
    }
    std::string getDnsServer() override
    {
        return {};
    }
    void disableScreenDimming(bool, const std::string &, const std::string &) override
    {
    }
    bool isScreenDimmingDisabled() override
    {
        return false;
    }
    void setBacklightBrightness(float) override
    {
    }
    float getBacklightBrightness() override
    {
        return 1.0f;
    }
    bool canSetBacklightBrightness() override
    {
        return false;
    }
    bool isApplicationMode() override
    {
        return true;
    }
    void exitToHomeMode(bool) override
    {
    }
    void forceEnableGamePlayRecording() override
    {
    }
    void openBrowser(std::string) override
    {
    }

  private:
    std::unique_ptr<Video> video_;
    std::unique_ptr<Input> input_;
    brls::NullAudioPlayer audio_;
    NoKeyboard keyboard_;
    Fonts fonts_;
    Clock::time_point next_{};
    Clock::time_point release_{};
};

bool parse_step(std::string_view word, Step *step)
{
    struct Name
    {
        std::string_view word;
        int button;
    };
    static constexpr Name kButtons[] = {
        {"a", brls::BUTTON_A},          {"b", brls::BUTTON_B},           {"x", brls::BUTTON_X},
        {"y", brls::BUTTON_Y},          {"up", brls::BUTTON_NAV_UP},     {"down", brls::BUTTON_NAV_DOWN},
        {"left", brls::BUTTON_NAV_LEFT}, {"right", brls::BUTTON_NAV_RIGHT}, {"lb", brls::BUTTON_LB},
        {"rb", brls::BUTTON_RB}};
    for (const Name &name : kButtons)
    {
        if (word == name.word)
        {
            *step = {Step::press, name.button, 0, {}};
            return true;
        }
    }
    if (word.starts_with("wait="))
    {
        *step = {Step::wait, 0, std::atoi(std::string(word.substr(5)).c_str()), {}};
        return true;
    }
    if (word.starts_with("shot=") && word.size() > 5)
    {
        *step = {Step::shot, 0, 0, std::string(word.substr(5))};
        return true;
    }
    return false;
}

// ---- sample data (all invented) --------------------------------------------------------------

using namespace player;

QueueEntry q(const char *title, const char *artist, float h)
{
    return {title, artist, draw::from_hsv(h, 0.65f, 0.75f), {}};
}

Lyrics sample_lyrics(Rgb cover)
{
    Lyrics l;
    l.sync = Lyrics::Sync::line;
    l.provider = "Musixmatch";
    l.background = draw::lyrics_colour(cover);
    l.highlight = {1, 1, 1};
    // Invented words, only so the layout has something to show.
    static const char *kWords[] = {
        "Hold the lantern up a little higher",
        "I can hear the river on the stones",
        "Every window in the valley's lit tonight",
        "and nobody is leaving for home",
        "Carry me across the paper bridge",
        "before the morning folds it flat",
        "We were only ever passing through",
        "but the ground remembers that",
        "So sing it slow, sing it slow",
        "until the whole town knows",
        "Hold the lantern up a little higher",
        "I can hear the river on the stones",
        "and a very long line to check that the wrapping works as it should when words run out of room",
        "Carry me across",
    };
    int t = 9000;
    for (const char *w : kWords)
    {
        l.lines.push_back({t, w});
        t += 4600;
    }
    return l;
}

// What librespot would send when an album or playlist is opened (invented songs).
void build_detail(Model *m, const Tile &tl, int col)
{
    static const char *kTitles[] = {"Lantern Season", "Paper Bridges", "Low Tide Radio",   "Carry Me Across", "Slow Orbit",
                                    "Glass Hours",    "Night Markets", "Hold the Lantern", "Evergreen",       "Kettle Song",
                                    "Afterimage",     "The Long Way",  "Salt and Static",  "Small Hours",     "Cartographer",
                                    "Open Windows",   "Moth Light",    "Quiet Engines",    "Harbour Lights",  "Sundial"};
    static const char *kArtists[] = {"The Nightjars", "Marit Voss", "Alive Harbour", "Dessa Quill", "Westbound", "Corvid", "KELI"};
    Detail &d = m->detail;
    d = Detail{};
    d.kind = tl.kind;
    d.title = tl.name;
    d.colour = tl.colour;
    d.saved = true;
    const int count = tl.kind == Tile::Kind::album ? 11 : (tl.kind == Tile::Kind::liked ? 20 : 16);
    for (int i = 0; i < count; ++i)
    {
        Song sg;
        sg.title = kTitles[(i * 7 + col * 3) % 20];
        sg.artist = tl.kind == Tile::Kind::album ? "The Nightjars" : kArtists[(i * 5 + col) % 7];
        sg.duration_ms = (150 + (i * 37 + col * 11) % 120) * 1000;
        sg.colour = draw::from_hsv(static_cast<float>((i * 47 + col * 29) % 360), 0.65f, 0.75f);
        d.songs.push_back(sg);
    }
    char buf[96];
    if (tl.kind == Tile::Kind::album)
    {
        d.owner = "The Nightjars";
        std::snprintf(buf, sizeof buf, "Album · %d", 2016 + (col * 3) % 10); // the release year
        d.description = "";
    }
    else
    {
        d.owner = tl.kind == Tile::Kind::liked ? "You" : "Made for you";
        std::snprintf(buf, sizeof buf, "Playlist · %d", 2025 - (col % 3)); // the year it was made

        d.description = tl.kind == Tile::Kind::liked ? "" : "Slow, bright songs for long evenings, refreshed every Friday.";
    }
    d.meta = buf;
}

void fill_detail(Model *m, int section, int col)
{
    const std::vector<Tile> &tiles = section == 0 ? m->playlists : m->albums;
    if (col >= 0 && col < static_cast<int>(tiles.size()))
        build_detail(m, tiles[col], col);
}

// What a search for "night" would find (invented).
void fill_search(Model *m)
{
    SearchResults &r = m->search;
    r = SearchResults{};
    r.query = "night";
    static const char *kSongs[] = {"Night Markets",   "Nightjar",           "All Night Long",     "Nightfall",
                                   "Midnight Radio",  "Night Drive",        "Nights in Amber",    "Night Swimming Lights",
                                   "The Night Shift", "Late Night Letters", "Nightingale Row",    "Night Bloom"};
    static const char *kArtists[] = {"The Nightjars", "Marit Voss", "Westbound", "Dessa Quill", "Corvid", "KELI", "Alive Harbour"};
    for (int i = 0; i < 12; ++i)
    {
        Song sg;
        sg.title = kSongs[i];
        sg.artist = kArtists[i % 7];
        sg.duration_ms = (170 + i * 17 % 90) * 1000;
        sg.colour = draw::from_hsv(static_cast<float>(i * 61 % 360), 0.65f, 0.75f);
        r.songs.push_back(sg);
    }
    static const char *kAlbums[] = {"Night Markets", "Nightfall Sessions", "Midnight Radio", "Nightjar",
                                    "Night Shift",   "Amber Nights",       "Nightingale",    "Late Night Letters"};
    for (int i = 0; i < 8; ++i)
    {
        Tile t;
        t.kind = Tile::Kind::album;
        t.name = kAlbums[i];
        t.sub = std::string(kArtists[(i * 3) % 7]) + " \xC2\xB7 " + std::to_string(2015 + (i * 3) % 10);
        t.colour = draw::from_hsv(static_cast<float>(i * 83 % 360 + 200), 0.7f, 0.75f);
        r.albums.push_back(t);
    }
}

void fill_sample(Model *m, const std::string &sample, const std::string &cover, const std::string &canvas)
{
    m->link = Link::connected;
    m->device = "PS5";
    m->track = {"Glass Hours", "The Nightjars", cover, draw::from_hsv(212, 0.7f, 0.7f), 211000, false};
    if (!cover.empty())
        m->track.cover_colour = draw::dominant_colour(cover, m->track.cover_colour);
    m->context_kind = "ALBUM";
    m->context_name = "Paper Bridges";
    m->position_ms = 34000;
    m->playing = true;
    m->has_canvas = true;
    m->canvas_still = canvas;
    m->liked = true;
    m->has_lyrics = true;
    m->lyrics = sample_lyrics(m->track.cover_colour);
    m->queue = {q("Low Tide Radio", "Marit Voss", 20),     q("Too Young to Stay", "Alive Harbour", 350),
                q("Going Somewhere", "The Altons", 175),   q("Melon Season", "Dessa Quill", 300),
                q("Sunflowers in March", "Amy Lorrimer", 45), q("Tigerlight", "Westbound", 190),
                q("Hypnotised", "KELI", 320),              q("Pick It Up", "Michael Minelli", 240),
                q("After the Rain Stops", "Corvid", 90),   q("Lanterns", "Marit Voss", 20)};
    // The library page (invented names).
    auto tile = [](Tile::Kind k, const char *name, float hue)
    {
        Tile t;
        t.kind = k;
        t.name = name;
        t.colour = draw::from_hsv(hue, 0.7f, 0.75f);
        return t;
    };
    m->playlists = {tile(Tile::Kind::liked, "Liked Songs", 280),    tile(Tile::Kind::playlist, "daylist", 30),
                    tile(Tile::Kind::playlist, "Blend", 150),       tile(Tile::Kind::playlist, "Release Radar", 340),
                    tile(Tile::Kind::playlist, "Discover Weekly", 250), tile(Tile::Kind::playlist, "Road Trip", 20),
                    tile(Tile::Kind::playlist, "Late Night Coding", 200), tile(Tile::Kind::playlist, "Gym Mix", 0),
                    tile(Tile::Kind::playlist, "Sunday Slow", 60),   tile(Tile::Kind::playlist, "Focus Flow", 170)};
    m->albums = {tile(Tile::Kind::album, "Paper Bridges", 212),   tile(Tile::Kind::album, "Low Tide", 20),
                 tile(Tile::Kind::album, "Cartographers", 160),   tile(Tile::Kind::album, "Night Markets", 300),
                 tile(Tile::Kind::album, "Slow Orbit", 45),       tile(Tile::Kind::album, "Glasshouse", 190),
                 tile(Tile::Kind::album, "Evergreen", 120),       tile(Tile::Kind::album, "The Long Way", 345),
                 tile(Tile::Kind::album, "Kettle Songs", 80),     tile(Tile::Kind::album, "Afterimage", 260)};
    // Long rows, as a real library has: the same names again with numbers.
    for (std::vector<Tile> *row : {&m->playlists, &m->albums})
    {
        const size_t base = row->size();
        for (size_t i = 0; i < 28; ++i)
        {
            Tile t = (*row)[1 + i % (base - 1)];
            t.name += " " + std::to_string(2 + i / (base - 1));
            t.colour = draw::from_hsv(static_cast<float>((i * 37 + 11) % 360), 0.7f, 0.75f);
            row->push_back(t);
        }
    }
    if (sample == "paused")
        m->playing = false;
    else if (sample == "long")
    {
        m->track.title = "Glass Hours (Live at the Old Mill, Remastered Deluxe Edition with a Longer Name)";
        m->track.artist = "The Nightjars, with a long list of guest musicians and featured artists";
        m->context_name = "A playlist with a rather long name that keeps going and going";
    }
    else if (sample == "nolyrics")
    {
        m->has_lyrics = false;
        m->has_canvas = false;
        m->liked = false;
        m->context_kind = "PLAYLIST";
        m->context_name = "Evening Drive";
    }
    else if (sample == "shuffle-on")
    {
        m->shuffle = true;
        m->repeat = Repeat::one;
        m->pending = true;
    }
    else if (sample == "idle")
        m->link = Link::idle;
    else if (sample == "kabuki")
        m->link = Link::waiting_kabuki;
    else if (sample == "offline")
        m->link = Link::offline;
}

} // namespace

brls::Platform *brls::Platform::createPlatform()
{
    return new ::Platform();
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <output folder> [options] [steps...]\n(see the top of host/main.cpp)\n", argv[0]);
        return 2;
    }
    preview.output = argv[1];
    preview.fonts = PLAYER_FONTS;
    std::string cover, canvas, sample = "playing";
    bool live = false;
    Look look;
    for (int i = 2; i < argc; ++i)
    {
        const std::string_view arg = argv[i];
        if (arg == "--size" && i + 2 < argc)
        {
            preview.width = std::atoi(argv[++i]);
            preview.height = std::atoi(argv[++i]);
        }
        else if (arg == "--fonts" && i + 1 < argc)
            preview.fonts = argv[++i];
        else if (arg == "--cover" && i + 1 < argc)
            cover = argv[++i];
        else if (arg == "--canvas" && i + 1 < argc)
            canvas = argv[++i];
        else if (arg == "--sample" && i + 1 < argc)
            sample = argv[++i];
        else if (arg == "--bg" && i + 1 < argc)
            look.background = std::string_view(argv[++i]) == "canvas" ? Look::Background::canvas : Look::Background::tint;
        else if (arg == "--focus" && i + 1 < argc)
            look.focus = std::string_view(argv[++i]) == "ring" ? Look::Focus::ring : Look::Focus::fill;
        else if (arg == "--hide-after" && i + 1 < argc)
            look.hide_after = static_cast<float>(std::atof(argv[++i]));
        else if (arg == "--no-pill")
            look.pill = false;
        else if (arg == "--mock")
            live = false;
        else if (arg == "--live")
            live = true;
        else if (Step step; parse_step(arg, &step))
            preview.script.push_back(step);
        else
        {
            std::fprintf(stderr, "unknown argument %s\n", argv[i]);
            return 2;
        }
    }

    App app;
    fill_sample(&app.model, sample, cover, canvas);
    // Commands go out through "librespot" only: here a stand-in that changes the sample state.
    app.send = [&app](Cmd cmd, int value)
    {
        Model &m = app.model;
        switch (cmd)
        {
        case Cmd::play_pause: m.playing = !m.playing; break;
        case Cmd::previous:
        case Cmd::next: m.position_ms = 0; break;
        case Cmd::shuffle: m.shuffle = !m.shuffle; break;
        case Cmd::repeat:
            m.repeat = m.repeat == Repeat::off ? Repeat::all : m.repeat == Repeat::all ? Repeat::one : Repeat::off;
            break;
        case Cmd::like: m.liked = !m.liked; break;
        case Cmd::seek:
            m.position_ms = value;
            std::printf("seek to %d ms\n", value);
            break;
        case Cmd::play_queued: std::printf("play queued #%d\n", value); break;
        case Cmd::open_item:
            std::printf("open section %d item %d\n", value / 1000, value % 1000);
            fill_detail(&m, value / 1000, value % 1000);
            break;
        case Cmd::play_song:
        case Cmd::play_all:
        {
            const int i = cmd == Cmd::play_all ? 0 : value;
            if (i >= 0 && i < static_cast<int>(m.detail.songs.size()))
            {
                const Song &sg = m.detail.songs[i];
                m.track.title = sg.title;
                m.track.artist = sg.artist;
                m.track.duration_ms = sg.duration_ms;
                m.track.cover_colour = sg.colour;
                m.position_ms = 0;
                m.playing = true;
                m.detail.playing = i;
                std::printf("play song %d\n", i);
            }
            break;
        }
        case Cmd::save_item: m.detail.saved = !m.detail.saved; break;
        case Cmd::open_search:
            std::printf("open search\n");
            fill_search(&m);
            break;
        case Cmd::play_found:
            if (value >= 0 && value < static_cast<int>(m.search.songs.size()))
            {
                const Song &sg = m.search.songs[value];
                m.track.title = sg.title;
                m.track.artist = sg.artist;
                m.track.duration_ms = sg.duration_ms;
                m.track.cover_colour = sg.colour;
                m.position_ms = 0;
                m.playing = true;
                std::printf("play found song %d\n", value);
            }
            break;
        case Cmd::open_found_album:
            if (value >= 0 && value < static_cast<int>(m.search.albums.size()))
                build_detail(&m, m.search.albums[value], value + 3);
            break;
        }
    };

    std::unique_ptr<player::Client> link;
    if(live){link=std::make_unique<player::Client>();app.model=link->snapshot();
        app.update=[&]{link->snapshot_into(app.model);};app.send=[&](Cmd cmd,int value){link->send(cmd,value,app.model);};}
    brls::Application::ORIGINAL_WINDOW_WIDTH = 1920;
    brls::Application::ORIGINAL_WINDOW_HEIGHT = 1080;
    brls::Logger::setLogLevel(brls::LogLevel::LOG_WARNING);
    if (!brls::Application::init())
    {
        std::fprintf(stderr, "borealis did not start\n");
        return 1;
    }
    brls::Application::createWindow("Player");
    brls::Application::setGlobalQuit(false);

    Screen *screen = make_screen(&app, look, [] { brls::Application::quit(); });
    set_clock_running(screen, !live && preview.script.empty());
    brls::Application::pushActivity(new brls::Activity(as_view(screen)), brls::TransitionAnimation::NONE);
    give_focus(screen);
    while (brls::Application::mainLoop())
    {
    }
    return preview.failed ? 1 : 0;
}
