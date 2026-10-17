//! Playback through the console's own USB music player, as one long track.
//!
//! The player is started by hand on a single file, "Librespot Streamer", alone in its
//! folder. It reads the file as it plays it, a few seconds ahead of what is heard, so this
//! module writes audio into the file just ahead of where the player is reading, like a tape
//! recorded a little in front of the playback head. Every track goes into the same file one
//! after the other, with no break; which track is playing is reported to the caller at the
//! moment it becomes audible, so that it can be shown some other way.
//!
//! A second file in another folder, "Stop Librespot", is a second of silence. Playing it
//! is how the person at the console asks for the stream to stop.
//!
//! The console's log says when its player starts and ends something, but not what. What
//! this module concludes from those lines is therefore partly assumption:
//! - a start while nothing of ours plays is the stream being started;
//! - an end about a second after a start is the stop file having been played;
//! - a start in the middle of the stream is the stream file being started again from its
//!   beginning (previous or next at the console, in a folder with one entry).
//!
//! What does tell the files apart is the notice the console puts up when its player starts
//! a track: the log gives its size, which depends on the track, so the stream file's is
//! known (`Config::stream_notice`). Any other size right after a start means other music:
//! the stream stops and the file is made ready to be started from its beginning again. The
//! console does not always put up a notice on a resume; with none, the start is taken to
//! be ours, as before.
//!
//! Nothing here touches the disk or the clock directly: the files are behind [`Store`] and
//! the time is passed in, so it runs against a pretend player in the tests.

use std::collections::VecDeque;
use std::io;
use std::sync::Arc;

use crate::flac::{self, block, Tags, AUDIO_START, RATE};
use crate::feed::{Feed, Popped, Signal, Store, Track};

/// The stream file and the stop file, as numbered in the [`Store`].
pub const STREAM: usize = 0;
pub const STOP: usize = 1;

pub const STREAM_TITLE: &str = "Librespot Streamer";
pub const STOP_TITLE: &str = "Stop Librespot";

#[derive(Clone, Debug)]
pub struct Config {
    /// Seconds of audio kept written ahead of what is being heard. With a file that is
    /// rewritten in place (`grow` off) it must stay above the player's read-ahead, about
    /// 9 s, by more than `slack`.
    pub lead: f64,
    /// How far below `lead` the tape may fall while it waits for audio before it writes
    /// silence instead.
    pub slack: f64,
    /// Seconds of silence at the start of the file. It is what the player meets whenever
    /// it starts the file, so it must be longer than the player's read-ahead.
    pub prefill: f64,
    /// Seconds without audio after which the stream stops, so an idle console does not
    /// write to the drive for ever.
    pub idle_limit: f64,
    /// Length of the stream file in hours. The player takes a file's size when it opens
    /// it, so this is the longest the stream can run before it has to be started again.
    pub hours: f64,
    /// Whether the file states its length. Without it the console's player crashed when
    /// it opened the file a second time (2026-10-11), so this stays on.
    pub show_length: bool,
    /// Size of the console's own notice for the stream file, as its log gives it (813 on
    /// 2026-10-11 and 2026-10-14, with a 12-hour file). See the top of the file.
    pub stream_notice: u32,
    /// Once the player has opened the file, cut the file off just after what has been
    /// written and let it grow as audio arrives. The player took the file's full
    /// size when it opened it, so it should not take the end of the file for the end of
    /// the track; if it waits there for more, it can never read further ahead than what
    /// exists, and `lead` can be a second or two instead of more than its nine.
    pub grow: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            lead: 2.0,
            slack: 0.3,
            prefill: 14.0,
            idle_limit: 600.0,
            hours: 12.0,
            show_length: true,
            stream_notice: 813,
            grow: true,
        }
    }
}

#[derive(Clone, Debug)]
pub enum Notice {
    /// Paused at the console.
    Paused,
    /// Resumed at the console after a pause that was reported.
    Resumed,
    /// This track has just become audible.
    Track(Track),
    /// The stop file was played.
    Stop,
    /// The player has just started the stream file, or gone on with it after a pause. The
    /// console shows a notice of its own for the file at that moment ("Librespot
    /// Streamer"); this is the cue to put ours in its place: the track that is on, if
    /// there is one. Sent once the console's notice has been seen, or a moment after the
    /// start if it does not come.
    Restarted(Option<Track>),
    /// Other music was started in the console's player: the stream has stopped, and the
    /// file is ready to be started again.
    Away,
}

#[derive(Clone, Copy, Debug, PartialEq)]
enum Player {
    /// The file is ready; nobody has pressed play.
    Waiting,
    Playing { t0: f64 },
    /// Paused at the console, `at` sample pairs into the file. `told` once the pause has
    /// lasted long enough to be passed on.
    Paused { at: u64, since: f64, told: bool },
    /// With a growing file, nothing has arrived for a long time and nothing more is being
    /// written: the player has played all there is and waits at the end of the file. When
    /// audio comes again the tape and the player are lined up (`Syncing`).
    Starved,
    /// Resumed at the console with a growing file: waiting until the player has played
    /// everything that was written and stands at the end of the file. Then a lead's worth
    /// of silence is written in one go and the tape and the player are in step again.
    Syncing { until: f64 },
    /// The stream has stopped. Once the log has been quiet for a while the file is made
    /// ready again.
    Draining { quiet_since: f64 },
}

/// An end followed by a start within this time is not a pause. Near the beginning of the
/// file it is the player's own start-up; later it is the file being started again.
const PAUSE_SETTLE: f64 = 1.5;
/// A second start reported this soon after the first is the same start; the later one is
/// nearer to when sound begins.
const START_SETTLE: f64 = 1.0;
/// The stop file's length in seconds, and how long after a start an end counts as it.
const STOP_SECONDS: f64 = 1.2;
// Kept narrow: a pause at the console two seconds after a resume was once taken for it.
const STOP_WINDOW: (f64, f64) = (0.95, 1.6);
/// The stop file goes round with less than this between one play and the next.
const STOP_GAP: f64 = 1.5;
/// Added to the time the player needs to play out what is written, when lining up again.
const SYNC_MARGIN: f64 = 1.0;
/// With a growing file, audio is replaced by silence once it is this many seconds behind
/// what is heard, so the file never holds more than a few seconds of it. Silence, not
/// zeros: when the player resumes after a pause it looks for its place in the file, and
/// where it met a long run of zeros it crashed (2026-10-11: `SystemMusicCore`, an
/// allocation of 25 MB failing, the length of the run). Every byte from the start of the
/// file to the end of what has been written must be a valid frame.
const KEEP_BEHIND: f64 = 5.0;
/// Frames replaced by silence in one call at most.
const SILENCE_BEHIND_STEP: u32 = 48;
/// Silence in the log after which a stopped stream is made ready again.
const QUIET: f64 = 6.0;
/// The file holds silence for this many seconds past the write head; the rest of it may
/// hold whatever was on the drive, or audio from hours ago. Silence, not zeros: if the
/// player ever reads further ahead than the audio has been written, it then plays a moment
/// of silence, where zeros would make it end the file.
const SILENCE_AHEAD: f64 = 30.0;
/// Silence written in one call at most, in seconds.
const SILENCE_STEP: f64 = 6.0;
/// Old audio is wiped this many bytes at a time: more while nothing plays, less while the
/// stream is running, so that it never holds up the audio.
const WIPE_STEP_IDLE: u64 = 8 << 20;
const WIPE_STEP_PLAYING: u64 = 256 << 10;
/// Where in the file (inside the header's padding) the extent of old audio is noted, so
/// that a later run of the program knows how much there is to wipe.
const MARK_AT: u64 = AUDIO_START - 12;
const MARK: &[u8; 4] = b"LSDE";
/// The note is rewritten when the extent has moved this many bytes.
const MARK_STEP: u64 = 4 << 20;
/// Frames written in one call at most.
const FRAMES_PER_TICK: usize = 400;
/// The console's notice for a track follows the start by well under a second; one later
/// than this is not taken to be about that start.
const IDENTIFY_WINDOW: f64 = 3.0;
/// How long [`Notice::Restarted`] waits for the console's notice before it goes anyway.
const HOLD_LIMIT: f64 = 2.5;
/// Notices of the console's this size and up are for a track its player started; smaller
/// ones are other things (and ours are left out before they get here).
const TRACK_NOTICE_MIN: u32 = 600;
/// After other music has been seen to start, the stream stops this much later, so that
/// the stop file (which also counts as other music) is recognised first.
const AWAY_DELAY: f64 = 4.5;
/// The shortest stream file worth having, in hours.
const SHORTEST_HOURS: f64 = 0.5;

fn frames_in(seconds: f64) -> u32 {
    (seconds * RATE as f64 / block() as f64).ceil() as u32
}

/// The stop file: its title and a little silence.
fn stop_file() -> Vec<u8> {
    let frames = frames_in(STOP_SECONDS);
    let tags = Tags { title: STOP_TITLE.to_string(), ..Tags::default() };
    let mut file = flac::header(frames as u64 * block() as u64, &tags);
    let silence = vec![0i16; block() * 2];
    for index in 0..frames {
        flac::push_frame(&mut file, index, &silence);
    }
    file
}

pub struct Stream {
    config: Config,
    store: Arc<dyn Store>,
    feed: Arc<Feed>,
    player: Player,
    /// Frames the file can hold, and frames of silence at its start.
    capacity: u32,
    prefill: u32,
    /// Frames written. Every frame is full, so this is also the position in samples.
    write_frames: u32,
    /// The frames from the write head up to this one hold silence.
    silent_to: u32,
    /// Offset up to which the file may hold audio (or anything else that is not zeros).
    /// Whatever of it lies past the silence is old and is wiped, from the top down.
    dirty_end: u64,
    /// The value of `dirty_end` last noted in the file.
    marked: u64,
    /// The file has been cut off at the write head and is growing (see `Config::grow`).
    grown: bool,
    /// With a growing file: the frame up to which played audio has been replaced by
    /// silence.
    wiped_to: u32,
    block: Vec<i16>,
    block_has_audio: bool,
    frame: Vec<u8>,
    last_audio: f64,
    /// No more is written until the file is made ready again.
    halted: bool,
    full_logged: bool,
    /// Tracks written and not yet audible, with the sample pair each begins at.
    pending: VecDeque<(u64, Track)>,
    /// The track the audio being written belongs to.
    current: Option<Track>,
    /// A track that has been announced and whose audio has not arrived yet.
    announced: Option<Track>,
    /// Where in the file the last track to be reported begins.
    announced_at: u64,
    last_start: Option<f64>,
    /// Short plays in a row, and when the last one ended (see `stop_file_played`).
    short_plays: u32,
    last_short_end: Option<f64>,
    /// Spotify has been told of a pause at the console and not yet of the resume.
    told_spotify: bool,
    /// The tape is to be lined up with the player at the next resume.
    resync: bool,
    /// A notice kept back until the console has said what its player started.
    held: Option<(f64, Notice)>,
    /// Until when the console's next notice is taken to be about the last start.
    identify_until: Option<f64>,
    /// Other music has started; the stream stops at this time.
    away_at: Option<f64>,
    /// The stream file has been seen playing since it was last made ready. Until then a
    /// start may have been other music all along, and stopping for it is not news.
    confirmed: bool,
    /// With a growing file: nothing has arrived for the idle time, so silence is no longer
    /// written to keep ahead of the player.
    starved: bool,
}

impl Stream {
    /// Sets up both files and gets the stream ready to be started. The player must not be
    /// playing either file when this is called.
    pub fn new(config: Config, store: Arc<dyn Store>, feed: Arc<Feed>) -> io::Result<Stream> {
        // The file is as long as asked for if the drive allows it, and otherwise the
        // longest it does allow (a FAT32 drive stops at 4 GB, a full one sooner).
        let mut hours = config.hours;
        let length = loop {
            let wanted = flac::frame_offset(frames_in(hours * 3600.0));
            let current = store.len(STREAM)?;
            if current == wanted {
                break current;
            }
            match store.set_len(STREAM, wanted) {
                Ok(()) => break wanted,
                Err(e) if hours / 2.0 >= SHORTEST_HOURS => {
                    log::warn!("a stream file of {hours} hours is not possible here ({e}); trying half");
                    hours /= 2.0;
                }
                Err(e) => return Err(e),
            }
        };
        let capacity = frames_in(hours * 3600.0);
        debug_assert_eq!(flac::frame_offset(capacity), length);
        let mut stream = Stream {
            prefill: frames_in(config.prefill).min(capacity),
            config,
            store,
            feed,
            player: Player::Waiting,
            capacity,
            write_frames: 0,
            silent_to: 0,
            dirty_end: 0,
            marked: u64::MAX,
            grown: false,
            wiped_to: 0,
            block: Vec::with_capacity(block() * 2),
            block_has_audio: false,
            frame: Vec::new(),
            last_audio: 0.0,
            halted: false,
            full_logged: false,
            pending: VecDeque::new(),
            current: None,
            announced: None,
            announced_at: 0,
            last_start: None,
            short_plays: 0,
            last_short_end: None,
            told_spotify: false,
            resync: false,
            held: None,
            identify_until: None,
            away_at: None,
            confirmed: false,
            starved: false,
        };
        log::info!("stream file: {:.1} hours, {} MB", hours, length >> 20);
        // How much of the file an earlier run left audio in. With no note (a new file, or
        // one from before the note existed) all of it is taken to need wiping.
        let mut note = [0u8; 12];
        let read = stream.store.read_at(STREAM, MARK_AT, &mut note)?;
        let noted = u64::from_le_bytes(note[4..].try_into().unwrap());
        stream.dirty_end = if read == 12 && &note[..4] == MARK && noted <= length { noted } else { length };
        if !stream.config.grow && stream.dirty_end > flac::frame_offset(stream.prefill) {
            log::info!("{} MB of the file may hold old audio: wiping it", stream.dirty_end >> 20);
        }
        stream.arm()?;
        Ok(stream)
    }

    /// Writes the silence at the start of the stream file and puts the stop file back.
    fn arm(&mut self) -> io::Result<()> {
        if self.grown {
            // Back to its full size, which is what the player must find when it opens it.
            self.store.set_len(STREAM, flac::frame_offset(self.capacity))?;
            self.grown = false;
        }
        let total = if self.config.show_length { self.capacity as u64 * block() as u64 } else { 0 };
        let tags = Tags { title: STREAM_TITLE.to_string(), ..Tags::default() };
        let mut start = flac::header(total, &tags);
        let silence = vec![0i16; block() * 2];
        for index in 0..self.prefill {
            flac::push_frame(&mut start, index, &silence);
        }
        self.store.write_at(STREAM, 0, &start)?;
        self.marked = u64::MAX;
        let stop = stop_file();
        self.store.write_at(STOP, 0, &stop)?;
        self.store.set_len(STOP, stop.len() as u64)?;

        // The queue announces the last track again, if there is one.
        self.current = None;
        self.announced = None;
        self.feed.reset();
        self.halted = false;
        self.starved = false;
        self.wiped_to = 0;
        self.resync = false;
        self.told_spotify = false;
        self.held = None;
        self.away_at = None;
        self.confirmed = false;
        self.rewind()?;
        self.full_logged = false;
        self.player = Player::Waiting;
        log::info!("ready: play \"{STREAM_TITLE}\" in the console's music player");
        Ok(())
    }

    /// Puts the write head back to just after the silence at the start of the file, where
    /// the player is whenever it has just started the file.
    fn rewind(&mut self) -> io::Result<()> {
        self.write_frames = self.prefill;
        self.block.clear();
        self.block_has_audio = false;
        // What follows the opening silence may be old audio: more silence goes over it
        // before the player, which is at the start of the file, reads that far.
        self.silent_to = self.prefill;
        self.silence_ahead()?;
        // Tracks that were written and not yet heard are gone; the one being received
        // carries on from here.
        self.pending.clear();
        self.announced_at = 0;
        if let Some(track) = &self.current {
            self.pending.push_back((self.written(), track.clone()));
        }
        Ok(())
    }

    /// Sample pairs written, counted from the start of the file.
    fn written(&self) -> u64 {
        self.write_frames as u64 * block() as u64
    }

    /// Sample pairs of the file that have been heard.
    fn heard(&self, now: f64) -> u64 {
        match self.player {
            Player::Waiting => 0,
            Player::Playing { t0 } => (((now - t0).max(0.0) * RATE as f64) as u64).min(self.written()),
            Player::Paused { at, .. } => at,
            Player::Starved | Player::Syncing { .. } | Player::Draining { .. } => self.written(),
        }
    }

    /// Stops the stream until the file is made ready again. The silence at the start of
    /// the file is removed: the player goes back to the start of a folder when it reaches
    /// the end, and with nothing playable there it stops instead of going round for ever.
    fn halt(&mut self) {
        if self.halted {
            return;
        }
        self.halted = true;
        // Zeros after the last audio, so the player ends the file there.
        let from = flac::frame_offset(self.write_frames);
        let mut after = vec![0u8; (flac::frame_offset(self.silent_to.max(self.write_frames)) - from) as usize];
        if self.config.grow {
            // The file stops at the write head, where the player would wait for ever: zeros
            // go after it. What is left of the audio becomes silence; the player has
            // already read it.
            after = vec![0u8; 1 << 18];
            if let Err(e) = self.silence_behind(self.write_frames) {
                log::error!("could not clear the stream file: {e}");
            }
        }
        let start = vec![0u8; (flac::frame_offset(self.prefill) - AUDIO_START) as usize];
        let cleared = self
            .store
            .write_at(STREAM, from, &after)
            .and_then(|()| self.store.write_at(STREAM, AUDIO_START, &start));
        if let Err(e) = cleared {
            log::error!("could not clear the stream file: {e}");
        }
        self.silent_to = self.write_frames;
    }

    /// Whether an end that has just been reported makes the second short play in a row:
    /// the stop file going round. One short play alone is not enough, since a pause soon
    /// after a resume looks the same (and was twice taken for it on 2026-10-11).
    fn stop_file_played(&mut self, now: f64) -> bool {
        let Some(start) = self.last_start else { return false };
        if !(STOP_WINDOW.0..=STOP_WINDOW.1).contains(&(now - start)) {
            self.short_plays = 0;
            return false;
        }
        let follows = self.last_short_end.is_some_and(|end| start - end < STOP_GAP);
        self.short_plays = if follows { self.short_plays + 1 } else { 1 };
        self.last_short_end = Some(now);
        self.short_plays >= 2
    }

    fn on_signal(&mut self, now: f64, signal: Signal, notices: &mut Vec<Notice>) {
        if let Signal::Notice(size) = signal {
            self.on_console_notice(now, size, notices);
            return;
        }
        if signal == Signal::End && self.stop_file_played(now) {
            log::info!("\"{STOP_TITLE}\" was played: the stream stops");
            self.short_plays = 0;
            self.away_at = None;
            self.held = None;
            // The player plays the stop file round and round; with nothing in it, it
            // stops. It is put back when the stream is made ready.
            let silence = vec![0u8; stop_file().len() - AUDIO_START as usize];
            if let Err(e) = self.store.write_at(STOP, AUDIO_START, &silence) {
                log::error!("could not clear the stop file: {e}");
            }
            self.halt();
            self.player = Player::Draining { quiet_since: now };
            self.told_spotify = false;
            notices.push(Notice::Stop);
            return;
        }
        if signal == Signal::Crashed {
            // The console starts its player afresh, and the next thing it does is open the
            // file: it must find it whole. Stop, and get ready as for a first start.
            if !matches!(self.player, Player::Waiting) {
                log::warn!("the console's music player crashed: the stream is made ready again");
                self.halt();
                self.player = Player::Draining { quiet_since: now - QUIET + 1.0 };
            }
            return;
        }
        let start_up = (START_SETTLE * RATE as f64) as u64;
        match (self.player, signal) {
            (Player::Waiting, Signal::Start) => {
                log::info!("the player has started: the stream is running");
                self.player = Player::Playing { t0: now };
                self.last_audio = now;
                self.hold(now, Notice::Restarted(None));
            }
            (Player::Playing { t0 }, Signal::Start) if now - t0 < START_SETTLE => {
                self.player = Player::Playing { t0: now };
            }
            // A start with no end before it. With a file that is rewritten in place it is
            // taken to be the file started again; with a growing file it is ignored (the
            // player reports starts in twos and threes).
            (Player::Playing { .. }, Signal::Start) => {
                if !self.config.grow {
                    self.restart(now);
                }
            }
            (Player::Paused { at, told: false, .. }, Signal::Start) if at < start_up => {
                // The end and second start that the player reports as it starts a file.
                self.player = Player::Playing { t0: now };
            }
            (Player::Paused { at, told, .. }, Signal::Start) => {
                if self.grown && (told || self.resync) {
                    // After a pause the tape and the player are lined up again: nothing
                    // is written until the player must have played all there is and be
                    // waiting at the end of the file.
                    let left = self.written().saturating_sub(at) as f64 / RATE as f64;
                    self.resync = true;
                    self.player = Player::Syncing { until: now + left + SYNC_MARGIN };
                } else if told || self.config.grow {
                    self.resume(now, at, notices);
                } else {
                    // An end and a start close together, well into the file.
                    self.restart(now);
                }
            }
            (Player::Syncing { .. } | Player::Starved, Signal::Start) => {}
            (Player::Syncing { .. } | Player::Starved, Signal::End) => {
                let at = self.written();
                self.player = Player::Paused { at, since: now, told: false };
                if self.grown {
                    if let Err(e) = self.ungrow(now, at) {
                        log::error!("writing to the stream file failed: {e}; the stream stops");
                        self.halt();
                        self.player = Player::Draining { quiet_since: now };
                    }
                }
            }
            (Player::Playing { .. }, Signal::End) => {
                let at = self.heard(now);
                if self.halted && at + (1.5 * RATE as f64) as u64 >= self.written() {
                    self.player = Player::Draining { quiet_since: now };
                } else {
                    self.player = Player::Paused { at, since: now, told: false };
                    if self.grown {
                        if let Err(e) = self.ungrow(now, at) {
                            log::error!("writing to the stream file failed: {e}; the stream stops");
                            self.halt();
                            self.player = Player::Draining { quiet_since: now };
                        }
                    }
                }
            }
            (Player::Draining { .. }, _) => {
                self.player = Player::Draining { quiet_since: now };
            }
            (_, Signal::End) => {}
            (_, Signal::Crashed) => {}
            (_, Signal::Notice(_)) => {}
        }
        if signal == Signal::Start {
            self.last_start = Some(now);
            self.identify_until = Some(now + IDENTIFY_WINDOW);
        }
    }

    /// Keeps `notice` back until the console says what was started (see the top of the
    /// file). A later one takes the place of an earlier one.
    fn hold(&mut self, now: f64, notice: Notice) {
        self.held = Some((now, notice));
    }

    /// The console has put up a notice of `size` bytes.
    fn on_console_notice(&mut self, now: f64, size: u32, notices: &mut Vec<Notice>) {
        if !self.identify_until.is_some_and(|until| now <= until) {
            log::debug!("a notice from the console ({size}) with no start just before it");
            return;
        }
        if size == self.config.stream_notice {
            self.identify_until = None;
            log::debug!("the console's notice is for \"{STREAM_TITLE}\"");
            let returning = self.away_at.take().is_some();
            self.confirmed = true;
            if returning {
                // Other music played in between, and now the stream file again: the player
                // starts it from its beginning, which is where the file is made ready.
                log::info!("\"{STREAM_TITLE}\" was started again after other music");
                let start = self.last_start.unwrap_or(now);
                if let Err(e) = self.leave() {
                    log::error!("writing to the stream file failed: {e}; the stream stops");
                    self.halt();
                    self.player = Player::Draining { quiet_since: now };
                    return;
                }
                self.player = Player::Playing { t0: start };
                self.last_audio = now;
                self.confirmed = true;
                notices.push(Notice::Restarted(None));
                return;
            }
            if let Some((_, notice)) = self.held.take() {
                notices.push(notice);
            }
        } else if size >= TRACK_NOTICE_MIN {
            self.identify_until = None;
            log::info!(
                "the console's player started other music (its notice: {size} bytes, \"{STREAM_TITLE}\" has {})",
                self.config.stream_notice
            );
            self.held = None;
            if !matches!(self.player, Player::Draining { .. }) {
                self.away_at = Some(now + AWAY_DELAY);
            }
        }
    }

    /// Other music is playing: stops the stream and gets the file ready to be started from
    /// its beginning, as at a first start.
    fn away(&mut self, notices: &mut Vec<Notice>) -> io::Result<()> {
        if matches!(self.player, Player::Waiting | Player::Draining { .. }) {
            return Ok(());
        }
        let was_playing = self.confirmed;
        self.leave()?;
        if was_playing {
            log::info!("other music is playing: the stream stops until \"{STREAM_TITLE}\" is played again");
            notices.push(Notice::Away);
        }
        Ok(())
    }

    /// Clears what was played from the file and gets it ready to be started.
    fn leave(&mut self) -> io::Result<()> {
        if self.config.grow {
            self.silence_behind(self.write_frames)?;
        }
        self.arm()
    }

    /// The player has paused (or ended) with a growing file. When it resumes it opens the
    /// file again and finds its place from the file's size, so the file must be its full
    /// size again by then (with a cut-off file it landed near the start and played the
    /// opening silence, 2026-10-11). Silence is put after what has been written, for it
    /// to read into before the file is cut off again.
    fn ungrow(&mut self, now: f64, at: u64) -> io::Result<()> {
        if !self.block.is_empty() {
            self.block.resize(block() * 2, 0);
            self.emit(now)?;
        }
        self.store.set_len(STREAM, flac::frame_offset(self.capacity))?;
        self.grown = false;
        // Enough for the player to read into from where it stopped; silence left from an
        // earlier pause counts.
        let wanted = (at / block() as u64) as u32 + self.prefill;
        let to = wanted.max(self.write_frames).min(self.capacity);
        let silence = vec![0i16; block() * 2];
        let mut frames = Vec::new();
        for index in self.write_frames..to {
            flac::push_frame(&mut frames, index, &silence);
        }
        self.store.write_at(STREAM, flac::frame_offset(self.write_frames), &frames)?;
        self.write_frames = to;
        self.silent_to = to;
        Ok(())
    }

    /// Playback goes on from `at` sample pairs into the file.
    fn resume(&mut self, now: f64, at: u64, notices: &mut Vec<Notice>) {
        self.player = Player::Playing { t0: now - at as f64 / RATE as f64 };
        self.last_audio = now;
        // The console announces the file again; ours for the track goes over it, and is
        // not sent a second time when the track's audio follows.
        self.hold(now, Notice::Restarted(self.current.clone()));
        self.announced_at = self.written();
        if self.config.grow {
            // Spotify is told when the tape is about to need its audio again (see
            // `advance`); until then the player is working through the silence.
            return;
        }
        if self.told_spotify {
            log::info!("resumed at the console");
            self.told_spotify = false;
            notices.push(Notice::Resumed);
        }
    }

    /// The player started the file again from its beginning while the stream was running.
    fn restart(&mut self, now: f64) {
        if self.halted {
            self.player = Player::Draining { quiet_since: now };
            return;
        }
        if self.config.grow {
            // It took the file's size as it is now, which is no use: stop, and get ready
            // to be started on a file of full size.
            log::info!("the player started the stream again: the stream stops; play it once more");
            self.halt();
            self.player = Player::Draining { quiet_since: now };
            return;
        }
        log::info!("the player started the stream again from its beginning");
        if let Err(e) = self.rewind() {
            log::error!("writing to the stream file failed: {e}; the stream stops");
            self.halt();
            self.player = Player::Draining { quiet_since: now };
            return;
        }
        self.player = Player::Playing { t0: now };
        self.last_audio = now;
    }

    /// Changes of state that come from time passing.
    fn advance(&mut self, now: f64, notices: &mut Vec<Notice>) -> io::Result<()> {
        if self.held.as_ref().is_some_and(|(since, _)| now - since > HOLD_LIMIT) {
            // The console put up no notice: the start is taken to be the stream file's.
            let (_, notice) = self.held.take().unwrap();
            self.confirmed = true;
            notices.push(notice);
        }
        if self.away_at.is_some_and(|at| now >= at) {
            self.away_at = None;
            self.away(notices)?;
        }
        match self.player {
            Player::Playing { t0 } => {
                let elapsed = ((now - t0).max(0.0) * RATE as f64) as u64;
                let settled = self.last_start.map_or(true, |start| now - start > START_SETTLE + 0.5);
                // Not once other music has been seen to start: the file must be whole
                // when the stream file is started again. (Not held back until the
                // console's notice either: with the file whole the player reads further
                // ahead, into silence, and audio written there later goes unheard.)
                let known = self.away_at.is_none();
                if self.config.grow && !self.grown && !self.halted && settled && known && now - t0 > START_SETTLE + 0.5 {
                    // The player has the file open and has read some way into the silence
                    // at its start. From here on the file ends where the audio ends.
                    self.store.set_len(STREAM, flac::frame_offset(self.write_frames))?;
                    self.grown = true;
                    self.wiped_to = self.wiped_to.max(self.prefill).min(self.write_frames);
                    log::info!("the file now grows with the audio");
                }
                let idle = !self.told_spotify && now - self.last_audio > self.config.idle_limit;
                if elapsed >= self.written() && self.starved && self.grown && !self.halted {
                    // The player has played all there is and waits at the end of the file.
                    self.player = Player::Starved;
                } else if elapsed >= self.written() {
                    if self.write_frames >= self.capacity {
                        log::warn!("the stream file has been played to its end: play it again to go on");
                    } else if !self.halted {
                        log::error!("the player caught up with the stream");
                    }
                    self.halt();
                    self.player = Player::Draining { quiet_since: now };
                } else if !self.halted && idle && self.config.grow {
                    // With a growing file the stream is not stopped: the file would have
                    // to be made whole again while the player still has it open. Writing
                    // simply stops, and the player waits at the end of the file.
                    if !self.starved && self.grown {
                        log::info!(
                            "nothing has played for {:.0} s: nothing more is written until something does",
                            self.config.idle_limit
                        );
                        self.starved = true;
                    }
                } else if !self.halted && idle {
                    log::info!(
                        "nothing has played for {:.0} s: the stream stops; play it again when needed",
                        self.config.idle_limit
                    );
                    self.halt();
                }
                let lead = ((self.config.lead + 1.0) * RATE as f64) as u64;
                if self.told_spotify && self.grown && self.written() < elapsed + lead {
                    log::info!("resumed at the console");
                    self.told_spotify = false;
                    notices.push(Notice::Resumed);
                }
                while self.pending.front().is_some_and(|&(at, _)| at <= elapsed) {
                    let (_, track) = self.pending.pop_front().unwrap();
                    notices.push(Notice::Track(track));
                }
            }
            Player::Paused { at, since, told: false } if now - since > PAUSE_SETTLE => {
                log::info!("paused at the console, {:.0} s into the stream", at as f64 / RATE as f64);
                self.player = Player::Paused { at, since, told: true };
                if !self.told_spotify {
                    self.told_spotify = true;
                    notices.push(Notice::Paused);
                }
            }
            Player::Syncing { until } if now >= until => {
                // The player is waiting at the end of the file. A lead's worth of silence
                // at once puts the tape that far ahead of it again.
                let at = self.written();
                self.block.clear();
                self.block_has_audio = false;
                for _ in 0..frames_in(self.config.lead) {
                    if self.write_frames >= self.capacity {
                        break;
                    }
                    self.block.resize(block() * 2, 0);
                    self.emit(now)?;
                }
                self.resync = false;
                self.resume(now, at, notices);
            }
            Player::Starved if self.feed.has_audio() => {
                log::info!("audio again: the stream goes on");
                self.starved = false;
                self.player = Player::Syncing { until: now };
            }
            // With a growing file a long pause changes nothing: nothing is written while
            // paused, and the file must stay as the player left it.
            Player::Paused { since, .. } if !self.config.grow && now - since > self.config.idle_limit => {
                log::info!("paused at the console for a long time: the stream stops");
                self.halt();
                self.player = Player::Draining { quiet_since: now };
            }
            Player::Draining { quiet_since } if now - quiet_since > QUIET => {
                self.arm()?;
            }
            _ => {}
        }
        Ok(())
    }

    /// Wipes old audio: whatever the file holds past the silence that follows the write
    /// head. After the stream has been stopped or started again, that is everything that
    /// was played before. A piece at a time, from the top down.
    fn wipe(&mut self, now: f64) -> io::Result<()> {
        if self.config.grow {
            // A growing file has its audio replaced by silence behind what is heard, a
            // piece at a time.
            if self.grown && !self.halted {
                let heard = (self.heard(now) / block() as u64) as u32;
                let behind = heard.saturating_sub(frames_in(KEEP_BEHIND));
                self.silence_behind(behind.min(self.wiped_to + SILENCE_BEHIND_STEP))?;
            }
            return Ok(());
        }
        let floor = flac::frame_offset(self.silent_to.max(self.write_frames));
        if self.dirty_end > floor {
            let playing = matches!(self.player, Player::Playing { .. });
            let step = if playing { WIPE_STEP_PLAYING } else { WIPE_STEP_IDLE };
            let from = floor.max(self.dirty_end.saturating_sub(step));
            let zeros = vec![0u8; (self.dirty_end - from) as usize];
            self.store.write_at(STREAM, from, &zeros)?;
            self.dirty_end = from;
            if from == floor {
                log::info!("old audio wiped");
            }
        }
        self.dirty_end = self.dirty_end.max(floor);
        if self.marked.abs_diff(self.dirty_end) >= MARK_STEP || (self.dirty_end == floor && self.marked > floor + MARK_STEP) {
            // Noted a little high, so that what is written before the next note is covered.
            let noted = self.dirty_end + MARK_STEP;
            let mut note = MARK.to_vec();
            note.extend(noted.to_le_bytes());
            self.store.write_at(STREAM, MARK_AT, &note)?;
            self.marked = self.dirty_end;
        }
        Ok(())
    }

    /// Replaces the audio from where that was last done up to frame `to` with silence.
    fn silence_behind(&mut self, to: u32) -> io::Result<()> {
        let from = self.wiped_to.max(self.prefill);
        if to > from {
            let silence = vec![0i16; block() * 2];
            let mut frames = Vec::new();
            for index in from..to {
                flac::push_frame(&mut frames, index, &silence);
            }
            self.store.write_at(STREAM, flac::frame_offset(from), &frames)?;
            self.wiped_to = to;
        }
        Ok(())
    }

    /// Keeps silence in the file just past the write head, a piece at a time.
    fn silence_ahead(&mut self) -> io::Result<()> {
        if self.halted || self.config.grow {
            return Ok(());
        }
        let wanted = (self.write_frames + frames_in(SILENCE_AHEAD)).min(self.capacity);
        if self.silent_to < wanted {
            let to = wanted.min(self.silent_to + frames_in(SILENCE_STEP));
            let silence = vec![0i16; block() * 2];
            let mut frames = Vec::new();
            for index in self.silent_to..to {
                flac::push_frame(&mut frames, index, &silence);
            }
            self.store.write_at(STREAM, flac::frame_offset(self.silent_to), &frames)?;
            self.silent_to = to;
        }
        Ok(())
    }

    fn emit(&mut self, now: f64) -> io::Result<()> {
        self.frame.clear();
        flac::push_frame(&mut self.frame, self.write_frames, &self.block);
        let offset = flac::frame_offset(self.write_frames);
        self.store.write_at(STREAM, offset, &self.frame)?;
        self.write_frames += 1;
        self.silent_to = self.silent_to.max(self.write_frames);
        if self.block_has_audio {
            self.last_audio = now;
        }
        self.block.clear();
        self.block_has_audio = false;
        Ok(())
    }

    fn write(&mut self, now: f64) -> io::Result<()> {
        if self.halted || !matches!(self.player, Player::Playing { .. }) {
            return Ok(());
        }
        if self.starved {
            if !self.feed.has_audio() {
                return Ok(());
            }
            // Audio came back before the player ran out of file: carry on as usual.
            self.starved = false;
            self.last_audio = now;
        }
        let target = (self.config.lead * RATE as f64) as i64;
        let slack = (self.config.slack * RATE as f64) as i64;
        for _ in 0..FRAMES_PER_TICK {
            if self.write_frames >= self.capacity {
                if !self.full_logged {
                    log::warn!("the stream file is full");
                    self.full_logged = true;
                }
                break;
            }
            let ahead = self.written() as i64 - self.heard(now) as i64;
            if ahead >= target {
                break;
            }
            // Fill the block from the queue; a track change is noted where it falls.
            let filled = loop {
                let room = block() * 2 - self.block.len();
                if room == 0 {
                    break true;
                }
                let before = self.block.len();
                match self.feed.pop_into(&mut self.block, room) {
                    Popped::Samples => {
                        self.block_has_audio = true;
                        // A track counts from its first audio, not from its announcement.
                        if let Some(track) = self.announced.take() {
                            let at = self.written() + (before / 2) as u64;
                            let again = self.current.as_ref().is_some_and(|now| now.tags == track.tags)
                                && at < self.announced_at + 5 * RATE;
                            if !again {
                                self.pending.push_back((at, track.clone()));
                                self.announced_at = at;
                            }
                            self.current = Some(track);
                        }
                    }
                    Popped::Track(track) => self.announced = Some(track),
                    Popped::Empty => break false,
                }
            };
            if !filled {
                if ahead >= target - slack {
                    // Audio is on its way and there is time to wait for it.
                    break;
                }
                self.block.resize(block() * 2, 0);
            }
            self.emit(now)?;
        }
        Ok(())
    }

    /// Call every few tens of milliseconds with the time in seconds and what the console's
    /// log has said since the last call.
    pub fn tick(&mut self, now: f64, signals: &[Signal]) -> Vec<Notice> {
        let mut notices = Vec::new();
        for &signal in signals {
            self.on_signal(now, signal, &mut notices);
        }
        let result = self
            .advance(now, &mut notices)
            .and_then(|()| self.write(now))
            .and_then(|()| self.silence_ahead())
            .and_then(|()| self.wipe(now));
        if let Err(e) = result {
            if !self.halted {
                log::error!("writing to the stream file failed: {e}; the stream stops");
            }
            self.halt();
        }
        let running = matches!(
            self.player,
            Player::Playing { .. } | Player::Paused { told: false, .. } | Player::Syncing { .. } | Player::Starved
        );
        self.feed.set_accepting(!self.halted && running && self.write_frames < self.capacity);
        notices
    }

    /// Re-arm only after the native USB worker has confirmed Stop.
    #[cfg(feature="usb-autostart")]
    pub fn prepare_native_start(&mut self) -> io::Result<()> {
        self.last_start = None;
        self.last_short_end = None;
        self.short_plays = 0;
        self.leave()
    }

    /// One line on where the stream is, for the log.
    pub fn status(&self, now: f64) -> String {
        format!(
            "{:?}, written to {:.1} s, {:.1} s ahead{}",
            self.player,
            self.written() as f64 / RATE as f64,
            (self.written() as i64 - self.heard(now) as i64) as f64 / RATE as f64,
            if self.halted { ", stopped" } else { "" }
        )
    }
}

#[cfg(test)]
mod tests;
