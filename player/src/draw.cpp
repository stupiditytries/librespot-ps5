#include "draw.hpp"
#include "art_cache.hpp"
#include "perf.hpp"
#include "presentation.hpp"

#include <borealis/core/application.hpp>
#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace player::draw
{

namespace
{
int g_font[4] = {-1, -1, -1, -1};
constexpr float kPi = 3.14159265f;

int font(Weight w)
{
    return g_font[static_cast<int>(w)];
}

int image(NVGcontext *vg, const std::string &file)
{
    return player::art::image(vg,file);
}

void set_font(NVGcontext *vg, Weight w, float size, float tracking)
{
    nvgFontFaceId(vg, font(w));
    nvgFontSize(vg, std::round(size*2.f)/2.f);
    nvgTextLetterSpacing(vg, tracking);
}
} // namespace

bool load_fonts(const std::string &dir)
{
    static const char *kFiles[4] = {"PlusJakartaSans-Regular.ttf", "PlusJakartaSans-Medium.ttf",
                                    "PlusJakartaSans-SemiBold.ttf", "PlusJakartaSans-Bold.ttf"};
    bool ok = true;
    for (int i = 0; i < 4; ++i)
    {
        const std::string name = std::string("pjs-") + std::to_string(i);
        if (brls::Application::loadFontFromFile(name, dir + "/" + kFiles[i]))
            g_font[i] = brls::Application::getFont(name);
        else
            ok = false;
    }
    // Japanese, Korean, Chinese and symbols (a music note): Noto Sans CJK, the face Spotify
    // itself falls back to, behind the Latin face. Missing only leaves those blank.
    NVGcontext *vg = brls::Application::getNVGContext();
    if (vg && brls::Application::loadFontFromFile("noto-cjk", dir + "/NotoSansCJK-Subset.otf"))
    {
        const int id = brls::Application::getFont("noto-cjk");
        for (int w = 0; w < 4; ++w)
            if (g_font[w] >= 0)
                nvgAddFallbackFontId(vg, g_font[w], id);
    }
    return ok;
}

NVGcolor rgba(int r, int g, int b, float a)
{
    return nvgRGBAf(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
NVGcolor rgb(Rgb c, float a)
{
    return nvgRGBAf(c.r, c.g, c.b, a);
}
NVGcolor white(float a)
{
    return nvgRGBAf(1, 1, 1, a);
}
NVGcolor grey(float a)
{
    return nvgRGBAf(0xB3 / 255.0f, 0xB3 / 255.0f, 0xB3 / 255.0f, a);
}
NVGcolor mix(NVGcolor a, NVGcolor b, float t)
{
    return nvgRGBAf(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
Rgb mix(Rgb a, Rgb b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

Rgb from_hsv(float h, float s, float v)
{
    h = std::fmod(std::fmod(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    const float c = v * s, x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f)), m = v - c;
    float r = 0, g = 0, b = 0;
    if (h < 1) { r = c; g = x; }
    else if (h < 2) { r = x; g = c; }
    else if (h < 3) { g = c; b = x; }
    else if (h < 4) { g = x; b = c; }
    else if (h < 5) { r = x; b = c; }
    else { r = c; b = x; }
    return {r + m, g + m, b + m};
}

void to_hsv(Rgb c, float *h, float *s, float *v)
{
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    *v = mx;
    *s = mx <= 0 ? 0 : d / mx;
    float hue = 0;
    if (d > 0)
    {
        if (mx == c.r) hue = std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) hue = (c.b - c.r) / d + 2.0f;
        else hue = (c.r - c.g) / d + 4.0f;
        hue *= 60.0f;
        if (hue < 0) hue += 360.0f;
    }
    *h = hue;
}

// [R1]: a blue cover sat on a (8, 22, 38) page: hue kept, saturation lifted, value about 0.15.
Rgb dark_tint(Rgb cover)
{
    float h, s, v;
    to_hsv(cover, &h, &s, &v);
    return from_hsv(h, std::clamp(0.35f + s * 0.5f, 0.0f, 0.85f), 0.15f);
}

Rgb lyrics_colour(Rgb cover)
{
    float h, s, v;
    to_hsv(cover, &h, &s, &v);
    return from_hsv(h, std::clamp(s, 0.45f, 0.80f), std::clamp(v, 0.42f, 0.62f));
}

Rgb dominant_colour(const std::string &file, Rgb fallback)
{
    int w = 0, h = 0, n = 0;
    unsigned char *px = stbi_load(file.c_str(), &w, &h, &n, 3);
    if (px == nullptr)
        return fallback;
    // Twelve hue buckets weighted by saturation; the heaviest bucket's average wins.
    struct Bucket { float wt = 0, r = 0, g = 0, b = 0; } buckets[12];
    const int step = std::max(1, std::min(w, h) / 64);
    for (int y = 0; y < h; y += step)
        for (int x = 0; x < w; x += step)
        {
            const unsigned char *p = px + (static_cast<size_t>(y) * w + x) * 3;
            Rgb c{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
            float hh, ss, vv;
            to_hsv(c, &hh, &ss, &vv);
            const float wt = ss * ss * std::min(vv, 0.9f) + 0.002f;
            Bucket &b = buckets[static_cast<int>(hh / 30.0f) % 12];
            b.wt += wt; b.r += c.r * wt; b.g += c.g * wt; b.b += c.b * wt;
        }
    stbi_image_free(px);
    const Bucket *best = &buckets[0];
    for (const Bucket &b : buckets)
        if (b.wt > best->wt) best = &b;
    return best->wt > 0 ? Rgb{best->r / best->wt, best->g / best->wt, best->b / best->wt} : fallback;
}

float ease_out(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

float text_width(NVGcontext *vg, Weight w, float size, const std::string &s, float tracking)
{
    using Key=std::tuple<NVGcontext*,int,float,float,std::string>;
    static std::map<Key,float> measured;
    size=std::round(size*2.f)/2.f;
    set_font(vg, w, size, tracking);
    if(s.empty())return 0;
    const Key key{vg,static_cast<int>(w),size,tracking,s};
    if(auto at=measured.find(key);at!=measured.end())return at->second;
    const float width=nvgTextBounds(vg, 0, 0, s.c_str(), nullptr, nullptr);
    if(!std::isfinite(width)||width<0)return float(s.size())*size*.55f;
    if(measured.size()>=4096)measured.clear();
    measured.emplace(key,width);return width;
}

void text(NVGcontext *vg, Weight w, float size, NVGcolor colour, float x, float base, const std::string &s,
          Align align, float tracking)
{
    set_font(vg, w, size, tracking);
    nvgFillColor(vg, colour);
    nvgTextAlign(vg, (align == Align::left ? NVG_ALIGN_LEFT : align == Align::right ? NVG_ALIGN_RIGHT : NVG_ALIGN_CENTER) |
                         NVG_ALIGN_BASELINE);
    nvgText(vg, x, base, s.c_str(), nullptr);
}

std::string fit_uncached(NVGcontext *vg, Weight w, float size, const std::string &s, float max_width);
std::string fit(NVGcontext *vg, Weight w, float size, const std::string &s, float max_width)
{
    if(s.empty()||!std::isfinite(max_width)||max_width<=0)return {};
    // Lists call this for every visible row each frame; the answer only changes with its inputs.
    using Key=std::tuple<NVGcontext*,int,float,float,std::string>;
    static std::map<Key,std::string> cached;
    const Key key{vg,static_cast<int>(w),std::round(size*2.f)/2.f,std::round(max_width),s};
    if(auto at=cached.find(key);at!=cached.end())return at->second;
    if(cached.size()>4096)cached.clear();
    return cached[key]=fit_uncached(vg,w,size,s,max_width);
}
std::string fit_uncached(NVGcontext *vg, Weight w, float size, const std::string &s, float max_width)
{
    perf::Scope measure(perf::TextFit);
    if (text_width(vg, w, size, s) <= max_width)
        return s;
    std::string cut = s;
    while (!cut.empty())
    {
        auto boundary=cut.size()-1;
        while(boundary>0&&(static_cast<unsigned char>(cut[boundary])&0xC0)==0x80)--boundary;
        cut.resize(boundary);
        while (!cut.empty() && cut.back() == ' ')
            cut.pop_back();
        if (text_width(vg, w, size, cut + "...") <= max_width)
            return cut + "...";
    }
    return "...";
}

std::vector<std::string> title_lines(NVGcontext* vg,Weight w,float size,const std::string& s,float width){
    using Key=std::tuple<NVGcontext*,int,float,float,std::string>;
    static std::map<Key,std::vector<std::string>> cache;
    size=std::round(size*2.f)/2.f;width=std::max(1.f,std::round(width));
    Key key{vg,int(w),size,width,s};if(auto it=cache.find(key);it!=cache.end())return it->second;
    set_font(vg,w,size,0);NVGtextRow rows[3];const int n=nvgTextBreakLines(vg,s.c_str(),nullptr,width,rows,3);
    std::vector<std::string> lines;
    if(n>0)lines.emplace_back(rows[0].start,rows[0].end);
    if(n>1)lines.push_back(n>2?fit(vg,w,size,std::string(rows[1].start,s.c_str()+s.size()),width):std::string(rows[1].start,rows[1].end));
    if(cache.size()>=512)cache.clear();cache.emplace(std::move(key),lines);return lines;
}

// Icons are drawn in a 24-unit box centred on (cx, cy).
namespace
{
struct Pen
{
    NVGcontext *vg;
    float ox, oy, k;
    float X(float u) const { return ox + u * k; }
    float Y(float u) const { return oy + u * k; }
};

// A filled triangle whose tip is (x, y), pointing along (dx, dy).
void arrow_head(const Pen &p, float x, float y, float dx, float dy, float len, NVGcolor c)
{
    const float nx = -dy, ny = dx;
    nvgBeginPath(p.vg);
    nvgMoveTo(p.vg, p.X(x), p.Y(y));
    nvgLineTo(p.vg, p.X(x - dx * len + nx * len * 0.62f), p.Y(y - dy * len + ny * len * 0.62f));
    nvgLineTo(p.vg, p.X(x - dx * len - nx * len * 0.62f), p.Y(y - dy * len - ny * len * 0.62f));
    nvgClosePath(p.vg);
    nvgFillColor(p.vg, c);
    nvgFill(p.vg);
}

void stroke_style(const Pen &p, NVGcolor c, float w)
{
    nvgStrokeColor(p.vg, c);
    nvgStrokeWidth(p.vg, w * p.k);
    nvgLineCap(p.vg, NVG_ROUND);
    nvgLineJoin(p.vg, NVG_ROUND);
}
} // namespace

void icon(NVGcontext *vg, Icon i, float cx, float cy, float size, NVGcolor c)
{
    const float k = size / 24.0f;
    const Pen p{vg, cx - 12.0f * k, cy - 12.0f * k, k};
    nvgSave(vg);
    nvgFillColor(vg, c);
    switch (i)
    {
    case Icon::play:
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(8), p.Y(5.2f));
        nvgLineTo(vg, p.X(19), p.Y(12));
        nvgLineTo(vg, p.X(8), p.Y(18.8f));
        nvgClosePath(vg);
        nvgFill(vg);
        break;
    case Icon::pause:
        nvgBeginPath(vg);
        nvgRoundedRect(vg, p.X(6.2f), p.Y(5), 4.0f * k, 14.0f * k, 1.2f * k);
        nvgRoundedRect(vg, p.X(13.8f), p.Y(5), 4.0f * k, 14.0f * k, 1.2f * k);
        nvgFill(vg);
        break;
    case Icon::previous:
    case Icon::next:
    {
        const float f = i == Icon::next ? 1.0f : -1.0f; // previous mirrors next about x = 12
        auto X = [&](float u) { return p.X(12.0f + f * (u - 12.0f)); };
        nvgBeginPath(vg);
        nvgMoveTo(vg, X(5), p.Y(5.5f));
        nvgLineTo(vg, X(17.4f), p.Y(12));
        nvgLineTo(vg, X(5), p.Y(18.5f));
        nvgClosePath(vg);
        nvgFill(vg);
        const float a = X(17.9f), b = X(20.1f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, std::min(a, b), p.Y(5.5f), std::fabs(b - a), 13.0f * k, 0.8f * k);
        nvgFill(vg);
        break;
    }
    case Icon::shuffle:
        stroke_style(p, c, 1.9f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(3.5f), p.Y(7.2f));
        nvgBezierTo(vg, p.X(9), p.Y(7.2f), p.X(10), p.Y(16.8f), p.X(16), p.Y(16.8f));
        nvgMoveTo(vg, p.X(3.5f), p.Y(16.8f));
        nvgBezierTo(vg, p.X(6), p.Y(16.8f), p.X(7.2f), p.Y(15.4f), p.X(8.4f), p.Y(13.8f));
        nvgMoveTo(vg, p.X(11.6f), p.Y(10.2f));
        nvgBezierTo(vg, p.X(12.8f), p.Y(8.6f), p.X(14), p.Y(7.2f), p.X(16), p.Y(7.2f));
        nvgStroke(vg);
        arrow_head(p, 20.6f, 7.2f, 1, 0, 3.8f, c);
        arrow_head(p, 20.6f, 16.8f, 1, 0, 3.8f, c);
        break;
    case Icon::repeat:
    case Icon::repeat_one:
        stroke_style(p, c, 1.9f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(5.2f), p.Y(11));
        nvgLineTo(vg, p.X(5.2f), p.Y(9.4f));
        nvgArcTo(vg, p.X(5.2f), p.Y(6.8f), p.X(7.8f), p.Y(6.8f), 2.6f * k);
        nvgLineTo(vg, p.X(14.2f), p.Y(6.8f));
        nvgMoveTo(vg, p.X(18.8f), p.Y(13));
        nvgLineTo(vg, p.X(18.8f), p.Y(14.6f));
        nvgArcTo(vg, p.X(18.8f), p.Y(17.2f), p.X(16.2f), p.Y(17.2f), 2.6f * k);
        nvgLineTo(vg, p.X(9.8f), p.Y(17.2f));
        nvgStroke(vg);
        arrow_head(p, 18.2f, 6.8f, 1, 0, 3.6f, c);
        arrow_head(p, 5.8f, 17.2f, -1, 0, 3.6f, c);
        if (i == Icon::repeat_one)
        {
            nvgBeginPath(vg);
            nvgMoveTo(vg, p.X(11.0f), p.Y(10.6f));
            nvgLineTo(vg, p.X(12.4f), p.Y(9.6f));
            nvgLineTo(vg, p.X(12.4f), p.Y(14.4f));
            nvgStroke(vg);
        }
        break;
    case Icon::heart:
    case Icon::heart_filled:
    {
        // One closed outline: two round lobes meeting at a dip, a point at the bottom.
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(12.0f), p.Y(21.0f));
        nvgLineTo(vg, p.X(10.6f), p.Y(19.8f));
        nvgBezierTo(vg, p.X(5.4f), p.Y(15.2f), p.X(2.2f), p.Y(12.3f), p.X(2.2f), p.Y(8.5f));
        nvgBezierTo(vg, p.X(2.2f), p.Y(5.5f), p.X(4.6f), p.Y(3.2f), p.X(7.5f), p.Y(3.2f));
        nvgBezierTo(vg, p.X(9.2f), p.Y(3.2f), p.X(10.9f), p.Y(4.0f), p.X(12.0f), p.Y(5.3f));
        nvgBezierTo(vg, p.X(13.1f), p.Y(4.0f), p.X(14.8f), p.Y(3.2f), p.X(16.5f), p.Y(3.2f));
        nvgBezierTo(vg, p.X(19.4f), p.Y(3.2f), p.X(21.8f), p.Y(5.5f), p.X(21.8f), p.Y(8.5f));
        nvgBezierTo(vg, p.X(21.8f), p.Y(12.3f), p.X(18.6f), p.Y(15.2f), p.X(13.4f), p.Y(19.8f));
        nvgClosePath(vg);
        if (i == Icon::heart_filled)
        {
            nvgFill(vg);
        }
        else
        {
            stroke_style(p, c, 1.8f);
            nvgStroke(vg);
        }
        break;
    }
    case Icon::lyrics:
        // A microphone on the diagonal, its handle down and to the left.
        stroke_style(p, c, 1.9f);
        nvgSave(vg);
        nvgTranslate(vg, p.X(14.4f), p.Y(9.6f));
        nvgRotate(vg, kPi / 4.0f);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, -3.4f * k, -6.4f * k, 6.8f * k, 10.6f * k, 3.4f * k);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, -3.4f * k, -1.2f * k);
        nvgLineTo(vg, 3.4f * k, -1.2f * k);
        nvgStroke(vg);
        nvgRestore(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(11.2f), p.Y(12.8f));
        nvgLineTo(vg, p.X(4.2f), p.Y(19.8f));
        nvgStroke(vg);
        break;
    case Icon::queue:
        stroke_style(p, c, 1.9f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(4), p.Y(6.2f));
        nvgLineTo(vg, p.X(20), p.Y(6.2f));
        nvgMoveTo(vg, p.X(4), p.Y(12));
        nvgLineTo(vg, p.X(20), p.Y(12));
        nvgMoveTo(vg, p.X(4), p.Y(17.8f));
        nvgLineTo(vg, p.X(13), p.Y(17.8f));
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(16.2f), p.Y(15.4f));
        nvgLineTo(vg, p.X(20.6f), p.Y(17.8f));
        nvgLineTo(vg, p.X(16.2f), p.Y(20.2f));
        nvgClosePath(vg);
        nvgFill(vg);
        break;
    case Icon::search:
        stroke_style(p, c, 2.2f);
        nvgBeginPath(vg);
        nvgCircle(vg, p.X(10.4f), p.Y(10.4f), 6.6f * k);
        nvgStroke(vg);
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(15.4f), p.Y(15.4f));
        nvgLineTo(vg, p.X(20.4f), p.Y(20.4f));
        nvgStroke(vg);
        break;
    case Icon::plus:
        stroke_style(p, c, 2.2f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(12), p.Y(4.5f));
        nvgLineTo(vg, p.X(12), p.Y(19.5f));
        nvgMoveTo(vg, p.X(4.5f), p.Y(12));
        nvgLineTo(vg, p.X(19.5f), p.Y(12));
        nvgStroke(vg);
        break;
    case Icon::check:
        stroke_style(p, c, 2.4f);
        nvgBeginPath(vg);
        nvgMoveTo(vg, p.X(5), p.Y(12.5f));
        nvgLineTo(vg, p.X(10), p.Y(17.5f));
        nvgLineTo(vg, p.X(19.5f), p.Y(6.5f));
        nvgStroke(vg);
        break;
    case Icon::note:
        // One continuous filled contour from the supplied SVG; no stem seam.
        nvgTranslate(vg,cx-size*.5f,cy-size*.5f);
        nvgScale(vg,size/322.199f,size/322.199f);
        nvgBeginPath(vg);
        #include "note_path.inc"
        nvgFill(vg);
        break;
    }
    nvgRestore(vg);
}

void cover(NVGcontext *vg, const std::string &file, Rgb c, float x, float y, float size, float radius, float alpha)
{
    const int img = image(vg, file);
    if(art::live()) {nvgBeginPath(vg);nvgRoundedRect(vg,x,y,size,size,radius);nvgFillColor(vg,rgb(c,alpha));nvgFill(vg);if(img<=0)return;alpha*=art::opacity(file);}
    if (img > 0)
    {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, size, size, radius);
        nvgFillPaint(vg, nvgImagePattern(vg, x, y, size, size, 0, img, alpha));
        nvgFill(vg);
        return;
    }
    // A sample cover painted from its colour: a diagonal wash, a disc and a bar.
    float h, s, v;
    to_hsv(c, &h, &s, &v);
    const Rgb top = from_hsv(h + 14, std::clamp(s, 0.4f, 0.9f), std::clamp(v + 0.28f, 0.5f, 1.0f));
    const Rgb bottom = from_hsv(h - 12, std::clamp(s + 0.1f, 0.4f, 1.0f), std::clamp(v * 0.45f, 0.12f, 0.5f));
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, size, size, radius);
    nvgFillPaint(vg, nvgLinearGradient(vg, x, y, x + size, y + size, rgb(top, alpha), rgb(bottom, alpha)));
    nvgFill(vg);
    nvgIntersectScissor(vg, x, y, size, size);
    const Rgb accent = from_hsv(h + 150, 0.55f, 0.95f);
    nvgBeginPath(vg);
    nvgCircle(vg, x + size * 0.62f, y + size * 0.4f, size * 0.24f);
    nvgFillColor(vg, rgb(accent, 0.85f * alpha));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, x, y + size * 0.74f, size, size * 0.07f);
    nvgFillColor(vg, nvgRGBAf(1, 1, 1, 0.55f * alpha));
    nvgFill(vg);
    nvgRestore(vg);
}

bool image_fill(NVGcontext *vg, const std::string &file, float x, float y, float w, float h, float alpha, bool contain)
{
    const int img = image(vg, file);
    if (img <= 0)
        return false;
    int iw = 0, ih = 0;
    nvgImageSize(vg, img, &iw, &ih);
    const auto rect=fitted_image(float(iw),float(ih),x,y,w,h,contain);
    if(rect.w<=0||rect.h<=0)return false;
    nvgBeginPath(vg);
    nvgRect(vg, contain?rect.x:x, contain?rect.y:y, contain?rect.w:w, contain?rect.h:h);
    nvgFillPaint(vg, nvgImagePattern(vg, rect.x, rect.y, rect.w, rect.h, 0, img, alpha));
    nvgFill(vg);
    return true;
}

void canvas_stand_in(NVGcontext *vg, Rgb c, float x, float y, float w, float h)
{
    // Soft colour fields and a dark figure-like blob; the caller's scrim goes on top.
    float hh, s, v;
    to_hsv(c, &hh, &s, &v);
    const Rgb a = from_hsv(hh, 0.55f, 0.55f), b = from_hsv(hh + 30, 0.4f, 0.8f), d = from_hsv(hh - 20, 0.6f, 0.25f);
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillPaint(vg, nvgLinearGradient(vg, x, y, x + w, y + h, rgb(b), rgb(d)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillPaint(vg, nvgRadialGradient(vg, x + w * 0.28f, y + h * 0.38f, 0, w * 0.5f, rgb(a, 0.9f), rgb(a, 0)));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgEllipse(vg, x + w * 0.72f, y + h * 0.62f, w * 0.12f, h * 0.42f);
    nvgFillPaint(vg, nvgRadialGradient(vg, x + w * 0.72f, y + h * 0.5f, 0, h * 0.5f, rgb(d, 0.95f), rgb(d, 0)));
    nvgFill(vg);
}

void equaliser(NVGcontext *vg, float x, float y, float h, float t, NVGcolor colour)
{
    const float w = h * 0.22f;
    const float ph[3] = {0.0f, 1.7f, 3.1f};
    nvgBeginPath(vg);
    for (int i = 0; i < 3; ++i)
    {
        const float f = 0.45f + 0.55f * (0.5f + 0.5f * std::sin(t * 5.0f + ph[i]));
        const float bh = h * f;
        nvgRect(vg, x + i * w * 1.5f, y + h - bh, w, bh);
    }
    nvgFillColor(vg, colour);
    nvgFill(vg);
}

void spinner(NVGcontext *vg, float cx, float cy, float r, float t, NVGcolor colour)
{
    nvgBeginPath(vg);
    const float a0 = t * 4.5f;
    nvgArc(vg, cx, cy, r, a0, a0 + kPi * 1.35f, NVG_CW);
    nvgStrokeColor(vg, colour);
    nvgStrokeWidth(vg, r * 0.16f);
    nvgLineCap(vg, NVG_ROUND);
    nvgStroke(vg);
}

} // namespace player::draw
