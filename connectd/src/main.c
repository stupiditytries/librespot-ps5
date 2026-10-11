/*
 * Spotify Connect receiver as a background program: no tile, no window. It is loaded by
 * hand (for example from the web file manager) and keeps running until it is told to stop
 * or the console restarts. Nothing loads it automatically.
 *
 * It keeps everything it writes under /data/spotifyps5:
 *   connect.log    what it did (also printed to the kernel log)
 *   cache/         the saved Spotify login
 *   quit.txt       change this file's content to make the program exit
 *   tape.txt       settings for the stream (written with the defaults if missing)
 *   tape-test.txt  if present, test audio is played through the stream and Spotify is
 *                  not started (test.pcm, if present, is the audio)
 *   debug.txt      if present, the log is detailed and a copy of the console's own log is
 *                  kept in klog.txt; for finding faults, not for everyday use
 *
 * The saved login is encrypted with a key made from an identifier of this console, so a
 * copy of the file is of no use on another machine. The account name is not logged.
 *
 * Sound does not come from this program. A background program's own audio output is muted
 * by the console, and asking the console's media service to play for it made that service
 * crash, so this program does neither. It writes the audio into one long file,
 * Music/Start Librespot/Librespot Streamer.flac on a USB drive, just ahead of where the
 * console's own USB music player is reading it (connect/src/stream.rs). That player is
 * started by hand from the Music card and keeps playing over games. Playing
 * Music/Librespot Controls/Stop Librespot.flac stops the stream. Each track that starts is
 * shown as a notification with its cover.
 *
 * The program makes the folders and the two files itself. With no USB drive to keep them
 * on there is nothing it can play through, and it says so and exits.
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
                                        "kern.hostuuid", "kern.hostid"};
    for (size_t at = 0; at < sizeof names / sizeof names[0]; at++)
    {
        uint8_t value[64];
        size_t length = sizeof value;
        if (sysctlbyname(names[at], value, &length, NULL, 0) != 0 || length < 4)
            continue;
        uint8_t any = 0;
        for (size_t byte = 0; byte < length; byte++)
            any |= value[byte];
        if (any == 0)
            continue;
        connect_set_secret(value, length);
        log_line("the saved login is tied to this console");
        return;
    }
    log_line("no identifier of this console could be read: the saved login is hidden, "
             "but a copy of it would work elsewhere");
}

/* Shows a notification (the receiver uses it for the track that has started). Sent as the
 * system user, as the console's own programs do. */
static int show_notification(const char *payload)
{
    return sceNotificationSend(0xfe, true, payload);
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
            sleep(2);
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

    int code = connect_run(&settings, &callbacks);
    char text[64];
    snprintf(text, sizeof text, "receiver returned %d", code);
    log_line(text);
    return 0;
}
