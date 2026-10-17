/*
 * Manually launched modular Connect receiver. The tested USB-free PCM transport
 * remains in the background; Kabuki owns the control-centre cards.
 * Playback requires a connected Kabuki provider. Missing Kabuki blocks play and
 * sends a notice; there is no exit timer. A separate signed Borealis folder app
 * is installed/updated while closed. Change quit.txt to stop the receiver.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <unistd.h>

#define DATA_DIR "/data/spotifyps5"
#define LOG_PATH DATA_DIR "/connect.log"
#define CACHE_DIR DATA_DIR "/cache"
#define QUIT_PATH DATA_DIR "/quit.txt"
#define TAPE_TEST_PATH DATA_DIR "/tape-test.txt"
#define DEBUG_PATH DATA_DIR "/debug.txt"
/* The log is started afresh (the old one kept as connect.log.old) at this size, so it
 * cannot fill the console's storage. */
#define LOG_LIMIT (4 * 1024 * 1024)
#define DEVICE_NAME "PS5 Connect"
/* A loopback port held for as long as the program runs, so a second copy can tell. */
#define INSTANCE_PORT 38573

struct connect_callbacks
{
    void *user_data;
    void (*log)(void *user_data, const char *line);
    int (*write)(void *user_data, const int16_t *samples, size_t count);
    void (*start)(void *user_data);
    void (*stop)(void *user_data);
};

struct connect_settings
{
    const char *name;
    const char *cache_dir;
    uint32_t bitrate;
    uint32_t initial_volume;
    int debug;
};

int connect_run(const struct connect_settings *settings, const struct connect_callbacks *callbacks);
void connect_stop(void);
void connect_set_secret(const uint8_t *secret, size_t length);
int connect_tape_start(const char *data_dir, void (*log)(void *user_data, const char *line),
                       void *user_data, int debug, int (*notify)(const char *payload));
int sceNotificationSend(int user, bool logged, const char *payload);
int connect_tape_test(const char *data_dir);

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

/* The receiver's interface has a place for an audio output of the caller's own. This
 * program has none (see the top of the file). */
static int discard_audio(void *user_data, const int16_t *samples, size_t count)
{
    (void)user_data;
    (void)samples;
    (void)count;
    return 0;
}

static void log_line(const char *text)
{
    pthread_mutex_lock(&log_lock);
    printf("[connectd] %s\n", text);
    fflush(stdout);
    struct stat size;
    if (stat(LOG_PATH, &size) == 0 && size.st_size > LOG_LIMIT)
        rename(LOG_PATH, LOG_PATH ".old");
    FILE *file = fopen(LOG_PATH, "a");
    if (file != NULL)
    {
        fprintf(file, "%s\n", text);
        fclose(file);
    }
    pthread_mutex_unlock(&log_lock);
}

static void receiver_log(void *user_data, const char *line)
{
    (void)user_data;
    log_line(line);
}

static void read_small_file(const char *path, char *text, size_t size)
{
    text[0] = 0;
    FILE *file = fopen(path, "r");
    if (file != NULL)
    {
        size_t length = fread(text, 1, size - 1, file);
        text[length] = 0;
        text[strcspn(text, "\r\n")] = 0;
        fclose(file);
    }
}

/* Gives the receiver an identifier of this console to encrypt the saved login with. The
 * identifier is not logged or written anywhere. */
static void set_device_secret(void)
{
    static const char *const names[] = {"machdep.openpsid", "machdep.openpsid_for_sys",
                                        "kern.hostuuid"};
    for (size_t at = 0; at < sizeof names / sizeof names[0]; at++)
    {
        uint8_t value[64];
        size_t length = sizeof value;
        if (sysctlbyname(names[at], value, &length, NULL, 0) != 0 || length < 16)
            continue;
        uint8_t any = 0;
        for (size_t byte = 0; byte < length; byte++)
            any |= value[byte];
        if (any == 0)
            continue;
        connect_set_secret(value, length);
        log_line("saved login device binding available (not a hardware keystore)");
        return;
    }
    log_line("device binding unavailable: login remains in memory; remembering is disabled");
}

/* Shows a notification (the receiver uses it for the track that has started). Sent as the
 * system user, as the console's own programs do. */
static int show_notification(const char *payload)
{
    return sceNotificationSend(0xfe, true, payload);
}
int librespot_song_notification(const char* payload){return show_notification(payload);}
void librespot_kabuki_required(const char *payload){
    if(payload)show_notification(payload);
}

/* Exits when quit.txt changes. Its content at start is the baseline, so a file left over
 * from an earlier run does nothing. */
static void *quit_watcher(void *unused)
{
    (void)unused;
    char baseline[64];
    read_small_file(QUIT_PATH, baseline, sizeof baseline);
    for (;;)
    {
        sleep(1);
        char now[64];
        read_small_file(QUIT_PATH, now, sizeof now);
        if (strcmp(now, baseline) != 0)
        {
            log_line("quit requested");
            connect_stop();
            extern int pcm_transport_shutdown(void);
            if (pcm_transport_shutdown()) log_line("USB-free PCM cleanup incomplete; inspect transport logs");
            _exit(0);
        }
    }
    return NULL;
}

/* Returns 0 when this is the only copy running. */
static int claim_instance(void)
{
    int socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_ < 0)
        return 0; /* cannot tell: carry on */
    struct sockaddr_in address;
    memset(&address, 0, sizeof address);
    address.sin_len = sizeof address;
    address.sin_family = AF_INET;
    address.sin_port = htons(INSTANCE_PORT);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(socket_, (struct sockaddr *)&address, sizeof address) != 0)
    {
        close(socket_);
        return -1;
    }
    listen(socket_, 1);
    return 0;
}

int main(void)
{
    mkdir(DATA_DIR, 0777);
    mkdir(CACHE_DIR, 0777);
    /* A copy that is already running is asked to exit (as "connectd.py stop" does), so
     * loading the program again is enough to replace it with a new build. */
    for (int attempt = 0; claim_instance() != 0; attempt++)
    {
        if (attempt == 4)
        {
            log_line("another copy is running and did not exit: this copy exits");
            return 0;
        }
        FILE *quit = fopen(QUIT_PATH, "w");
        if (quit != NULL)
        {
            fprintf(quit, "replaced %d %d\n", (int)getpid(), attempt);
            fclose(quit);
        }
        sleep(4);
    }
    remove(LOG_PATH);
    log_line("starting");
    struct stat marker;
    int debug = stat(DEBUG_PATH, &marker) == 0;
    set_device_secret();

    pthread_t watcher;
    pthread_create(&watcher, NULL, quit_watcher, NULL);

#ifdef USB_AUTOSTART
    extern int usb_transport_quiesce_previous(void);
    if (usb_transport_quiesce_previous() != 0) {
        log_line("previous native worker cleanup is not confirmed; this receiver exits");
        return 1;
    }
#endif
#ifndef SHELL_PCM
    if (connect_tape_start(DATA_DIR, receiver_log, NULL, debug, show_notification) != 0)
    {
        /* The reason is in the log. Without the stream file there is no way to be heard. */
        log_line("no stream: this copy exits");
        show_notification("{\"rawData\":{\"viewTemplateType\":\"ToastTemplateA\","
                          "\"toastOverwriteType\":\"Always\",\"channelType\":\"MusicTrackChange\","
                          "\"useCaseId\":\"NUC59\",\"isImmediate\":false,\"priority\":2,"
                          "\"viewData\":{\"message\":{\"body\":\"librespot-ps5 could not start\"},"
                          "\"subMessage\":{\"body\":\"It needs a USB drive the console plays music from\"}}},"
                          "\"createdDateTime\":\"2026-01-01T00:00:00.000Z\",\"localNotificationId\":\"1\"}");
        return 1;
    }
    if (stat(TAPE_TEST_PATH, &marker) == 0)
    {
        log_line("tape-test.txt is present: playing test audio through the stream; Spotify is not started");
        connect_tape_test(DATA_DIR);
        return 0;
    }

#endif

    struct connect_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.name = DEVICE_NAME;
    settings.cache_dir = CACHE_DIR;
    settings.bitrate = 320;
    settings.initial_volume = 100;
    settings.debug = debug;

    /* The receiver sends its audio to the stream; it does not hand any back here. */
    struct connect_callbacks callbacks;
    memset(&callbacks, 0, sizeof callbacks);
    callbacks.log = receiver_log;
    callbacks.write = discard_audio;

    extern int librespot_install_app(void);
    const int installed=librespot_install_app();
    log_line(installed==0?"Borealis app installed or current":installed==1?"App files updated while Music Player was open: close and reopen Music Player to use them":"Borealis app update failed; the receiver continues");
    int code = connect_run(&settings, &callbacks);
    extern int pcm_transport_shutdown(void);
    if (pcm_transport_shutdown()) log_line("USB-free PCM cleanup incomplete; inspect transport logs");
    char text[64];
    snprintf(text, sizeof text, "receiver returned %d", code);
    log_line(text);
    return 0;
}
