// borealis' few words ("OK", "Back"), in English. This takes the place of borealis'
// lib/core/i18n.cpp, which reads JSON files from a resources folder the overlay does not have.

#include <borealis/core/i18n.hpp>

#include <string_view>

namespace brls
{

namespace
{

struct Word
{
    std::string_view name;
    const char *english;
};
constexpr Word kWords[] = {
    {"hints/ok", "OK"},     {"hints/cancel", "Cancel"}, {"hints/back", "Back"}, {"hints/exit", "Exit"},
    {"hints/open", "Open"}, {"hints/on", "On"},         {"hints/off", "Off"},   {"hints/submit", "OK"},
};

} // namespace

void loadTranslations()
{
}

namespace internal
{
std::string getRawStr(std::string stringName)
{
    for (const Word &word : kWords)
        if (word.name == stringName)
            return word.english;
    return stringName;
}
} // namespace internal

inline namespace literals
{
std::string operator"" _i18n(const char *str, size_t len)
{
    return internal::getRawStr(std::string(str, len));
}
} // namespace literals

} // namespace brls
