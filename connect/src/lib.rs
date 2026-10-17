//! Spotify Connect receiver for the PS5, built on librespot.
//!
//! The app (C++) calls [`connect_run`] on a thread of its own. This crate advertises the
//! console on the local network, waits for the Spotify app to pick it, and plays what it is
//! told to. Decoded audio is handed back to the app through [`ConnectCallbacks::write`] as
//! 16-bit interleaved stereo at 44.1 kHz; the app owns the audio output.
//!
//! Credentials are cached under `cache_dir`, so after the first pairing from the phone the
//! console reconnects by itself. Pairing needs no password on the console.

pub mod control;
#[cfg(feature="kabuki-provider")]
mod kabuki;
pub mod feed;
pub mod flac;
pub mod host;
pub mod library;
mod media_api;
mod media_features;
mod login;
#[cfg(feature="kabuki-provider")]
mod track_notices;
#[allow(dead_code)] // Kept for reference: library views now use client_api.rs.
mod web_api;
mod client_api;
mod browse_cache;
mod native_library;
#[cfg(feature="shell-pcm")]
mod pcm;
pub mod stream;
pub mod toast;
#[cfg(feature="usb-autostart")]
mod usb_policy;
#[cfg(feature="usb-autostart")]
mod transport;

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::future::Future;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use futures_util::StreamExt;
use librespot::connect::{ConnectConfig, LoadRequest, Spirc};
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
#[cfg(not(feature="shell-pcm"))]
struct AppSink {
    callbacks: ConnectCallbacks,
    started: bool,
}

#[cfg(not(feature="shell-pcm"))]
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
const COVERS_KEPT: usize = 32;

/// The file name for a cover: the picture's own identifier, which is the last part of its
/// address. Every picture has its own file, because the console remembers a picture by
/// its file name: when a few names were used in turn, a notification sometimes showed the
/// cover that had the name before (seen 2026-10-12).
fn cover_name(address: &str) -> Option<String> {
    let path = address.split(['?', '#']).next()?;
    let id = path.rsplit('/').next()?;
    if id.is_empty() || id=="." || id==".." { return None; }
    let simple=id.chars().all(|c|c.is_ascii_alphanumeric());
    let image_file=[".jpg",".jpeg",".png",".webp"].iter().any(|ext|id.to_ascii_lowercase().ends_with(ext))
        && id.chars().all(|c|c.is_ascii_alphanumeric()||matches!(c,'.'|'_'|'-'));
    if !simple&&!image_file{return None;}
    // Official Liked Songs and generated artwork use full image filenames.
    // Hash those URLs and long mosaics so filenames stay distinct and safe.
    if !path.starts_with("https://i.scdn.co/image/") || !simple || id.len() > 64 {
        use sha1::{Digest, Sha1};
        return Some(format!("{}.jpg", hex::encode(Sha1::digest(path.as_bytes()))));
    }
    Some(format!("{id}.jpg"))
}

/// Where the queue's cover pictures are kept, and how many.
const QUEUE_COVER_DIR: &str = "/user/data/spotifyps5/queue-covers";
const QUEUE_COVERS_KEPT: usize = 128;

/// Removes all but the newest `keep` covers in `dir`.
fn tidy_covers(dir: &str, keep: usize) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    let mut files: Vec<(std::time::SystemTime, PathBuf)> = entries
        .flatten()
        .filter(|entry| entry.path().extension().is_some_and(|extension| extension == "jpg"))
        .filter_map(|entry| Some((entry.metadata().ok()?.modified().ok()?, entry.path())))
        .collect();
    files.sort();
    for (_, path) in files.iter().rev().skip(keep) {
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

// Match the formats accepted by Kabuki's image validator. A .jpg name alone is
// not evidence: old downloads may contain WebP, an HTTP error page or partial data.
fn supported_picture_header(bytes: &[u8]) -> bool {
    bytes.starts_with(&[0xff, 0xd8, 0xff]) ||
        bytes.starts_with(&[0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a])
}

fn cached_picture_supported(path: &str) -> bool {
    use std::io::Read;
    let Ok(info) = std::fs::symlink_metadata(path) else { return false };
    if !info.is_file() || info.len() == 0 || info.len() > COVER_LIMIT as u64 { return false; }
    let Ok(mut file) = std::fs::File::open(path) else { return false };
    let mut head = [0u8; 8];
    let Ok(n) = file.read(&mut head) else { return false };
    supported_picture_header(&head[..n])
}

fn canonical_cover(bytes:&[u8])->Result<bytes::Bytes,String>{
    let mut reader=image::ImageReader::new(std::io::Cursor::new(bytes)).with_guessed_format().map_err(|_|"Unknown cover format")?;
    let mut limits=image::Limits::default();limits.max_image_width=Some(4096);limits.max_image_height=Some(4096);limits.max_alloc=Some(64*1024*1024);
    reader.limits(limits);
    let image=reader.decode().map_err(|_|"Could not decode artwork")?.thumbnail(1024,1024).to_rgb8();
    let mut output=Vec::new();
    image::codecs::jpeg::JpegEncoder::new_with_quality(&mut output,88).encode_image(&image).map_err(|_|"Could not encode artwork")?;
    if output.len()>COVER_LIMIT{return Err("Artwork is too large".into());}
    Ok(bytes::Bytes::from(output))
}

async fn fetch(session: &Session, address: &str) -> Result<bytes::Bytes, String> {
    if !cover_address_allowed(address) {
        return Err("not an address of Spotify's picture servers".to_string());
    }
    let request = http::Request::get(address)
        .header(http::header::ACCEPT, "image/jpeg, image/png")
        .body(bytes::Bytes::new())
        .map_err(|e| e.to_string())?;
    use http_body_util::BodyExt;
    let response=session.http_client().request_fut(request).map_err(|_|"Artwork network unavailable")?.await.map_err(|_|"Artwork network unavailable")?;
    if response.status()!=200{return Err(format!("Artwork HTTP {}",response.status().as_u16()));}
    let mut body=response.into_body();let mut data=Vec::new();
    while let Some(frame)=body.frame().await{if let Ok(bytes)=frame.map_err(|_|"Artwork read failed")?.into_data(){
        if data.len()+bytes.len()>COVER_LIMIT{return Err("Artwork is too large".into());}data.extend_from_slice(&bytes);
    }}
    let picture=bytes::Bytes::from(data);
    if picture.len() > COVER_LIMIT {
        return Err(format!("{} bytes is too large for a cover", picture.len()));
    }
    if supported_picture_header(&picture){return Ok(picture);}
    // Spotify's CDN can send WebP despite the requested Accept types. Convert
    // once in the receiver so Kabuki and the app share a supported cache asset.
    if picture.starts_with(b"RIFF")&&picture.get(8..12)==Some(b"WEBP"){
        static CONVERSIONS:OnceLock<Arc<tokio::sync::Semaphore>>=OnceLock::new();
        let permit=CONVERSIONS.get_or_init(||Arc::new(tokio::sync::Semaphore::new(2))).clone().acquire_owned().await.map_err(|_|"Artwork conversion unavailable")?;
        let converted=tokio::task::spawn_blocking(move||{let _permit=permit;canonical_cover(&picture)}).await.map_err(|_|"Cover conversion failed".to_string())?;
        return converted;
    }
    log::warn!("cover format unsupported bytes={} kind={}",picture.len(),if picture.starts_with(b"<"){"html"}else if picture.starts_with(&[0x1f,0x8b]){"gzip"}else{"unknown"});
    Err("Artwork is temporarily unavailable".into())
}

/// Fetches a track's cover and saves it where the console can show it. Returns the file's
/// path, or nothing if there is no cover or it cannot be had in a few seconds.
async fn cover_file(session: &Session, track: &feed::Track) -> Option<String> {
    cover_file_in(session, track, COVER_DIR, COVERS_KEPT).await
}

async fn cover_file_in(session: &Session, track: &feed::Track, dir: &str, keep: usize) -> Option<String> {
    picture_file(session, &track.cover, dir, keep).await
}

/// Fetches the picture at `address` (Spotify's picture servers only) into `dir`, keeping the
/// newest `keep` there. Returns its path, or nothing if it cannot be had in a few seconds.
pub(crate) fn cached_picture_file(address: &str, dir: &str) -> Option<String> {
    if !cover_address_allowed(address) { return None; }
    let name = cover_name(address)?;
    // A cover already fetched for the queue or a notification also serves Library.
    for folder in [dir, COVER_DIR, QUEUE_COVER_DIR, "/user/data/spotifyps5/library-covers"] {
        let path = format!("{folder}/{name}");
        if cached_picture_supported(&path) {
            return Some(path);
        }
    }
    None
}

pub(crate) async fn picture_file(session: &Session, address: &str, dir: &str, keep: usize) -> Option<String> {
    if let Some(path) = cached_picture_file(address, dir) { return Some(path); }
    let path = format!("{dir}/{}", cover_name(address)?);
    match tokio::time::timeout(Duration::from_secs(6), fetch(session, address)).await {
        Ok(Ok(picture)) => {
            let _ = std::fs::create_dir_all(dir);
            // Written under another name first, so the console never reads half a file.
            let partial = format!("{path}.part");
            match std::fs::write(&partial, &picture).and_then(|()| std::fs::rename(&partial, &path)) {
                Ok(()) => {
                    tidy_covers(dir, keep);
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
    toast::note_sent(payload.len());
    let Ok(payload) = CString::new(payload) else { return };
    let code = notify(payload.as_ptr());
    if code != 0 {
        log::warn!("the console refused the notification: {code:#x}");
    }
}

/// Why this program paused Spotify, so that it knows when to start it again.
#[derive(Clone, Copy, PartialEq)]
enum PausedBy {
    /// Paused at the console: Spotify goes on when the stream is resumed there.
    Console,
    /// Nothing was playing "Librespot Streamer" (not started yet, other music, the stop
    /// file): Spotify goes on when it is started.
    NotRunning,
}

/// Shown when Spotify starts playing while nothing plays the stream file.
#[cfg(not(feature="usb-autostart"))]
const OPEN_FIRST: (&str, &str) = ("Play \"Librespot Streamer\" to listen", "Music > Start Librespot");
/// Shown at most this often.
#[cfg(not(feature="usb-autostart"))]
const OPEN_FIRST_EVERY: Duration = Duration::from_secs(3);

/// Shows a message about the stream itself.
fn send_status(_notify: host::NotifyFunction, _first: &str, _second: &str) {
    // Deliberately quiet: no receiver started/stopped/ready status notifications.
}
/// Shows the notification for a track that has just become audible, with its cover.
/// Nothing here can hold up the audio.
async fn show_toast(session: Session, track: feed::Track, notify: host::NotifyFunction) {
    let icon = cover_file(&session, &track).await;
    send_toast(notify, &track.tags.title, &track.tags.artist, icon.as_deref());
}

// ---------------------------------------------------------------------------------------
// The overlay panel's channel (see control.rs)
// ---------------------------------------------------------------------------------------

type Panel = Option<Arc<tokio::sync::watch::Sender<control::State>>>;

fn publish(panel: &Panel, change: impl FnOnce(&mut control::State)) {
    if let Some(panel) = panel {
        panel.send_modify(change);
    }
}

/// Shows a track that has just become audible (its position comes from the timeline), and
/// fetches its cover for the panel.
fn show_track(panel: &Panel, session: &Session, track: feed::Track) {
    publish(panel, |s| {
        s.title = track.tags.title.clone();
        s.artist = track.tags.artist.clone();
        s.album = track.tags.album.clone();
        s.duration_ms = (track.seconds * 1000.0).max(0.0) as u32;
        s.cover.clear();
    });
    if let Some(sender) = panel.clone() {
        let session = session.clone();
        tokio::spawn(async move {
            if let Some(path) = cover_file(&session, &track).await {
                // Only if the panel still shows this track.
                sender.send_if_modified(|s| {
                    let same = s.title == track.tags.title && s.cover != path;
                    if same {
                        s.cover = path;
                    }
                    same
                });
            }
        });
    }
}

/// Starts playing at once, at `track` (a URI) if given.
fn local_start(spirc:&Spirc,request:LoadRequest)->Result<(),librespot::core::Error>{
    #[cfg(feature="kabuki-provider")]
    if !kabuki::permit_play(){return Err(librespot::core::Error::failed_precondition("Kabuki is required"));}
    spirc.activate().and_then(|()|spirc.load(request))
}
fn play_options(track: Option<String>) -> librespot::connect::LoadRequestOptions {
    librespot::connect::LoadRequestOptions {
        start_playing: true,
        seek_to: 0,
        context_options: None,
        playing_track: track.map(librespot::connect::PlayingTrack::Uri),
    }
}

/// How many queued tracks the panel is told about.
const QUEUE_SHOWN: usize = 32;

/// Looks up title, artists and cover for each queued URI (remembered between calls) and gives
/// the panel the list, unless a newer queue has arrived meanwhile (`generation`).
async fn resolve_queue(
    session: Session,
    uris: Vec<String>,
    known: Arc<Mutex<std::collections::HashMap<String, control::QueueEntry>>>,
    generation: Arc<std::sync::atomic::AtomicU64>,
    mine: u64,
    panel: Panel,
) {
    let mut entries = Vec::with_capacity(uris.len());
    for (index, uri) in uris.into_iter().enumerate() {
        if generation.load(Ordering::Relaxed) != mine {
            return;
        }
        if let Some(mut entry) = known.lock().unwrap().get(&uri).cloned() {
            entry.index = index as u32;
            entries.push(entry);
            continue;
        }
        let Ok(id) = librespot::core::SpotifyUri::from_uri(&uri) else { continue };
        let item = match tokio::time::timeout(Duration::from_secs(5), AudioItem::get_file(&session, id)).await {
            Ok(Ok(item)) => item,
            Ok(Err(e)) => {
                log::warn!("queue: no details for {uri}: {e}");
                continue;
            }
            Err(_) => {
                log::warn!("queue: details for {uri} took too long");
                continue;
            }
        };
        let track = track_of(&item);
        let cover = cover_file_in(&session, &track, QUEUE_COVER_DIR, QUEUE_COVERS_KEPT).await.unwrap_or_default();
        let entry = control::QueueEntry { duration_ms:item.duration_ms,explicit:item.is_explicit,uri: uri.clone(), index: index as u32, title: track.tags.title.clone(), artist: track.tags.artist.clone(), cover };
        known.lock().unwrap().insert(uri, entry.clone());
        entries.push(entry);
    }
    if generation.load(Ordering::Relaxed) == mine {
        publish(&panel, |s| s.queue = Some(entries));
    }
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
    #[cfg(feature="shell-pcm")] let _=callbacks;
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
    #[cfg(not(feature="shell-pcm"))]
    let sink_callbacks = callbacks;
    // With a tape running the audio goes to it, not to the app.
    let tape = host::current();
    let sink_tape = tape.clone();
    let sink_events: Arc<Mutex<Option<PlayerEventChannel>>> = Arc::new(Mutex::new(None));
    let events_for_sink = sink_events.clone();
    let player = Player::new(player_config, session.clone(), mixer.get_soft_volume(), move || {
        match sink_tape {
            Some(tape) => Box::new(TapeSink { tape, events: events_for_sink, track: None }),
            #[cfg(feature="shell-pcm")]
            None => Box::new(pcm::Sink::default()),
            #[cfg(not(feature="shell-pcm"))]
            None => Box::new(AppSink {
                callbacks: sink_callbacks,
                started: false,
            }),
        }
    });
    // A pause or resume at the console is passed on to Spotify, so the phone agrees with
    // what is heard.
    // The overlay panel needs the player's events too, with or without a tape.
    let mut player_events: Option<PlayerEventChannel> = Some(player.get_player_event_channel());
    let mut notices = None;
    if let Some(tape) = &tape {
        *sink_events.lock().unwrap() = Some(player.get_player_event_channel());
        let (sender, receiver) = tokio::sync::mpsc::unbounded_channel();
        tape.on_notice(Box::new(move |notice| {
            let _ = sender.send(notice);
        }));
        notices = Some(receiver);
    }
    let mut playing = false;
    let mut paused_by_us: Option<PausedBy> = None;
    #[cfg(not(feature="usb-autostart"))]
    let mut told_to_open: Option<Instant> = None;
    let notify = tape.as_ref().and_then(|tape| tape.notify);

    // The overlay panel's channel. What it is told is what is heard: librespot's changes go
    // into a timeline that is heard `lead` later, and a new track is shown when the stream
    // says it has become audible.
    let lead = tape.as_ref().map_or(Duration::ZERO, |tape| tape.lead);
    // Install before opening UI ports: a bind failure must never allow playback.
    #[cfg(feature="kabuki-provider")]
    librespot::connect::on_playback_gate(kabuki::permit_play);
    let (panel, mut panel_commands, panel_lines): (Panel, _, _) = match control::listen(lead).await {
        Ok(control) => {
            #[cfg(feature="kabuki-provider")]
            {kabuki::start(control.state.clone(),control.lines.clone(),control.sender.clone());
             track_notices::start(control.state.clone());}
            log::info!("overlay panel channel on 127.0.0.1:{}", control::PORT);
            (Some(control.state), Some(control.commands), Some(control.lines))
        }
        Err(e) => {
            log::warn!("no channel for the overlay panel: {e}");
            (None, None, None)
        }
    };
    let streaming_now = || {
        #[cfg(feature="shell-pcm")] {return transport::state()==2;}
        #[cfg(not(feature="shell-pcm"))] {tape.as_ref().map_or(true, |tape| tape.feed.accepting())}
    };
    publish(&panel, |s| s.streaming = streaming_now());
    // Play or pause asked for by the panel while a track loads, sent once it has loaded:
    // librespot's player refuses both until then and can be left stuck ("Player::play
    // called from invalid state: Stopped", seen 2026-10-15).
    let mut deferred_play: Option<bool> = None;
    let mut app_search:Option<tokio::task::JoinHandle<()>>=None;
    tokio::spawn(async {if let Err(e)=media_api::serve_art().await{log::warn!("Art server unavailable: {e}");}});
    // The queue, as librespot-connect reports it each time it sends the connect state.
    let (queue_sender, mut queue_uris) = tokio::sync::mpsc::unbounded_channel::<(String, Vec<librespot::connect::NextTrack>)>();
    librespot::connect::on_next_tracks(move |context, uris| {
        let _ = queue_sender.send((context, uris));
    });
    let mut last_queue: Option<Vec<String>> = None;
    // The context playing (a playlist, an album…) and its whole queue, to play a queued track.
    let (recent_sender,mut recent_contexts)=tokio::sync::mpsc::unbounded_channel::<String>();
    librespot::connect::on_context_played(move |uri|{let _=recent_sender.send(uri.to_owned());});
    let mut current_context = String::new();
    let mut full_queue: Vec<librespot::connect::NextTrack> = Vec::new();
    let queue_known: Arc<Mutex<std::collections::HashMap<String, control::QueueEntry>>> = Arc::default();
    let queue_generation = Arc::new(std::sync::atomic::AtomicU64::new(0));

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

    #[cfg(feature="usb-autostart")]
    librespot::connect::on_playback_intent(transport::intent);
    #[cfg(feature="usb-autostart")]
    let mut last_transport_state = 0;
    let mut transport_tick = tokio::time::interval(Duration::from_millis(250));
    transport_tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);

    let mut spirc: Option<Spirc> = None;
    let mut spirc_task: Option<SpircTask> = None;
    let mut connecting = last_credentials.is_some();
    let mut connected_at: Option<Instant> = None;
    let mut retry = MIN_RETRY;
    let mut retry_at=tokio::time::Instant::now();
    publish(&panel,|s|{s.auth_status=if connecting{"connecting"}else{"unpaired"}.into();});

    loop {
        tokio::select! {
            _ = stop_signal().notified() => break,

            _ = transport_tick.tick(), if cfg!(feature="usb-autostart") => {
                #[cfg(feature="usb-autostart")]
                if let Some(tape) = &tape {
                    let state = transport::state();
                    if state != last_transport_state {
                        log::info!("USB transport state: {state}");
                        if state < 0 && transport::wanted() {
                            if let Some(notify) = notify {
                                send_status(notify, "Streamer could not start automatically", "Open Librespot Streamer, then press Play again");
                            }
                        }
                        last_transport_state = state;
                    }
                    let decision = transport::tick(tape.feed.accepting(), tape.native_prepared());
                    if decision.reset { tape.prepare_native_start(); }
                    if let Some(action) = decision.native { transport::request(action); }
                    if decision.resume && transport::wanted() {
                        if let Some(spirc) = &spirc {
                            log::info!("USB Streamer ready: resuming Spotify");
                            paused_by_us = None;
                            let _ = spirc.play();
                        }
                    }
                    publish(&panel, |s| s.streaming = streaming_now());
                }
                #[cfg(feature="shell-pcm")]
                if tape.is_none() {
                    let state=transport::state();
                    if pcm::pause_for_transition(last_transport_state,state,transport::wanted()) {
                        transport::intent(false);
                        if let Some(spirc)=&spirc {let _=spirc.pause();}
                    }
                    if pcm::resume_for_transition(last_transport_state,state,transport::wanted()) {
                        log::info!("PCM service resumed at the console: Spotify goes on");
                        transport::intent(true);
                        if let Some(spirc)=&spirc {paused_by_us=None;let _=spirc.play();}
                    }
                    if state!=last_transport_state {log::info!("PCM service state: {state}");last_transport_state=state;}
                    let decision=transport::tick(state==2,true);
                    if let Some(action)=decision.native {transport::request(action);}
                    if decision.resume&&transport::wanted() {
                        if let Some(spirc)=&spirc {paused_by_us=None;let _=spirc.play();}
                    }
                    publish(&panel,|s|s.streaming=state==2);
                }
            }


            event = async {
                match player_events.as_mut() {
                    Some(events) => events.recv().await,
                    None => std::future::pending().await,
                }
            } => {
                // The overlay panel's view of the event: what librespot plays now is heard
                // `lead` later.
                let now = Instant::now();
                let mut loaded = false;
                match &event {
                    Some(PlayerEvent::Loading { position_ms, .. }) => publish(&panel, |s| {
                        s.loading = true;
                        s.timeline.record(now, *position_ms, false, false);
                    }),
                    Some(PlayerEvent::Playing { position_ms, .. }) => {
                        loaded = true;
                        publish(&panel, |s| {
                            s.loading = false;
                            s.timeline.record(now, *position_ms, true, false);
                        })
                    }
                    Some(PlayerEvent::Paused { position_ms, .. }) => {
                        loaded = true;
                        publish(&panel, |s| {
                            s.loading = false;
                            s.timeline.record(now, *position_ms, false, false);
                        })
                    }
                    Some(PlayerEvent::Seeked { position_ms, .. }) => {
                        #[cfg(feature="shell-pcm")] pcm::flush();
                        publish(&panel, |s| {
                        let moving = s.timeline.moving();
                        s.timeline.record(now, *position_ms, moving, false);
                    })},
                    Some(
                        PlayerEvent::PositionCorrection { position_ms, .. }
                        | PlayerEvent::PositionChanged { position_ms, .. },
                    ) => publish(&panel, |s| {
                        let moving = s.timeline.moving();
                        s.timeline.record(now, *position_ms, moving, true);
                    }),
                    Some(PlayerEvent::Stopped { .. }) => publish(&panel, |s| {
                        s.loading = false;
                        let at = s.timeline.played(now);
                        s.timeline.record(now, at, false, false);
                    }),
                    Some(PlayerEvent::VolumeChanged { volume }) => {
                        publish(&panel, |s| s.volume = (*volume as u32 * 100 + 32767) / 65535)
                    }
                    Some(PlayerEvent::ShuffleChanged { shuffle }) => publish(&panel, |s| s.shuffle = *shuffle),
                    Some(PlayerEvent::RepeatChanged { context, track }) => publish(&panel, |s| {
                        s.repeat = match (*track, *context) {
                            (true, _) => control::Repeat::One,
                            (false, true) => control::Repeat::All,
                            (false, false) => control::Repeat::Off,
                        }
                    }),
                    // With a tape the track is shown when the stream says it is heard.
                    Some(PlayerEvent::TrackChanged { audio_item }) if tape.is_none() => {
                        publish(&panel,|s|{
                            s.media_generation=s.media_generation.wrapping_add(1);
                            if s.media_session==0{s.media_session=crate::native_library::millis();}
                            s.uri=audio_item.track_id.to_uri().unwrap_or_default();
                            s.explicit=audio_item.is_explicit;
                            s.cover_large.clear();
                            s.background.clear();
                        });
                        if let Some(sender)=panel.clone(){
                            let session=session.clone();let uri=audio_item.uri.clone();
                            let address=audio_item.covers.iter().min_by_key(|c|(c.width-640).abs()).map(|c|c.url.clone()).unwrap_or_default();
                            tokio::spawn(async move{
                                let large=picture_file(&session,&address,COVER_DIR,COVERS_KEPT).await.unwrap_or_default();
                                let details=client_api::track_info(&session,&uri).await.unwrap_or_default();
                                sender.send_if_modified(|s|{if s.uri!=uri{return false;}s.cover_large=large;
                                    s.album_uri=details["album_uri"].as_str().unwrap_or("").into();
                                    s.artists=details["artists"].as_array().cloned().unwrap_or_default();
                                    s.liked=details["liked"].as_bool().unwrap_or(false);true});
                                // The wide artist picture comes after, so it never delays the rest.
                                let artist=details["artists"][0]["uri"].as_str().unwrap_or("").to_owned();
                                let background=media_api::artist_background(&session,&uri,&artist).await;
                                sender.send_if_modified(|s|{if s.uri!=uri||s.background==background{return false;}s.background=background;true});
                            });
                        }
                        show_track(&panel, &session, track_of(audio_item));
                    }
                    _ => {}
                }
                publish(&panel, |s| s.streaming = streaming_now());
                // A play or pause from the panel that waited for the track to load.
                if loaded {
                    if let (Some(want), Some(spirc)) = (deferred_play.take(), spirc.as_ref()) {
                        let playing_now = matches!(event, Some(PlayerEvent::Playing { .. }));
                        if want != playing_now {
                            log::info!("overlay panel: {} now that the track has loaded", if want { "play" } else { "pause" });
                            let _ = if want { spirc.play() } else { spirc.pause() };
                        }
                    }
                }
                match event {
                    Some(PlayerEvent::Playing { .. }) => {
                        playing = true;
                        // Spotify is playing and nothing plays the stream file: what it
                        // plays would go nowhere. Pause it, and say what to do.
                        let streaming = tape.as_ref().map_or(true, |tape| tape.feed.accepting());
                        // The PCM writer already waits for the console Service, so Spotify
                        // is not paused while it resumes: that showed Play, Pause, Play.
                        #[cfg(feature="usb-autostart")]
                        if !cfg!(feature="shell-pcm") && transport::playing(streaming && transport::state()==2) {
                            if let Some(spirc) = &spirc {
                                if spirc.pause().is_ok() { paused_by_us = Some(PausedBy::NotRunning); }
                            }
                        }
                        #[cfg(not(feature="usb-autostart"))]
                        if let (false, Some(spirc)) = (streaming, spirc.as_ref()) {
                            log::info!("Spotify started playing while \"Librespot Streamer\" is not playing: paused");
                            if spirc.pause().is_ok() {
                                paused_by_us = Some(PausedBy::NotRunning);
                            }
                            if let Some(notify) = notify {
                                if told_to_open.map_or(true, |at| at.elapsed() >= OPEN_FIRST_EVERY) {
                                    told_to_open = Some(Instant::now());
                                    send_status(notify, OPEN_FIRST.0, OPEN_FIRST.1);
                                }
                            }
                        }
                    }
                    Some(PlayerEvent::Paused { .. } | PlayerEvent::Stopped { .. }) => {
                        playing = false;
                        #[cfg(feature="usb-autostart")]
                        transport::paused();
                    },
                    // The cover is fetched as soon as the track is known, a little before
                    // it is heard, so that its notification can go out complete at once.
                    Some(PlayerEvent::TrackChanged { audio_item }) => {
                        if notify.is_some() {
                            let session = session.clone();
                            let track = track_of(&audio_item);
                            tokio::spawn(async move {
                                let _ = cover_file(&session, &track).await;
                            });
                        }
                    }
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
                // The overlay panel switches track when the new one is heard.
                match &notice {
                    Some(stream::Notice::Track(track)) => show_track(&panel, &session, track.clone()),
                    Some(stream::Notice::Restarted(Some(track)))
                        if panel.as_ref().is_some_and(|p| p.borrow().title != track.tags.title) =>
                    {
                        show_track(&panel, &session, track.clone())
                    }
                    _ => {}
                }
                publish(&panel, |s| s.streaming = streaming_now());
                match (notice, spirc.as_ref()) {
                    (Some(stream::Notice::Paused), spirc) => {
                        #[cfg(feature="usb-autostart")]
                        transport::intent(false);
                        if let (true, Some(spirc)) = (playing, spirc) {
                            if spirc.pause().is_ok() {
                                paused_by_us = Some(PausedBy::Console);
                            }
                        }
                        if let Some(notify) = notify {
                            send_status(notify, "librespot-ps5 stopped", "Paused at the console");
                        }
                    }
                    (Some(stream::Notice::Resumed), Some(spirc)) if paused_by_us == Some(PausedBy::Console) => {
                        paused_by_us = None;
                        #[cfg(feature="usb-autostart")]
                        transport::intent(true);
                        let _ = spirc.play();
                    }
                    (Some(stream::Notice::Stop), spirc) => {
                        #[cfg(feature="usb-autostart")]
                        transport::intent(false);
                        // The stop file was played at the console: stopped on purpose, so
                        // Spotify does not start again by itself.
                        paused_by_us = None;
                        if let (true, Some(spirc)) = (playing, spirc) {
                            let _ = spirc.pause();
                        }
                        if let Some(notify) = notify {
                            send_status(notify, "librespot-ps5 stopped", "\"Stop Librespot\" was played");
                        }
                    }
                    (Some(stream::Notice::Away), spirc) => {
                        #[cfg(feature="usb-autostart")]
                        transport::intent(false);
                        // Other music was started at the console. Spotify waits, and goes
                        // on when "Librespot Streamer" is played again.
                        if let (true, Some(spirc)) = (playing, spirc) {
                            if spirc.pause().is_ok() {
                                paused_by_us = Some(PausedBy::NotRunning);
                            }
                        } else if paused_by_us.is_some() {
                            paused_by_us = Some(PausedBy::NotRunning);
                        }
                        if let Some(notify) = notify {
                            send_status(notify, "librespot-ps5 stopped", "Other music is playing");
                        }
                    }
                    (Some(stream::Notice::Restarted(track)), spirc) => {
                        // The player has started or resumed the stream file (the stream
                        // waits until the console's own notice says it is ours). If this
                        // program paused Spotify, it goes on now: after a resume the tape
                        // holds it back until the silence written for the player has been
                        // played, but the phone shows it playing straight away.
                        #[cfg(feature="usb-autostart")]
                        let may_resume = transport::wanted() && !transport::holding();
                        #[cfg(not(feature="usb-autostart"))]
                        let may_resume = true;
                        if let (true, Some(_), Some(spirc)) = (may_resume, paused_by_us, spirc) {
                            log::info!("\"Librespot Streamer\" is playing: Spotify goes on");
                            paused_by_us = None;
                            let _ = spirc.play();
                        }
                        if let Some(notify) = notify {
                            tokio::spawn(async move {
                                tokio::time::sleep(Duration::from_millis(400)).await;
                                match track {
                                    Some(track) => {
                                        let line = format!("{} - {}", track.tags.title, track.tags.artist);
                                        send_status(notify, "librespot-ps5 started", &line)
                                    }
                                    None => send_status(notify, "librespot-ps5 ready!", "by Jack (stupiditytries)"),
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

            recent = recent_contexts.recv() => {
                if let Some(uri)=recent {
                    library::record_context_played(&session,&uri,panel_lines.as_ref(),panel.as_ref().map_or(0,|p|p.borrow().media_session));
                }
            }

            reported = queue_uris.recv() => {
                if let Some((context, uris)) = reported {
                    if current_context != context {
                        current_context = context;
                        publish(&panel,|s|s.context=serde_json::json!({"uri":current_context,"kind":if current_context.starts_with("spotify:album:"){"album"}else if current_context.starts_with("spotify:artist:"){"artist"}else if current_context.ends_with(":collection"){"collection"}else{"playlist"},"name":""}));
                        let mine=current_context.clone();let panel=panel.clone();let session=session.clone();
                        tokio::spawn(async move {
                            if let Ok(Ok(info))=tokio::time::timeout(Duration::from_secs(8),client_api::list_info(&session,&mine)).await {
                                publish(&panel,|s|{if s.context["uri"].as_str()==Some(mine.as_str()){s.context["name"]=info["name"].clone();}});
                            }
                        });
                    }
                    full_queue = uris.clone();
                    let shown: Vec<String> = uris.into_iter().take(QUEUE_SHOWN).map(|track| track.uri).collect();
                    if last_queue.as_ref() != Some(&shown) {
                        last_queue = Some(shown.clone());
                        let mine = queue_generation.fetch_add(1, Ordering::Relaxed) + 1;
                        tokio::spawn(resolve_queue(
                            session.clone(),
                            shown,
                            queue_known.clone(),
                            queue_generation.clone(),
                            mine,
                            panel.clone(),
                        ));
                    }
                }
            }

            command = async {
                match panel_commands.as_mut() {
                    Some(commands) => commands.recv().await,
                    None => std::future::pending().await,
                }
            } => {
                match (command, spirc.as_ref()) {
                    (Some(command), Some(spirc)) => {
                        log::info!("overlay panel: {command:?}");
                        let result = match command {
                            // An explicit play or pause, from what librespot is doing, not a
                            // toggle; held until a loading track is ready.
                            control::Command::MediaFeature{feature,uri,client,id,session:epoch,generation}=>{
                                if let (Some(panel),Some(lines))=(&panel,&panel_lines){
                                    let state=panel.borrow();
                                    if state.uri==uri&&state.media_session==epoch&&state.media_generation==generation {
                                        tokio::spawn(media_features::request(session.clone(),feature,uri,client,id,epoch,generation,panel.clone(),lines.clone()));
                                    }
                                }Ok(())
                            },
                            control::Command::Pause=>spirc.pause(),
                            control::Command::PlayList{context}=>local_start(spirc,LoadRequest::from_context_uri(context,play_options(None))),
                            control::Command::PlayTrack{track}=>local_start(spirc,LoadRequest::from_tracks(vec![track],play_options(None))),
                            control::Command::Search{query,id}=>{if let Some(lines)=&panel_lines {if let Some(old)=app_search.take(){old.abort();}app_search=Some(tokio::spawn(media_api::search(session.clone(),query,id,lines.clone())));}Ok(())},
                            control::Command::Save{uri,on}=>{if let Some(lines)=&panel_lines {tokio::spawn(media_api::save(session.clone(),uri,on,lines.clone(),panel.clone()));}Ok(())},
                            control::Command::PlayPause => {
                                let (loading, playing_now) = panel
                                    .as_ref()
                                    .map_or((false, playing), |p| (p.borrow().loading, p.borrow().timeline.moving()));
                                let want = !deferred_play.unwrap_or(playing_now);
                                #[cfg(feature="usb-autostart")]
                                transport::intent(want);
                                if loading {
                                    log::info!("overlay panel: {} once the track has loaded", if want { "play" } else { "pause" });
                                    deferred_play = Some(want);
                                    Ok(())
                                } else if want {
                                    spirc.play()
                                } else {
                                    spirc.pause()
                                }
                            }
                            control::Command::Previous => spirc.prev(),
                            control::Command::Next => spirc.next(),
                            // Heard there one lead later; the panel holds until then.
                            control::Command::Seek(ms) => spirc.set_position_ms(ms),
                            control::Command::Volume(volume) => {
                                spirc.set_volume((volume.min(100) * u16::MAX as u32 / 100) as u16)
                            }
                            control::Command::Shuffle(on) => spirc.shuffle(on),
                            control::Command::Repeat(mode) => spirc
                                .repeat(mode == control::Repeat::All)
                                .and_then(|()| spirc.repeat_track(mode == control::Repeat::One)),
                            // A queued track: within the context it came from, so what follows
                            // it still does; without one, the queue from there on.
                            control::Command::PlayQueued { uri, index } => {
                                deferred_play = None;
                                // Metadata failures can hide rows; the UI sends the original queue index.
                                let offset = if full_queue.get(index as usize).is_some_and(|t| t.uri == uri) {
                                    Some(index as usize)
                                } else {
                                    full_queue.iter().position(|t| t.uri == uri)
                                };
                                let selected = offset.and_then(|i| full_queue.get(i));
                                let request = if current_context.is_empty() || selected.map_or(true, |t| t.queued) {
                                    let offset = offset.unwrap_or(full_queue.len());
                                    let rest: Vec<String> = full_queue.iter().skip(offset).map(|t| t.uri.clone()).collect();
                                    let rest = if rest.first() == Some(&uri) { rest } else { vec![uri] };
                                    LoadRequest::from_tracks(rest, play_options(None))
                                } else {
                                    LoadRequest::from_context_uri(current_context.clone(), play_options(Some(uri)))
                                };
                                local_start(spirc,request)
                            }
                            control::Command::Play { context, track } => {
                                deferred_play = None;
                                local_start(spirc,LoadRequest::from_context_uri(context, play_options(Some(track))))
                            }
                            control::Command::Library => {
                                if let Some(lines) = &panel_lines {
                                    tokio::spawn(library::send_library(session.clone(), lines.clone(),panel.as_ref().map_or(0,|p|p.borrow().media_session),false));
                                }
                                Ok(())
                            }
                            control::Command::OpenListPage { uri,offset,client,id } => {
                                if let Some(lines)=&panel_lines {tokio::spawn(library::send_tracks_page(session.clone(),uri,offset,client,id,lines.clone(),panel.as_ref().map_or(0,|p|p.borrow().media_session)));}
                                Ok(())
                            }
                            control::Command::OpenList { uri } => {
                                if let Some(lines) = &panel_lines {
                                    tokio::spawn(library::send_tracks(session.clone(), uri, lines.clone(),panel.as_ref().map_or(0,|p|p.borrow().media_session)));
                                }
                                Ok(())
                            }
                        };
                        if let Err(e) = result {
                            log::warn!("overlay panel command failed: {e}");
                        }
                    }
                    (Some(command), None) => log::info!("overlay panel: {command:?} ignored; no Spotify app is connected"),
                    (None, _) => panel_commands = None,
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
                        publish(&panel,|s|{s.media_session=crate::native_library::millis();s.media_generation=0;});
                        last_credentials = Some(credentials);
                        retry = MIN_RETRY;retry_at=tokio::time::Instant::now();
                        shut_down_connection(&mut spirc, &mut spirc_task, &session);
                        publish(&panel, |s| s.connected = false);
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

            _ = tokio::time::sleep_until(retry_at), if connecting && last_credentials.is_some() => {
                // A failed constructor may leave a partial session even without is_invalid().
                session.shutdown();session=Session::new(session_config.clone(),cache.clone());player.set_session(session.clone());
                publish(&panel,|s|{s.auth_status="connecting".into();s.auth_retry_seconds=0;});
                let credentials=last_credentials.clone().unwrap_or_default();
                let attempt=tokio::select!{
                    _=stop_signal().notified()=>break,
                    result=tokio::time::timeout(Duration::from_secs(30),Spirc::new(connect_config.clone(),session.clone(),credentials,player.clone(),mixer.clone()))=>result.unwrap_or_else(|_|Err(librespot::core::Error::deadline_exceeded("Spotify login timed out")))
                };
                match attempt
                {
                    Ok((new_spirc, task)) => {
                        log::info!("connected to Spotify");
                        last_credentials=session.reusable_credentials().or(last_credentials);
                        publish(&panel, |s| {s.connected=true;s.auth_status="ready".into();s.auth_retry_seconds=0;
                            s.media_session=crate::native_library::millis().max(s.media_session.saturating_add(1));s.media_generation=0;});
                        if let Some(lines)=&panel_lines{
                            tokio::spawn(library::send_library(session.clone(),lines.clone(),panel.as_ref().map_or(0,|p|p.borrow().media_session),true));
                        }
                        spirc = Some(new_spirc);
                        spirc_task = Some(Box::pin(task));
                        connected_at = Some(Instant::now());
                        connecting = false;
                    }
                    Err(e) => {
                        let pair=login::needs_pairing(&e);
                        log::warn!("Spotify connection failed kind={:?} pairing_required={pair}",e.kind);
                        session.shutdown();
                        if pair{
                            connecting=false;publish(&panel,|s|{s.auth_status="reauth_required".into();s.connected=false;});
                        }else{
                            // Timer in select leaves discovery and stop processing available.
                            let jitter=Duration::from_millis((crate::native_library::millis()%501)as u64);
                            retry_at=tokio::time::Instant::now()+retry+jitter;
                            publish(&panel,|s|{s.auth_status="retrying".into();s.auth_retry_seconds=retry.as_secs();s.connected=false;});
                            retry=(retry*2).min(MAX_RETRY);
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
                #[cfg(feature="usb-autostart")]
                transport::intent(false);
                publish(&panel, |s| {
                    s.connected = false;
                    s.loading = false;
                    let at = s.timeline.played(Instant::now());
                    s.timeline.record(Instant::now(), at, false, false);
                });
                deferred_play = None;
                if connected_at.take().map_or(false, |at| at.elapsed() >= HEALTHY) {
                    retry = MIN_RETRY;
                }
                if last_credentials.is_some() {
                    log::warn!("the Spotify connection ended; reconnecting in {retry:?}");
                    retry_at=tokio::time::Instant::now()+retry;
                    publish(&panel,|s|{s.auth_status="retrying".into();s.auth_retry_seconds=retry.as_secs();});
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
    use super::{cached_picture_file, cached_picture_supported, cover_address_allowed, cover_name};
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
        let common="a".repeat(80);
        assert_ne!(cover_name(&format!("https://mosaic.scdn.co/640/{common}1")),cover_name(&format!("https://mosaic.scdn.co/640/{common}2")));
        assert_eq!(cover_name("https://i.scdn.co/image/abc?size=300"),cover_name("https://i.scdn.co/image/abc"));
        assert!(cover_name(crate::native_library::LIKED_COVER).is_some());
        assert!(cover_name("https://image-cdn-ak.spotifycdn.com/image/mix.jpg").is_some());
        assert_ne!(cover_name("https://dailymix-images.scdn.co/v2/img/aaa/1/en/default"),
                   cover_name("https://dailymix-images.scdn.co/v2/img/bbb/2/en/default"));
        // Nothing that could name a file somewhere else.
        for address in ["", "https://i.scdn.co/image/", "https://i.scdn.co/image/..", "https://i.scdn.co/image/a.b"] {
            assert_eq!(cover_name(address), None, "{address}");
        }
    }

    #[test]
    fn existing_covers_are_available_before_any_network_request() {
        let dir=std::env::temp_dir().join(format!("cover-hit-{}",std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let folder=dir.to_str().unwrap();
        let path=dir.join("cachetest.jpg");
        std::fs::write(&path,[0xff,0xd8,0xff,0xd9]).unwrap();
        assert_eq!(cached_picture_file("https://i.scdn.co/image/cachetest",folder),Some(path.to_str().unwrap().to_owned()));
        assert!(cached_picture_file("https://example.com/image/cachetest",folder).is_none());
        for invalid in [&b"RIFFxxxxWEBPdata"[..], &b"<html>error</html>"[..], &[0x89, b'P', b'N', b'G'][..], &[][..]] {
            std::fs::write(&path, invalid).unwrap();
            assert!(cached_picture_file("https://i.scdn.co/image/cachetest",folder).is_none());
        }
        std::fs::write(&path, [0x89, b'P', b'N', b'G', 0x0d, 0x0a, 0x1a, 0x0a]).unwrap();
        assert!(cached_picture_file("https://i.scdn.co/image/cachetest",folder).is_some());
        #[cfg(unix)] {
            let link = dir.join("linked.jpg");
            std::os::unix::fs::symlink(&path, &link).unwrap();
            assert!(!cached_picture_supported(link.to_str().unwrap()));
        }
        std::fs::remove_dir_all(dir).unwrap();
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
