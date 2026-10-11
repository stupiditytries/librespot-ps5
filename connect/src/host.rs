//! Runs the stream (see `stream.rs`) on the console: the two files on a USB drive, the
//! console's log as the clock, a settings file, and the threads that keep it going.

use std::ffi::{c_char, c_int};
use std::fs::{File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::net::TcpStream;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{mpsc, Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use crate::flac::{Tags, RATE};
use crate::stream::{self, Config, Notice, Stream};
use crate::feed::{Feed, Signal, Store, Track};

const SETTINGS_FILE: &str = "tape.txt";
/// The folders, inside a USB drive's Music folder. Each holds one file, so the console's
/// player has nothing to skip to.
pub const STREAM_FOLDER: &str = "Start Librespot";
pub const CONTROLS_FOLDER: &str = "Librespot Controls";
/// Where the console's log can be read (klogsrv, part of the jailbreak set-up).
const LOG_ADDRESS: &str = "127.0.0.1:3232";
/// On connecting, the log server first repeats old lines. Lines in this time are ignored.
const LOG_HISTORY: Duration = Duration::from_secs(3);
const TICK: Duration = Duration::from_millis(20);

/// Waits. The standard library's sleep is not used: with the console's C library a timed
/// wait has been seen to return at once, and usleep is known to work there.
pub fn pause(time: Duration) {
    #[cfg(unix)]
    {
        extern "C" {
            fn usleep(microseconds: u32) -> i32;
        }
        unsafe { usleep((time.as_micros() as u32).max(1000)) };
    }
    #[cfg(not(unix))]
    std::thread::sleep(time);
}

// ---------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------

const SETTINGS_TEXT: &str = "\
# Settings for playback through the console's USB music player. Read when the program starts.
# lead: seconds of audio written ahead of what is heard. Everything done on the phone is
#       heard this much later.
lead=2
# idle: seconds without audio after which the stream stops writing to the drive.
idle=600
# hours: length of the stream file. The stream has to be started again after this long.
#        One hour takes about 635 MB on the drive.
hours=12
# toast: 1 shows a notification with the cover, title and artist when a track starts.
toast=1
# toast_style: which kind of notification: 0 the console's own small music kind, which
#              takes the place of the console's own notice (not shown over games); 2 a
#              larger general kind, shown as well as the console's own; 3 the same on a
#              channel the console's pop-up settings do not hold back.
toast_style=0
";

struct Settings {
    config: Config,
    toast: bool,
}

fn read_settings(data_dir: &Path) -> Settings {
    let path = data_dir.join(SETTINGS_FILE);
    let text = std::fs::read_to_string(&path).unwrap_or_else(|_| {
        if let Err(e) = std::fs::write(&path, SETTINGS_TEXT) {
            log::warn!("could not write {}: {e}", path.display());
        }
        SETTINGS_TEXT.to_string()
    });
    let mut settings = Settings { config: Config::default(), toast: true };
    let (mut repeat, mut bits, mut frame) = (1, 16, 4096);
    for line in text.lines() {
        let line = line.trim();
        let Some((key, value)) = line.split_once('=') else { continue };
        if line.starts_with('#') {
            continue;
        }
        let number = value.trim().parse::<f64>().ok().filter(|number| number.is_finite());
        let config = &mut settings.config;
        match (key.trim(), number) {
            ("lead", Some(number)) => config.lead = number.clamp(1.5, 120.0),
            ("slack", Some(number)) => config.slack = number.clamp(0.1, 5.0),
            ("prefill", Some(number)) => config.prefill = number.clamp(5.0, 300.0),
            ("idle", Some(number)) => config.idle_limit = number.max(30.0),
            ("hours", Some(number)) => config.hours = number.clamp(0.5, 48.0),
            ("length", Some(number)) => config.show_length = number != 0.0,
            ("grow", Some(number)) => config.grow = number != 0.0,
            ("toast", Some(number)) => settings.toast = number != 0.0,
            ("toast_style", Some(number)) => crate::toast::set_style(number as u8),
            ("rate", Some(number)) => repeat = number as usize,
            ("bits", Some(number)) => bits = number as usize,
            ("frame", Some(number)) => frame = number as usize,
            _ => log::warn!("{}: did not understand \"{line}\"", path.display()),
        }
    }
    crate::flac::set_format(repeat, bits);
    crate::flac::set_block(frame);
    let config = &mut settings.config;
    config.prefill = config.prefill.max(config.lead + 3.0);
    settings
}

// ---------------------------------------------------------------------------------------
// The files
// ---------------------------------------------------------------------------------------

fn child_folder(parent: &Path, name: &str) -> Option<PathBuf> {
    std::fs::read_dir(parent)
        .ok()?
        .flatten()
        .find(|entry| {
            entry.file_name().to_string_lossy().eq_ignore_ascii_case(name) && entry.path().is_dir()
        })
        .map(|entry| entry.path())
}

/// Looks for the two folders on every USB drive. Drive numbers change between sessions,
/// so they are found by what is on the drive.
fn find_folders() -> Option<(PathBuf, PathBuf)> {
    (0..8).find_map(|number| {
        let music = child_folder(Path::new(&format!("/mnt/usb{number}")), "Music")?;
        Some((child_folder(&music, STREAM_FOLDER)?, child_folder(&music, CONTROLS_FOLDER)?))
    })
}

/// Whether a drive is plugged in at `path`: the folder is there either way, and is on the
/// drive only if it is on another device than the folder that holds it.
fn mounted(path: &Path) -> bool {
    #[cfg(unix)]
    {
        use std::os::unix::fs::MetadataExt;
        let device = |path: &Path| std::fs::metadata(path).map(|data| data.dev());
        match (device(path), path.parent().map(device)) {
            (Ok(here), Some(Ok(above))) => here != above,
            _ => false,
        }
    }
    #[cfg(not(unix))]
    {
        let _ = path;
        false
    }
}

/// The two folders, made if no drive has them yet: on the first USB drive that has a Music
/// folder, and otherwise on the first USB drive, in a Music folder made for them.
fn find_or_make_folders() -> Result<(PathBuf, PathBuf), String> {
    if let Some(found) = find_folders() {
        return Ok(found);
    }
    let drives: Vec<PathBuf> = (0..8)
        .map(|number| PathBuf::from(format!("/mnt/usb{number}")))
        .filter(|drive| mounted(drive))
        .collect();
    let music = drives
        .iter()
        .find_map(|drive| child_folder(drive, "Music"))
        .or_else(|| drives.first().map(|drive| drive.join("Music")))
        .ok_or("no USB drive is plugged in (it must be one the console shows music from, not one formatted as extended storage)")?;
    let folders = (music.join(STREAM_FOLDER), music.join(CONTROLS_FOLDER));
    for folder in [&folders.0, &folders.1] {
        std::fs::create_dir_all(folder).map_err(|e| format!("could not make {}: {e}", folder.display()))?;
    }
    log::warn!("made the folders \"{STREAM_FOLDER}\" and \"{CONTROLS_FOLDER}\" in {}", music.display());
    Ok(folders)
}

/// The stream file and the stop file, kept open.
struct FileStore {
    files: Vec<Mutex<File>>,
}

impl FileStore {
    fn open(paths: &[PathBuf]) -> io::Result<FileStore> {
        let open = |path: &PathBuf| {
            let file = OpenOptions::new().read(true).write(true).create(true).truncate(false).open(path)?;
            Ok(Mutex::new(file))
        };
        Ok(FileStore { files: paths.iter().map(open).collect::<io::Result<_>>()? })
    }
}

impl Store for FileStore {
    fn len(&self, slot: usize) -> io::Result<u64> {
        Ok(self.files[slot].lock().unwrap().metadata()?.len())
    }

    fn set_len(&self, slot: usize, length: u64) -> io::Result<()> {
        self.files[slot].lock().unwrap().set_len(length)
    }

    fn read_at(&self, slot: usize, offset: u64, buffer: &mut [u8]) -> io::Result<usize> {
        let mut file = self.files[slot].lock().unwrap();
        file.seek(SeekFrom::Start(offset))?;
        let mut done = 0;
        while done < buffer.len() {
            match file.read(&mut buffer[done..])? {
                0 => break,
                count => done += count,
            }
        }
        Ok(done)
    }

    fn write_at(&self, slot: usize, offset: u64, data: &[u8]) -> io::Result<()> {
        let mut file = self.files[slot].lock().unwrap();
        file.seek(SeekFrom::Start(offset))?;
        file.write_all(data)
    }
}

// ---------------------------------------------------------------------------------------
// The console's log
// ---------------------------------------------------------------------------------------

/// The copy of the console's log stops growing at this size.
const LOG_COPY_LIMIT: usize = 16 << 20;

fn watch_log(signals: mpsc::Sender<Signal>, stop: Arc<AtomicBool>, copy_to: Option<PathBuf>) {
    // When asked for, everything read is also kept in a file: reading the log here may
    // keep the lines from a reader on the PC, and they are needed to see what the player
    // did.
    let mut copy = copy_to.and_then(|path| File::create(path).ok());
    let mut copied = 0;
    let mut complained = false;
    while !stop.load(Ordering::Relaxed) {
        let mut stream = match TcpStream::connect(LOG_ADDRESS) {
            Ok(stream) => stream,
            Err(e) => {
                if !complained {
                    log::error!(
                        "cannot read the console's log at {LOG_ADDRESS}: {e}. Without it the tape cannot tell where the player is"
                    );
                    complained = true;
                }
                pause(Duration::from_secs(5));
                continue;
            }
        };
        log::info!("reading the console's log");
        complained = false;
        let connected = Instant::now();
        let mut pending: Vec<u8> = Vec::new();
        let mut buffer = [0u8; 16384];
        loop {
            let count = match stream.read(&mut buffer) {
                Ok(0) => break,
                Ok(count) => count,
                Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
                Err(_) => break,
            };
            if stop.load(Ordering::Relaxed) {
                return;
            }
            if copied < LOG_COPY_LIMIT {
                if let Some(file) = copy.as_mut() {
                    let _ = file.write_all(&buffer[..count]);
                    copied += count;
                }
            }
            pending.extend(&buffer[..count]);
            let complete = pending.iter().rposition(|&byte| byte == b'\n').map_or(0, |at| at + 1);
            let lines: Vec<u8> = pending.drain(..complete).collect();
            if pending.len() > 65536 {
                pending.clear();
            }
            if connected.elapsed() < LOG_HISTORY {
                continue;
            }
            for line in lines.split(|&byte| byte == b'\n') {
                let has = |needle: &[u8]| line.windows(needle.len()).any(|part| part == needle);
                let signal = if has(b"NotifyPlaybackStartFromPeer") {
                    Signal::Start
                } else if has(b"NotifyPlaybackTerminationFromPeer") {
                    Signal::End
                } else if has(b"# proc name: SystemMusicCore.elf") {
                    // From the kernel's report of a fatal signal in the player.
                    Signal::Crashed

                } else {
                    continue;
                };
                if signals.send(signal).is_err() {
                    return;
                }
            }
        }
        log::warn!("the console's log closed; connecting again");
        pause(Duration::from_secs(2));
    }
}


// ---------------------------------------------------------------------------------------
// The running stream
// ---------------------------------------------------------------------------------------

type NoticeHandler = Box<dyn Fn(Notice) + Send>;
/// Shows a notification on the console from its description (JSON). Returns 0 if it was
/// accepted.
pub type NotifyFunction = extern "C" fn(payload: *const c_char) -> c_int;

pub struct Tape {
    pub feed: Arc<Feed>,
    /// How to show a notification, if the app provided a way and the setting is on.
    pub notify: Option<NotifyFunction>,
    stop: Arc<AtomicBool>,
    on_notice: Mutex<Option<NoticeHandler>>,
}

static TAPE: OnceLock<Arc<Tape>> = OnceLock::new();

/// The stream, if one was started.
pub fn current() -> Option<Arc<Tape>> {
    TAPE.get().cloned()
}

impl Tape {
    /// Has `handler` told what the stream notices (see [`Notice`]).
    pub fn on_notice(&self, handler: NoticeHandler) {
        *self.on_notice.lock().unwrap() = Some(handler);
    }

    pub fn stop(&self) {
        self.stop.store(true, Ordering::Relaxed);
    }

    fn stopping(&self) -> bool {
        self.stop.load(Ordering::Relaxed)
    }
}

/// Starts the stream. `data_dir` holds the settings file. Fails if there is no USB drive
/// to keep the two folders on.
pub fn start(data_dir: &Path, notify: Option<NotifyFunction>, debug: bool) -> Result<Arc<Tape>, String> {
    if TAPE.get().is_some() {
        return Err("the stream is already running".to_string());
    }
    let settings = read_settings(data_dir);
    let (stream_folder, controls_folder) = find_or_make_folders()?;
    let config = settings.config;
    log::info!(
        "stream: {}{}, frames of {}, {} s ahead of the player, stops after {} s without audio, notifications {}",
        stream_folder.display(),
        if config.grow { ", growing file" } else { "" },
        crate::flac::block(),
        config.lead,
        config.idle_limit,
        if settings.toast && notify.is_some() { "on" } else { "off" }
    );
    let paths = [
        stream_folder.join(format!("{}.flac", stream::STREAM_TITLE)),
        controls_folder.join(format!("{}.flac", stream::STOP_TITLE)),
    ];
    if paths.iter().any(|path| !path.exists()) {
        log::warn!(
            "the files are new: the console's music menu will not show them in a folder it has already listed until it has listed another folder with music in it, or the console has restarted"
        );
    }
    let store = FileStore::open(&paths).map_err(|e| format!("the stream files: {e}"))?;
    let feed = Feed::new();
    let mut stream = Stream::new(config, Arc::new(store), feed.clone())
        .map_err(|e| format!("preparing the stream file: {e}"))?;

    let tape = Arc::new(Tape {
        feed,
        notify: notify.filter(|_| settings.toast),
        stop: Arc::new(AtomicBool::new(false)),
        on_notice: Mutex::new(None),
    });
    let (sender, receiver) = mpsc::channel();
    let stop = tape.stop.clone();
    // The copy of the console's log is for finding faults: it is large, and the log has
    // other programs' lines in it.
    let copy_to = debug.then(|| data_dir.join("klog.txt"));
    std::thread::spawn(move || watch_log(sender, stop, copy_to));

    let running = tape.clone();
    std::thread::spawn(move || {
        let begun = Instant::now();
        let mut last_status = 0.0;
        let mut signals = Vec::new();
        while !running.stopping() {
            signals.clear();
            signals.extend(receiver.try_iter());
            let now = begun.elapsed().as_secs_f64();
            for &signal in &signals {
                log::debug!("console log: {signal:?} at {now:.2}");
            }
            for notice in stream.tick(now, &signals) {
                if let Notice::Track(track) = &notice {
                    log::info!("now heard: \"{}\" by \"{}\"", track.tags.title, track.tags.artist);
                }
                if let Some(handler) = running.on_notice.lock().unwrap().as_ref() {
                    handler(notice);
                }
            }
            if now - last_status >= 30.0 {
                last_status = now;
                log::debug!("stream: {}", stream.status(now));
            }
            pause(TICK);
        }
    });
    TAPE.set(tape.clone()).map_err(|_| "the stream is already running".to_string())?;
    Ok(tape)
}

// ---------------------------------------------------------------------------------------
// Test audio, for trying the tape without Spotify
// ---------------------------------------------------------------------------------------

/// How long each test track is.
const TEST_TRACK_SECONDS: u64 = 60;

/// Feeds the stream test audio until it is stopped, as a new track every minute. The audio
/// is `test.pcm` in `data_dir` (16-bit stereo at 44.1 kHz, no header) played round and
/// round if that file exists, and otherwise a steady note per track with a short pip every
/// second, so a gap or a repeat can be heard.
pub fn run_test(tape: &Tape, data_dir: &Path) {
    let recording: Vec<i16> = std::fs::read(data_dir.join("test.pcm"))
        .map(|bytes| bytes.chunks_exact(2).map(|b| i16::from_le_bytes([b[0], b[1]])).collect())
        .unwrap_or_default();
    let recording = &recording[..recording.len() / 2 * 2];
    if recording.is_empty() {
        log::info!("stream test: no test.pcm, playing notes");
    } else {
        log::info!("stream test: playing test.pcm ({} s)", recording.len() as u64 / 2 / RATE);
    }
    const NOTES: [f64; 5] = [220.0, 247.5, 277.2, 330.0, 370.0];
    let mut at_recording = 0;
    let mut number = 0u64;
    'tracks: loop {
        // Nothing is announced until the player has been started, so the first track is
        // heard from its beginning.
        while !tape.feed.accepting() {
            if tape.stopping() {
                break 'tracks;
            }
            pause(Duration::from_millis(100));
        }
        number += 1;
        let tags = Tags {
            title: format!("Stream test {number}"),
            artist: "spotifyps5".to_string(),
            album: "Stream test".to_string(),
        };
        log::info!("stream test: track {number}");
        tape.feed.track(Track { tags, seconds: TEST_TRACK_SECONDS as f64, cover: String::new() });
        let note = NOTES[(number as usize - 1) % NOTES.len()];
        let mut pair = 0u64;
        let mut chunk = Vec::with_capacity(2048);
        while pair < TEST_TRACK_SECONDS * RATE {
            chunk.clear();
            for _ in 0..1024 {
                if recording.is_empty() {
                    let time = pair as f64 / RATE as f64;
                    let mut value = 0.12 * (time * note * std::f64::consts::TAU).sin();
                    if pair % RATE < RATE / 25 {
                        value += 0.2 * (time * 1760.0 * std::f64::consts::TAU).sin();
                    }
                    let sample = (value * 32767.0) as i16;
                    chunk.extend([sample, sample]);
                } else {
                    chunk.extend([recording[at_recording], recording[at_recording + 1]]);
                    at_recording = (at_recording + 2) % recording.len();
                }
                pair += 1;
            }
            if tape.stopping() {
                break 'tracks;
            }
            // While the tape is not running (not started, paused, stopped), wait.
            while !tape.feed.accepting() {
                if tape.stopping() {
                    break 'tracks;
                }
                pause(Duration::from_millis(50));
            }
            tape.feed.push(&chunk, &pause);
        }
    }
    log::info!("stream test: stopped");
}
