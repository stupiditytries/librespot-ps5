// What the app does when something inside it gives up: abort(), a failed assert, or the
// C++ library's own checks. Each says where it was called from in the console's kernel log
// before stopping, because an abort otherwise leaves nothing to go on.
//
// The address logged is where the call came from. Subtract the address the app is loaded
// at (0x400000 on the console) and look the result up in the linker's map file
// (llvm-pie.map in the build folder) to find the function.

#include <cstdarg>
#include <cstdio>

#include <__verbose_abort>

extern "C"
{
    int sceKernelDebugOutText(int channel, const char *text);

    static void say(const char *format, ...)
    {
        char text[400];
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(text, sizeof text, format, arguments);
        va_end(arguments);
        char line[440];
        std::snprintf(line, sizeof line, "[stremio] %s\n", text);
        sceKernelDebugOutText(0, line);
    }

    [[noreturn]] void abort(void)
    {
        say("abort() called from %p", __builtin_return_address(0));
        // Stops the app in a way the console reports as a crash, as abort() would.
        __builtin_trap();
    }

    [[noreturn]] void __assert(const char *function, const char *file, int line,
                               const char *expression)
    {
        say("assertion failed: %s, in %s at %s:%d, called from %p", expression,
            function != nullptr ? function : "?", file, line, __builtin_return_address(0));
        __builtin_trap();
    }
}

_LIBCPP_BEGIN_NAMESPACE_STD
// The C++ library calls this when one of its own checks fails (an index past the end of a
// container, for example), with a description of what went wrong.
void __libcpp_verbose_abort(char const *format, ...)
{
    char text[360];
    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    say("C++ library check failed: %s (called from %p)", text, __builtin_return_address(0));
    __builtin_trap();
}
_LIBCPP_END_NAMESPACE_STD
