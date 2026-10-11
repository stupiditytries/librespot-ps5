//! Spotify Connect receiver for the PS5, built on librespot.
//!
//! The app (C++) calls [`connect_run`] on a thread of its own. This crate advertises the
//! console on the local network, waits for the Spotify app to pick it, and plays what it is
//! told to. Decoded audio is handed back to the app through [`ConnectCallbacks::write`] as
//! 16-bit interleaved stereo at 44.1 kHz; the app owns the audio output.
//!
//! Credentials are cached under `cache_dir`, so after the first pairing from the phone the
//! console reconnects by itself. Pairing needs no password on the console.

pub mod feed;
pub mod flac;
pub mod host;
pub mod stream;
pub mod toast;

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::future::Future;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use futures_util::StreamExt;
use librespot::connect::{ConnectConfig, Spirc};
use librespot::core::authentication::Credentials;
use librespot::core::cache::Cache;
use librespot::core::config::{DeviceType, SessionConfig};
use librespot::core::session::Session;
use librespot::discovery::Discovery;
use librespot::metadata::audio::{AudioItem, UniqueFields};
use librespot::playback::audio_backend::{Sink, SinkError, SinkResult};
use librespot::playback::config::{Bitrate, PlayerConfig};
use librespot::playback::convert::Converter;
use librespot::playback::decoder::AudioPacket;
use librespot::playback::mixer::softmixer::SoftMixer;
use librespot::playback::mixer::{Mixer, MixerConfig};
use librespot::playback::player::{Player, PlayerEvent, PlayerEventChannel};
use sha1::{Digest, Sha1};
use tokio::sync::Notify;

/// What the app provides. Every function is called from this crate's own threads.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ConnectCallbacks {
    pub user_data: *mut c_void,
    /// One line of log text, without a newline.
    pub log: extern "C" fn(user_data: *mut c_void, line: *const c_char),
    /// Interleaved 16-bit stereo samples (`count` values, so `count / 2` frames) at 44.1 kHz.
    /// May block until the audio device has room; that is how playback is paced. Return 0 on
    /// success; anything else stops the current track.
    pub write: extern "C" fn(user_data: *mut c_void, samples: *const i16, count: usize) -> c_int,
    /// Playback is about to produce audio (open the output). May be null.
    pub start: Option<extern "C" fn(user_data: *mut c_void)>,
    /// Playback paused or ended (the output may be closed or drained). May be null.
    pub stop: Option<extern "C" fn(user_data: *mut c_void)>,
}

// The callbacks are plain function pointers; the app promises `user_data` may be used from
// any thread.
unsafe impl Send for ConnectCallbacks {}
unsafe impl Sync for ConnectCallbacks {}

/// Settings for [`connect_run`]. Strings are NUL-terminated UTF-8 and are copied.
#[repr(C)]
pub struct ConnectSettings {
    /// Name shown in the Spotify app's device list.
    pub name: *const c_char,
    /// Folder for the cached login (created if missing). Null disables the cache, so the
    /// phone must pick the console again after every start.
    pub cache_dir: *const c_char,
    /// Streaming quality in kbit/s: 96, 160 or 320. Anything else means 160.
    pub bitrate: u32,
    /// Volume used before the phone sets one, 0 to 100.
    pub initial_volume: u32,
    /// Non-zero logs librespot's own debug messages too.
    pub debug: c_int,
}

struct Settings {
    name: String,
    cache_dir: Option<PathBuf>,
    bitrate: Bitrate,
    initial_volume: u16,
    debug: bool,
}

/// Wakes the run loop when [`connect_stop`] is called. A stored permit makes a stop that
/// arrives before the loop starts waiting count as well.
fn stop_signal() -> &'static Notify {
    static STOP: OnceLock<Notify> = OnceLock::new();
    STOP.get_or_init(Notify::new)
}

static RUNNING: AtomicBool = AtomicBool::new(false);

// ---------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------

type LogFunction = extern "C" fn(user_data: *mut c_void, line: *const c_char);

struct AppLogger {
    log: LogFunction,
    user_data: *mut c_void,
    max: log::LevelFilter,
}

// As for the callbacks: `user_data` may be used from any thread.
unsafe impl Send for AppLogger {}
unsafe impl Sync for AppLogger {}

/// Sends this crate's and librespot's log lines to the app. The first call decides; later
/// ones change nothing.
fn install_logger(log: LogFunction, user_data: *mut c_void, debug: bool) {
    let max = if debug { log::LevelFilter::Debug } else { log::LevelFilter::Info };
    if log::set_boxed_logger(Box::new(AppLogger { log, user_data, max })).is_ok() {
        log::set_max_level(max);
    }
}

impl log::Log for AppLogger {
    fn enabled(&self, metadata: &log::Metadata) -> bool {
        metadata.level() <= self.max
    }

    fn log(&self, record: &log::Record) {
        if !self.enabled(record.metadata()) {
            return;
        }
        // Third-party crates are chatty at info level; keep their lines to warnings.
        let target = record.target();
        let ours = target.starts_with("connect_ps5") || target.starts_with("librespot");
        if !ours && record.level() > log::Level::Warn {
            return;
        }
        // The discovery responder warns about every packet from other devices that it
        // cannot read, several times a second on a busy network.
        if target.starts_with("libmdns") && record.level() > log::Level::Error {
            return;
        }
        let text = format!("{} {}: {}", record.level(), target, record.args());
        if let Ok(line) = CString::new(text.replace('\0', " ")) {
            (self.log)(self.user_data, line.as_ptr());
        }
    }

    fn flush(&self) {}
}

fn say(callbacks: &ConnectCallbacks, text: &str) {
    if let Ok(line) = CString::new(text.replace('\0', " ")) {
        (callbacks.log)(callbacks.user_data, line.as_ptr());
    }
}

// ---------------------------------------------------------------------------------------
// Audio sink
// ---------------------------------------------------------------------------------------

/// Hands every decoded packet to the app as 16-bit samples.
struct AppSink {
    callbacks: ConnectCallbacks,
    started: bool,
}

impl Sink for AppSink {
    fn start(&mut self) -> SinkResult<()> {
        if !self.started {
            self.started = true;
            if let Some(start) = self.callbacks.start {
                start(self.callbacks.user_data);
            }
        }
        Ok(())
    }

    fn stop(&mut self) -> SinkResult<()> {
        if self.started {
            self.started = false;
            if let Some(stop) = self.callbacks.stop {
                stop(self.callbacks.user_data);
            }
        }
        Ok(())
    }

    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        let samples = packet
            .samples()
            .map_err(|e| SinkError::OnWrite(e.to_string()))?;
        if samples.is_empty() {
            return Ok(());
        }
        // The player may write before it calls start() after a seek or a track change.
        self.start()?;
        let pcm = converter.f64_to_s16(samples);
        let status = (self.callbacks.write)(self.callbacks.user_data, pcm.as_ptr(), pcm.len());
        if status == 0 {
            Ok(())
        } else {
            Err(SinkError::OnWrite(format!("audio output returned {status}")))
        }
    }
}

/// Hands every decoded packet to the stream (see `stream.rs`), with the track changes at the
/// right place between the samples.
struct TapeSink {
    tape: Arc<host::Tape>,
    /// The player's events. The player sends "track changed" from the thread that calls
    /// `write`, before the track's first packet, so reading them here puts each change
    /// exactly in front of its audio.
    events: Arc<Mutex<Option<PlayerEventChannel>>>,
    /// The track being played, to announce it again when playback starts anew.
    track: Option<feed::Track>,
}

fn track_of(item: &AudioItem) -> feed::Track {
    let (artist, album) = match &item.unique_fields {
        UniqueFields::Track { artists, album, .. } => {
            let names: Vec<&str> = artists.0.iter().map(|artist| artist.name.as_str()).collect();
            (names.join(", "), album.clone())
        }
        UniqueFields::Local { artists, album, .. } => {
            (artists.clone().unwrap_or_default(), album.clone().unwrap_or_default())
        }
        UniqueFields::Episode { show_name, .. } => (show_name.clone(), show_name.clone()),
    };
    // The cover nearest to 300 pixels: plenty for a notification, and quick to fetch.
    let cover = item
        .covers
        .iter()
        .min_by_key(|cover| (cover.width - 300).abs())
        .map(|cover| cover.url.clone())
        .unwrap_or_default();
    feed::Track {
        tags: flac::Tags { title: item.name.clone(), artist, album },
        seconds: item.duration_ms as f64 / 1000.0,
        cover,
    }
}

impl Sink for TapeSink {
    fn start(&mut self) -> SinkResult<()> {
        Ok(())
    }

    fn stop(&mut self) -> SinkResult<()> {
        Ok(())
    }

    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        if let Some(events) = self.events.lock().unwrap().as_mut() {
            while let Ok(event) = events.try_recv() {
                match event {
                    PlayerEvent::TrackChanged { audio_item } => {
                        let track = track_of(&audio_item);
                        self.track = Some(track.clone());
                        self.tape.feed.track(track);
                    }
                    // Playback starting in the middle of a track (a resume, a transfer
                    // from the phone) is announced too, so the notification shows then.
                    PlayerEvent::Playing { .. } => {
                        if let Some(track) = &self.track {
                            self.tape.feed.track(track.clone());
                        }
                    }
                    _ => {}
                }
            }
        }
        let samples = packet
            .samples()
            .map_err(|e| SinkError::OnWrite(e.to_string()))?;
        if samples.is_empty() {
            return Ok(());
        }
        let pcm = converter.f64_to_s16(samples);
        self.tape.feed.push(&pcm, &host::pause);
        Ok(())
    }
}

/// Where the cover pictures for notifications are kept on the console.
const COVER_DIR: &str = "/user/data/spotifyps5/covers";
/// How many are kept. The newest are the ones a notification may still be showing.
const COVERS_KEPT: usize = 8;

/// The file name for a cover: the picture's own identifier, which is the last part of its
/// address. Every picture has its own file, because the console remembers a picture by
/// its file name: when a few names were used in turn, a notification sometimes showed the
/// cover that had the name before (seen 2026-10-12).
fn cover_name(address: &str) -> Option<String> {
    let id: String = address.rsplit('/').next()?.chars().take(64).collect();
    (!id.is_empty() && id.chars().all(|c| c.is_ascii_alphanumeric())).then(|| format!("{id}.jpg"))
}

/// Removes all but the newest covers.
fn tidy_covers() {
    let Ok(entries) = std::fs::read_dir(COVER_DIR) else { return };
    let mut files: Vec<(std::time::SystemTime, PathBuf)> = entries
        .flatten()
        .filter(|entry| entry.path().extension().is_some_and(|extension| extension == "jpg"))
        .filter_map(|entry| Some((entry.metadata().ok()?.modified().ok()?, entry.path())))
        .collect();
    files.sort();
    for (_, path) in files.iter().rev().skip(COVERS_KEPT) {
        let _ = std::fs::remove_file(path);
    }
}

/// The largest cover picture kept. Spotify's are tens of kilobytes.
const COVER_LIMIT: usize = 2 << 20;

/// Whether a cover address is one to fetch: Spotify's picture servers, over HTTPS. The
/// address comes with the track's details, and this program fetches it from inside the
/// home network and saves what it gets, so anything else is refused.
fn cover_address_allowed(address: &str) -> bool {
    let Some(rest) = address.strip_prefix("https://") else { return false };
    let host = rest.split(['/', '?', '#']).next().unwrap_or("");
    !host.contains('@') && !host.contains(':') && (host == "i.scdn.co" || host.ends_with(".scdn.co") || host.ends_with(".spotifycdn.com"))
}

async fn fetch(session: &Session, address: &str) -> Result<bytes::Bytes, String> {
    if !cover_address_allowed(address) {
        return Err("not an address of Spotify's picture servers".to_string());
    }
    let request = http::Request::get(address)
        .body(bytes::Bytes::new())
        .map_err(|e| e.to_string())?;
    let picture = session.http_client().request_body(request).await.map_err(|e| e.to_string())?;
    if picture.len() > COVER_LIMIT {
        return Err(format!("{} bytes is too large for a cover", picture.len()));
    }
    Ok(picture)
}

/// Fetches a track's cover and saves it where the console can show it. Returns the file's
/// path, or nothing if there is no cover or it cannot be had in a few seconds.
async fn cover_file(session: &Session, track: &feed::Track) -> Option<String> {
    let path = format!("{COVER_DIR}/{}", cover_name(&track.cover)?);
    if std::fs::metadata(&path).is_ok_and(|file| file.len() > 0) {
        return Some(path);
    }
    match tokio::time::timeout(Duration::from_secs(6), fetch(session, &track.cover)).await {
        Ok(Ok(picture)) => {
            let _ = std::fs::create_dir_all(COVER_DIR);
            // Written under another name first, so the console never reads half a file.
            let partial = format!("{path}.part");
            match std::fs::write(&partial, &picture).and_then(|()| std::fs::rename(&partial, &path)) {
                Ok(()) => {
                    tidy_covers();
                    return Some(path);
                }
                Err(e) => log::warn!("could not save the cover to {path}: {e}"),
            }
        }
        Ok(Err(e)) => log::warn!("could not fetch the cover: {e}"),
        Err(_) => log::warn!("the cover took too long to fetch"),
    }
    None
}

fn send_toast(notify: host::NotifyFunction, first: &str, second: &str, icon: Option<&str>) {
    let payload = toast::payload(first, second, icon, std::time::SystemTime::now());
    let Ok(payload) = CString::new(payload) else { return };
    let code = notify(payload.as_ptr());
    if code != 0 {
        log::warn!("the console refused the notification: {code:#x}");
    }
}

/// Shows the notification for a track that has just become audible, with its cover.
/// Nothing here can hold up the audio.
async fn show_toast(session: Session, track: feed::Track, notify: host::NotifyFunction) {
    let icon = cover_file(&session, &track).await;
    send_toast(notify, &track.tags.title, &track.tags.artist, icon.as_deref());
}

// ---------------------------------------------------------------------------------------
// Run loop
// ---------------------------------------------------------------------------------------

type SpircTask = Pin<Box<dyn Future<Output = ()> + Send>>;

const MIN_RETRY: Duration = Duration::from_secs(2);
const MAX_RETRY: Duration = Duration::from_secs(60);
/// A connection that lasted this long counts as healthy and resets the back-off.
const HEALTHY: Duration = Duration::from_secs(60);

/// Stable identifier for this console: 40 hex characters derived from the name. Public so
/// that a login made elsewhere (see `examples/login.rs`) is made for the same device.
pub fn device_id_for(name: &str) -> String {
    let mut hasher = Sha1::new();
    hasher.update(b"ps5-connect:");
    hasher.update(name.as_bytes());
    hex::encode(hasher.finalize())
}

async fn run(settings: Settings, callbacks: ConnectCallbacks) -> i32 {
    let device_id = device_id_for(&settings.name);
    let mut session_config = SessionConfig::default();
    session_config.device_id = device_id.clone();
    let client_id = session_config.client_id.clone();
    // Tracks are downloaded into temporary files. The default folder (/tmp) does not exist
    // inside the console's app sandbox, where every track failed with "No such file or
    // directory" and the player skipped on. Use a folder next to the login cache, emptied
    // at start in case an earlier run was cut short.
    if let Some(dir) = &settings.cache_dir {
        let temporary = dir.join("tmp");
        let _ = std::fs::remove_dir_all(&temporary);
        match std::fs::create_dir_all(&temporary) {
            Ok(()) => session_config.tmp_dir = temporary,
            Err(e) => log::warn!("no folder for downloads at {}: {e}", temporary.display()),
        }
    }

    let cache = match &settings.cache_dir {
        Some(dir) => {
            let _ = std::fs::create_dir_all(dir);
            match Cache::new(Some(dir), Some(dir), None, None) {
                Ok(cache) => Some(cache),
                Err(e) => {
                    log::warn!("login cache unavailable: {e}");
                    None
                }
            }
        }
        None => None,
    };
    let mut last_credentials: Option<Credentials> =
        cache.as_ref().and_then(|cache| cache.credentials());
    if last_credentials.is_some() {
        log::info!("using the login saved from an earlier pairing");
    }

    let mixer: Arc<dyn Mixer> = match SoftMixer::open(MixerConfig::default()) {
        Ok(mixer) => Arc::new(mixer),
        Err(e) => {
            log::error!("mixer: {e}");
            return 1;
        }
    };
    let connect_config = ConnectConfig {
        name: settings.name.clone(),
        device_type: DeviceType::GameConsole,
        initial_volume: ((settings.initial_volume as u32 * u16::MAX as u32) / 100) as u16,
        ..ConnectConfig::default()
    };

    let mut session = Session::new(session_config.clone(), cache.clone());
    let player_config = PlayerConfig {
        bitrate: settings.bitrate,
        gapless: true,
        ..PlayerConfig::default()
    };
    let sink_callbacks = callbacks;
    // With a tape running the audio goes to it, not to the app.
    let tape = host::current();
    let sink_tape = tape.clone();
    let sink_events: Arc<Mutex<Option<PlayerEventChannel>>> = Arc::new(Mutex::new(None));
    let events_for_sink = sink_events.clone();
    let player = Player::new(player_config, session.clone(), mixer.get_soft_volume(), move || {
        match sink_tape {
            Some(tape) => Box::new(TapeSink { tape, events: events_for_sink, track: None }),
            None => Box::new(AppSink {
                callbacks: sink_callbacks,
                started: false,
            }),
        }
    });
    // A pause or resume at the console is passed on to Spotify, so the phone agrees with
    // what is heard.
    let mut player_events: Option<PlayerEventChannel> = None;
    let mut notices = None;
    if let Some(tape) = &tape {
        *sink_events.lock().unwrap() = Some(player.get_player_event_channel());
        player_events = Some(player.get_player_event_channel());
        let (sender, receiver) = tokio::sync::mpsc::unbounded_channel();
        tape.on_notice(Box::new(move |notice| {
            let _ = sender.send(notice);
        }));
        notices = Some(receiver);
    }
    let mut playing = false;
    let mut paused_from_console = false;

    // Advertise on the network. A failure is not fatal when a saved login exists.
    let mut discovery = match start_discovery(&settings, &device_id, &client_id) {
        Ok(discovery) => Some(discovery),
        Err(e) => {
            log::warn!("discovery unavailable: {e}");
            None
        }
    };
    if discovery.is_none() && last_credentials.is_none() {
        log::error!("no saved login and no discovery: nothing can connect");
        return 2;
    }
    if discovery.is_some() {
        log::info!("waiting for the Spotify app to pick \"{}\"", settings.name);
    }

    let mut spirc: Option<Spirc> = None;
    let mut spirc_task: Option<SpircTask> = None;
    let mut connecting = last_credentials.is_some();
    let mut connected_at: Option<Instant> = None;
    let mut retry = MIN_RETRY;

    loop {
        tokio::select! {
            _ = stop_signal().notified() => break,

            event = async {
                match player_events.as_mut() {
                    Some(events) => events.recv().await,
                    None => std::future::pending().await,
                }
            } => {
                match event {
                    Some(PlayerEvent::Playing { .. }) => playing = true,
                    Some(PlayerEvent::Paused { .. } | PlayerEvent::Stopped { .. }) => playing = false,
                    Some(_) => {}
                    None => player_events = None,
                }
            }

            notice = async {
                match notices.as_mut() {
                    Some(notices) => notices.recv().await,
                    None => std::future::pending().await,
                }
            } => {
                match (notice, spirc.as_ref()) {
                    (Some(stream::Notice::Paused), Some(spirc)) if playing => {
                        paused_from_console = spirc.pause().is_ok();
                    }
                    (Some(stream::Notice::Resumed), Some(spirc)) if paused_from_console => {
                        paused_from_console = false;
                        let _ = spirc.play();
                    }
                    (Some(stream::Notice::Stop), Some(spirc)) => {
                        // The stop file was played at the console.
                        paused_from_console = false;
                        if playing {
                            let _ = spirc.pause();
                        }
                    }
                    (Some(stream::Notice::Restarted(track)), _) => {
                        // The console has just put up its own notice for the file. Ours
                        // goes out a moment later and takes its place (same kind, and
                        // marked to replace), so there is one notice, not two.
                        if let Some(notify) = tape.as_ref().and_then(|tape| tape.notify) {
                            let session = session.clone();
                            tokio::spawn(async move {
                                tokio::time::sleep(Duration::from_millis(1500)).await;
                                match track {
                                    Some(track) => show_toast(session, track, notify).await,
                                    None => send_toast(notify, "librespot-ps5 ready!", "by Jack (stupiditytries)", None),
                                }
                            });
                        }
                    }
                    (Some(stream::Notice::Track(track)), _) => {
                        if let Some(notify) = tape.as_ref().and_then(|tape| tape.notify) {
                            tokio::spawn(show_toast(session.clone(), track, notify));
                        }
                    }

                    (None, _) => notices = None,
                    _ => {}
                }
            }

            credentials = async {
                match discovery.as_mut() {
                    Some(d) => d.next().await,
                    None => std::future::pending().await,
                }
            } => {
                match credentials {
                    Some(credentials) => {
                        log::info!("the Spotify app picked this console");
                        last_credentials = Some(credentials);
                        retry = MIN_RETRY;
                        shut_down_connection(&mut spirc, &mut spirc_task, &session);
                        connecting = true;
                    }
                    None => {
                        log::warn!("discovery stopped; only the saved login will work");
                        discovery = None;
                        if last_credentials.is_none() {
                            log::error!("no way left to receive a login");
                            break;
                        }
                    }
                }
            }

            _ = async {}, if connecting && last_credentials.is_some() => {
                if session.is_invalid() {
                    session = Session::new(session_config.clone(), cache.clone());
                    player.set_session(session.clone());
                }
                let credentials = last_credentials.clone().unwrap_or_default();
                match Spirc::new(
                    connect_config.clone(),
                    session.clone(),
                    credentials,
                    player.clone(),
                    mixer.clone(),
                )
                .await
                {
                    Ok((new_spirc, task)) => {
                        log::info!("connected to Spotify");
                        spirc = Some(new_spirc);
                        spirc_task = Some(Box::pin(task));
                        connected_at = Some(Instant::now());
                        connecting = false;
                    }
                    Err(e) => {
                        log::warn!("could not connect: {e}");
                        if discovery.is_some() {
                            // The saved login may be stale; wait for the phone to pick us.
                            last_credentials = None;
                            connecting = false;
                        } else {
                            tokio::select! {
                                _ = stop_signal().notified() => break,
                                _ = tokio::time::sleep(retry) => {}
                            }
                            retry = (retry * 2).min(MAX_RETRY);
                        }
                    }
                }
            }

            _ = async {
                match spirc_task.as_mut() {
                    Some(task) => task.await,
                    None => std::future::pending().await,
                }
            } => {
                spirc_task = None;
                spirc = None;
                if connected_at.take().map_or(false, |at| at.elapsed() >= HEALTHY) {
                    retry = MIN_RETRY;
                }
                if last_credentials.is_some() {
                    log::warn!("the Spotify connection ended; reconnecting in {retry:?}");
                    tokio::select! {
                        _ = stop_signal().notified() => break,
                        _ = tokio::time::sleep(retry) => {}
                    }
                    retry = (retry * 2).min(MAX_RETRY);
                    if !session.is_invalid() {
                        session.shutdown();
                    }
                    connecting = true;
                } else {
                    log::info!("the Spotify connection ended");
                }
            }
        }
    }

    log::info!("stopping");
    if let Some(spirc) = spirc.take() {
        let _ = spirc.shutdown();
    }
    if let Some(task) = spirc_task.take() {
        let _ = tokio::time::timeout(Duration::from_secs(3), task).await;
    }
    if !session.is_invalid() {
        session.shutdown();
    }
    if let Some(discovery) = discovery.take() {
        let _ = tokio::time::timeout(Duration::from_secs(3), discovery.shutdown()).await;
    }
    0
}

/// The port the Spotify app connects to when it picks this device. A fixed one: on the
/// console a port chosen by the system (65281, say) refused connections from the network,
/// while the console's own Spotify service is reachable on 41800.
const PAIRING_PORT: u16 = 41801;

fn start_discovery(
    settings: &Settings,
    device_id: &str,
    client_id: &str,
) -> Result<Discovery, librespot::core::Error> {
    let backend = librespot::discovery::find(None)?;
    let mut builder = Discovery::builder(device_id.to_string(), client_id.to_string())
        .name(settings.name.clone())
        .device_type(DeviceType::GameConsole)
        .port(PAIRING_PORT)
        .zeroconf_backend(backend);
    // On a PC with several network adapters (virtual machines, WSL) the announcement can go
    // out with addresses the phone cannot reach. CONNECT_ZEROCONF_IP limits it to one.
    if let Some(address) = std::env::var("CONNECT_ZEROCONF_IP")
        .ok()
        .and_then(|text| text.parse::<std::net::IpAddr>().ok())
    {
        builder = builder.zeroconf_ip(vec![address]);
    }
    builder.launch()
}

fn shut_down_connection(
    spirc: &mut Option<Spirc>,
    spirc_task: &mut Option<SpircTask>,
    session: &Session,
) {
    if let Some(old) = spirc.take() {
        if let Err(e) = old.shutdown() {
            log::warn!("error asking the old connection to stop: {e}");
        }
    }
    if let Some(task) = spirc_task.take() {
        // Let the old connection finish stopping on its own.
        tokio::spawn(task);
    }
    if !session.is_invalid() {
        session.shutdown();
    }
}

// ---------------------------------------------------------------------------------------
// C interface
// ---------------------------------------------------------------------------------------

fn text(pointer: *const c_char) -> Option<String> {
    if pointer.is_null() {
        return None;
    }
    let value = unsafe { CStr::from_ptr(pointer) }.to_string_lossy().into_owned();
    (!value.is_empty()).then_some(value)
}

/// Runs the receiver until [`connect_stop`] is called. Blocks, so call it from a thread of
/// its own. Returns 0 after a requested stop and a non-zero code if it could not start.
#[no_mangle]
pub extern "C" fn connect_run(
    settings: *const ConnectSettings,
    callbacks: *const ConnectCallbacks,
) -> c_int {
    if settings.is_null() || callbacks.is_null() {
        return -1;
    }
    if RUNNING.swap(true, Ordering::SeqCst) {
        return -2;
    }
    let callbacks = unsafe { *callbacks };
    let raw = unsafe { &*settings };
    let settings = Settings {
        name: text(raw.name).unwrap_or_else(|| "PS5".to_string()),
        cache_dir: text(raw.cache_dir).map(PathBuf::from),
        bitrate: match raw.bitrate {
            96 => Bitrate::Bitrate96,
            320 => Bitrate::Bitrate320,
            _ => Bitrate::Bitrate160,
        },
        initial_volume: raw.initial_volume.min(100) as u16,
        debug: raw.debug != 0,
    };

    install_logger(callbacks.log, callbacks.user_data, settings.debug);

    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(e) => {
            say(&callbacks, &format!("ERROR connect_ps5: no async runtime: {e}"));
            // Say which part fails, so the next run needs no guessing.
            let time_only = tokio::runtime::Builder::new_current_thread()
                .enable_time()
                .build()
                .map(|_| "ok".to_string())
                .unwrap_or_else(|e| e.to_string());
            let io_only = tokio::runtime::Builder::new_current_thread()
                .enable_io()
                .build()
                .map(|_| "ok".to_string())
                .unwrap_or_else(|e| e.to_string());
            say(&callbacks, &format!("  time driver alone: {time_only}; io driver alone: {io_only}"));
            RUNNING.store(false, Ordering::SeqCst);
            return 3;
        }
    };
    let code = runtime.block_on(run(settings, callbacks));
    // Do not wait for lingering tasks (a connection that will not close, say).
    runtime.shutdown_timeout(Duration::from_secs(2));
    RUNNING.store(false, Ordering::SeqCst);
    code
}

/// Gives the receiver a secret of the device it runs on (an identifier that programs on
/// the device can read and that is not in any file). The saved login is encrypted with it,
/// so a copy of the file is of no use elsewhere. Call before [`connect_run`]. Without it
/// the saved login is still not readable text, but any copy of this program can read it.
#[no_mangle]
pub extern "C" fn connect_set_secret(secret: *const u8, length: usize) {
    if secret.is_null() || length == 0 {
        return;
    }
    let secret = unsafe { std::slice::from_raw_parts(secret, length) };
    librespot::core::cache::set_credentials_secret(secret);
}

/// Asks [`connect_run`] to return. Safe to call from any thread, and before it has started
/// waiting. Also stops the tape, if one is running.
#[no_mangle]
pub extern "C" fn connect_stop() {
    stop_signal().notify_one();
    if let Some(tape) = host::current() {
        tape.stop();
    }
}

/// Starts playback through the console's USB music player (see `stream.rs`). Call before
/// [`connect_run`], which then sends its audio there and no longer calls `write`.
/// `data_dir` is the folder that holds `tape.txt`. `notify`, if given, shows a
/// notification on the console; it is used when a track starts. Returns 0 if the stream
/// is running, and a non-zero code (with the reason in the log) if it is not.
#[no_mangle]
pub extern "C" fn connect_tape_start(
    data_dir: *const c_char,
    log: LogFunction,
    user_data: *mut c_void,
    debug: c_int,
    notify: Option<host::NotifyFunction>,
) -> c_int {
    install_logger(log, user_data, debug != 0);
    let Some(data_dir) = text(data_dir) else { return -1 };
    match host::start(&PathBuf::from(data_dir), notify, debug != 0) {
        Ok(_) => 0,
        Err(reason) => {
            log::error!("tape: {reason}");
            1
        }
    }
}

/// Feeds the tape test audio (see `host::run_test`) until [`connect_stop`] is called.
/// For trying the tape without Spotify. Blocks. `data_dir` may hold `test.pcm`.
#[no_mangle]
pub extern "C" fn connect_tape_test(data_dir: *const c_char) -> c_int {
    let (Some(tape), Some(data_dir)) = (host::current(), text(data_dir)) else { return -1 };
    host::run_test(&tape, &PathBuf::from(data_dir));
    0
}

#[cfg(test)]
mod tests {
    use super::{cover_address_allowed, cover_name};
    use librespot::core::cache::{seal, unseal};

    #[test]
    fn the_saved_login_is_sealed() {
        let login = br#"{"username":"someone","auth_type":1,"auth_data":"c2VjcmV0"}"#;
        let sealed = seal(b"this console", login);
        assert!(!sealed.windows(7).any(|part| part == b"someone"));
        assert!(!sealed.windows(8).any(|part| part == b"c2VjcmV0"));
        assert_eq!(unseal(b"this console", &sealed).as_deref(), Some(&login[..]));
        // Not on another device, and not after any change to the file.
        assert_eq!(unseal(b"another console", &sealed), None);
        for at in [0, 5, 21, sealed.len() - 1] {
            let mut changed = sealed.clone();
            changed[at] ^= 1;
            assert_eq!(unseal(b"this console", &changed), None, "byte {at}");
        }
        assert_eq!(unseal(b"this console", &sealed[..sealed.len() - 1]), None);
        // Sealing twice does not give the same bytes.
        assert_ne!(seal(b"this console", login), sealed);
    }

    #[test]
    fn each_cover_has_its_own_file() {
        assert_eq!(
            cover_name("https://i.scdn.co/image/ab67616d00001e02abcdef").as_deref(),
            Some("ab67616d00001e02abcdef.jpg")
        );
        assert_ne!(cover_name("https://i.scdn.co/image/aaa1"), cover_name("https://i.scdn.co/image/aaa2"));
        // Nothing that could name a file somewhere else.
        for address in ["", "https://i.scdn.co/image/", "https://i.scdn.co/image/..", "https://i.scdn.co/image/a.b"] {
            assert_eq!(cover_name(address), None, "{address}");
        }
    }

    #[test]
    fn covers_come_only_from_spotify() {
        assert!(cover_address_allowed("https://i.scdn.co/image/ab67616d00001e02abcdef"));
        for address in [
            "http://i.scdn.co/image/x",
            "https://192.168.0.1/admin",
            "https://i.scdn.co.example.com/x",
            "https://example.com/i.scdn.co",
            "https://i.scdn.co@example.com/x",
            "https://i.scdn.co:8443/x",
            "file:///data/spotifyps5/cache/credentials.json",
            "",
        ] {
            assert!(!cover_address_allowed(address), "{address}");
        }
    }
}
