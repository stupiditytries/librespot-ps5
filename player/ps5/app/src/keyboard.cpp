#include "keyboard.hpp"

#include <cstring>

void log_line(const char *format, ...);

namespace
{
// The dialog's settings, as the console lays them out.
struct DialogSetting
{
    std::int32_t user;
    std::int32_t type;                // 0: any text
    std::uint64_t languages;          // 0: the console's own
    std::int32_t enter_label;         // 2: "Search"
    std::int32_t input_method;
    void *filter;
    std::uint32_t option;
    std::uint32_t max_length;
    std::uint16_t *text;              // typed into, and holding what it starts with
    float x, y;
    std::int32_t horizontal, vertical; // which point of the dialog (x, y) places
    const std::uint16_t *placeholder;
    const std::uint16_t *title;
    std::int8_t reserved[16];
};
static_assert(sizeof(DialogSetting) == 96);

struct DialogResult
{
    std::int32_t outcome; // 0 done, 1 cancelled by the user, 2 closed by the console
    std::int8_t reserved[12];
};

constexpr std::uint16_t kDialogModule = 0x0096;
constexpr int kRunning = 1, kFinished = 2;
constexpr std::uint32_t kLongest = 60;
} // namespace

extern "C"
{
int sceSysmoduleLoadModule(std::uint16_t module);
int sceUserServiceGetInitialUser(std::int32_t *user);
int sceImeDialogInit(const DialogSetting *setting, void *extended);
int sceImeDialogGetStatus(void);
int sceImeDialogGetResult(DialogResult *result);
int sceImeDialogTerm(void);
}

namespace ps5
{
namespace
{
// Text between the app's UTF-8 and the dialog's 16-bit characters. Characters outside
// the 16-bit range (emoji) are left out.
void widen(const std::string &text, std::uint16_t *out, std::size_t capacity)
{
    std::size_t count = 0;
    for (std::size_t index = 0; index < text.size() && count + 1 < capacity;)
    {
        const auto byte = static_cast<unsigned char>(text[index]);
        const auto next = [&](std::size_t offset) {
            return index + offset < text.size() ? static_cast<unsigned char>(text[index + offset]) & 0x3Fu : 0u;
        };
        if (byte < 0x80)
            out[count++] = byte, index += 1;
        else if ((byte & 0xE0) == 0xC0)
            out[count++] = static_cast<std::uint16_t>((byte & 0x1Fu) << 6 | next(1)), index += 2;
        else if ((byte & 0xF0) == 0xE0)
            out[count++] = static_cast<std::uint16_t>((byte & 0x0Fu) << 12 | next(1) << 6 | next(2)), index += 3;
        else
            index += 4;
    }
    out[count] = 0;
}

std::string narrowed(const std::uint16_t *text, std::size_t capacity)
{
    std::string out;
    for (std::size_t index = 0; index < capacity && text[index] != 0; ++index)
    {
        const std::uint16_t unit = text[index];
        if (unit < 0x80)
            out += static_cast<char>(unit);
        else if (unit < 0x800)
        {
            out += static_cast<char>(0xC0 | unit >> 6);
            out += static_cast<char>(0x80 | (unit & 0x3F));
        }
        else if (unit >= 0xD800 && unit <= 0xDFFF)
            continue; // half of a character outside the 16-bit range
        else
        {
            out += static_cast<char>(0xE0 | unit >> 12);
            out += static_cast<char>(0x80 | (unit >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (unit & 0x3F));
        }
    }
    // Typed (or dictated) words come with stray spaces at their ends.
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    out.erase(0, out.find_first_not_of(' ') == std::string::npos ? out.size() : out.find_first_not_of(' '));
    return out;
}
} // namespace

bool Keyboard::open(const std::string &text)
{
    if (active_)
        return true;
    if (!loaded_)
    {
        const int loaded = sceSysmoduleLoadModule(kDialogModule);
        log_line("keyboard: dialog module %s (0x%x)", loaded >= 0 ? "loaded" : "did not load", loaded);
        loaded_ = true; // tried: the dialog itself says whether it can open
    }
    std::memset(buffer_, 0, sizeof buffer_);
    widen(text, buffer_, kLongest + 1);
    widen("Search", title_, std::size(title_));
    DialogSetting setting{};
    sceUserServiceGetInitialUser(&setting.user);
    setting.enter_label = 2;
    setting.max_length = kLongest;
    setting.text = buffer_;
    setting.x = 960.0f;
    setting.y = 1000.0f;
    setting.horizontal = 1; // centred
    setting.vertical = 2;   // by its lower edge
    setting.title = title_;
    const int opened = sceImeDialogInit(&setting, nullptr);
    if (opened != 0)
    {
        log_line("keyboard: the dialog did not open (0x%x)", opened);
        return false;
    }
    active_ = true;
    return true;
}

Keyboard::Result Keyboard::poll(std::string &text)
{
    if (!active_)
        return Result::None;
    const int status = sceImeDialogGetStatus();
    if (status == kRunning)
        return Result::None;
    Result result = Result::Cancelled;
    if (status == kFinished)
    {
        DialogResult outcome{};
        sceImeDialogGetResult(&outcome);
        if (outcome.outcome == 0)
        {
            text = narrowed(buffer_, std::size(buffer_));
            result = Result::Done;
        }
    }
    sceImeDialogTerm();
    active_ = false;
    log_line("keyboard: %s", result == Result::Done ? "done" : "cancelled");
    return result;
}
} // namespace ps5
