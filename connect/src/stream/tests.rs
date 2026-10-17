//! The stream run against a pretend console player that follows what was observed on the
//! real one: it takes a file's size when it opens it, reads about nine seconds ahead of
//! what is heard, ends a file at the first thing that is not a valid frame, and half a
//! second later starts the folder again from its first file. A file with next to no audio
//! is not played, and the player stops there. When it starts a file the console puts up a
//! notice whose size depends on the file; on a resume it puts up none.

use std::sync::Mutex;

use super::*;
use crate::feed::Offer;

const READ_AHEAD: f64 = 9.0;
const GAP: f64 = 0.5;
/// A track of someone's own, in another folder.
const OTHER: usize = 2;

/// The size of the console's notice for each file.
fn notice_for(file: usize) -> u32 {
    match file {
        STREAM => Config::default().stream_notice,
        STOP => 790,
        _ => 805,
    }
}

/// A minute of someone's own music: every sample is 7.
fn other_file() -> Vec<u8> {
    let frames = (60 * RATE) as u32 / block() as u32;
    let mut file = flac::header(frames as u64 * block() as u64, &Tags::default());
    let audio = vec![7i16; block() * 2];
    for index in 0..frames {
        flac::push_frame(&mut file, index, &audio);
    }
    file
}

struct MemStore {
    files: Mutex<Vec<Vec<u8>>>,
}

impl Store for MemStore {
    fn len(&self, slot: usize) -> io::Result<u64> {
        Ok(self.files.lock().unwrap()[slot].len() as u64)
    }
    fn set_len(&self, slot: usize, length: u64) -> io::Result<()> {
        self.files.lock().unwrap()[slot].resize(length as usize, 0);
        Ok(())
    }
    fn read_at(&self, slot: usize, offset: u64, buffer: &mut [u8]) -> io::Result<usize> {
        let files = self.files.lock().unwrap();
        let file = &files[slot];
        let start = (offset as usize).min(file.len());
        let count = buffer.len().min(file.len() - start);
        buffer[..count].copy_from_slice(&file[start..start + count]);
        Ok(count)
    }
    fn write_at(&self, slot: usize, offset: u64, data: &[u8]) -> io::Result<()> {
        let mut files = self.files.lock().unwrap();
        let file = &mut files[slot];
        let end = offset as usize + data.len();
        if file.len() < end {
            file.resize(end, 0);
        }
        file[offset as usize..end].copy_from_slice(data);
        Ok(())
    }
}

struct Open {
    file: usize,
    size: u64,
    t0: f64,
    data: Vec<u8>,
    cursor: usize,
    played: u64,
}

enum State {
    Stopped,
    Playing(Open),
    Paused(Open, f64),
    Gap { file: usize, until: f64 },
}

struct Console {
    store: Arc<MemStore>,
    state: State,
    /// Every sample that came out of the speakers from the stream file.
    heard: Vec<i16>,
    said: Vec<Signal>,
    /// Seconds spent waiting at the end of a file that was cut short.
    waited: f64,
}

/// Reads on, as far as the size taken at open and the file as it is now allow.
fn read_to(open: &mut Open, store: &MemStore, to: u64) {
    let to = to.min(open.size).min(store.len(open.file).unwrap()) as usize;
    if to > open.data.len() {
        let from = open.data.len();
        open.data.resize(to, 0);
        let got = store.read_at(open.file, from as u64, &mut open.data[from..]).unwrap();
        assert_eq!(got, to - from);
    }
}

impl Console {
    /// Starts a file if it has audio at its beginning; stops if it has not.
    fn play(&mut self, file: usize, now: f64) {
        let size = self.store.len(file).unwrap();
        let mut open = Open { file, size, t0: now, data: Vec::new(), cursor: AUDIO_START as usize, played: 0 };
        read_to(&mut open, &self.store, flac::frame_offset(4));
        let mut at = AUDIO_START as usize;
        let mut frames = 0;
        while let Some((_, _, size)) = open.data.get(at..).and_then(flac::read_frame) {
            at += size;
            frames += 1;
        }
        if frames < 3 {
            self.state = State::Stopped;
            return;
        }
        self.state = State::Playing(open);
        self.said.push(Signal::Start);
        self.said.push(Signal::Notice(notice_for(file)));
    }

    fn pause(&mut self, now: f64) {
        if let State::Playing(open) = std::mem::replace(&mut self.state, State::Stopped) {
            let elapsed = now - open.t0;
            self.state = State::Paused(open, elapsed);
            self.said.push(Signal::End);
        }
    }

    /// The player opens the file again and finds its place from the file's size. If the
    /// file is not the size its stated length calls for, it lands near the start.
    fn resume(&mut self, now: f64) {
        if let State::Paused(mut open, elapsed) = std::mem::replace(&mut self.state, State::Stopped) {
            let size = self.store.len(open.file).unwrap();
            if open.file == STREAM && size < open.size {
                open = Open { file: STREAM, size, t0: now, data: Vec::new(), cursor: AUDIO_START as usize, played: 0 };
            } else {
                open.t0 = now - elapsed;
                open.size = size;
                open.data.truncate(open.cursor);
            }
            self.state = State::Playing(open);
            self.said.push(Signal::Start);
        }
    }

    /// Picks another file while one is playing.
    fn switch_to(&mut self, file: usize, now: f64) {
        self.said.push(Signal::End);
        self.play(file, now);
    }

    fn tick(&mut self, now: f64) -> Vec<Signal> {
        match &mut self.state {
            State::Playing(open) => {
                let elapsed = ((now - open.t0) * RATE as f64) as u64;
                let ahead = elapsed + (READ_AHEAD * RATE as f64) as u64;
                read_to(open, &self.store, flac::frame_offset((ahead / block() as u64) as u32 + 1));
                let mut ended = false;
                while open.played <= elapsed {
                    read_to(open, &self.store, open.cursor as u64 + flac::frame_size(u32::MAX));
                    match flac::read_frame(&open.data[open.cursor..]) {
                        Some((_, samples, size)) => {
                            open.cursor += size;
                            open.played += samples.len() as u64 / 2;
                            if open.file == STREAM {
                                self.heard.extend(samples);
                            }
                        }
                        // The file is shorter than it was when it was opened, and this is
                        // its end: the assumption under test is that the player waits.
                        None if open.data.len() as u64 == self.store.len(open.file).unwrap()
                            && (open.data.len() as u64) < open.size
                            && ((open.data.len() - open.cursor) as u64) < flac::frame_size(0) =>
                        {
                            open.t0 += 0.01;
                            self.waited += 0.01;
                            break;
                        }
                        None => {
                            ended = true;
                            break;
                        }
                    }
                }
                if ended {
                    self.said.push(Signal::End);
                    // Each file is alone in its folder: the player goes round to it again.
                    self.state = State::Gap { file: open.file, until: now + GAP };
                }
            }
            State::Gap { file, until } if now >= *until => {
                let file = *file;
                self.play(file, now);
            }
            _ => {}
        }
        std::mem::take(&mut self.said)
    }
}

/// Stands in for the receiver. Sample pair `i` of track `n` is `(1 + i % 20000, n)`.
struct Feeder {
    plan: VecDeque<(i16, u64)>,
    current: Option<(i16, u64, u64)>,
    held: bool,
}

impl Feeder {
    fn step(&mut self, feed: &Feed) {
        if self.held {
            return;
        }
        loop {
            if self.current.is_none() {
                let Some((number, pairs)) = self.plan.pop_front() else { return };
                let tags = Tags { title: format!("Track {number}"), ..Tags::default() };
                feed.track(Track { tags, seconds: 0.0, cover: String::new() });
                self.current = Some((number, 0, pairs));
            }
            let (number, sent, total) = self.current.unwrap();
            let count = (total - sent).min(1024);
            let chunk: Vec<i16> =
                (sent..sent + count).flat_map(|i| [1 + (i % 20000) as i16, number]).collect();
            match feed.offer(&chunk) {
                Offer::Taken => {
                    self.current = (sent + count < total).then_some((number, sent + count, total));
                }
                _ => return,
            }
        }
    }
}

struct Rig {
    store: Arc<MemStore>,
    feed: Arc<Feed>,
    stream: Stream,
    console: Console,
    feeder: Feeder,
    now: f64,
    delay: f64,
    in_flight: VecDeque<(f64, Signal)>,
    /// What the stream reported, with the time.
    notices: Vec<(f64, String)>,
    /// When it reported the stream file started or resumed, and the track then on.
    restarts: Vec<(f64, Option<String>)>,
    /// The receiver was paused because the stream stopped for other music; it goes on when
    /// the stream file is started (as the receiver does).
    paused_for_other: bool,
}

impl Rig {
    fn new(config: Config) -> Rig {
        let store = Arc::new(MemStore { files: Mutex::new(vec![Vec::new(), Vec::new(), other_file()]) });
        let feed = Feed::new();
        let console =
            Console { store: store.clone(), state: State::Stopped, heard: Vec::new(), said: Vec::new(), waited: 0.0 };
        let stream = Stream::new(config, store.clone(), feed.clone()).unwrap();
        let feeder = Feeder { plan: VecDeque::new(), current: None, held: false };
        Rig {
            store,
            feed,
            stream,
            console,
            feeder,
            now: 0.0,
            delay: 0.3,
            in_flight: VecDeque::new(),
            notices: Vec::new(),
            restarts: Vec::new(),
            paused_for_other: false,
        }
    }

    fn step(&mut self) {
        self.now += 0.01;
        for signal in self.console.tick(self.now) {
            self.in_flight.push_back((self.now + self.delay, signal));
        }
        let mut arrived = Vec::new();
        while self.in_flight.front().is_some_and(|&(at, _)| at <= self.now) {
            arrived.push(self.in_flight.pop_front().unwrap().1);
        }
        for notice in self.stream.tick(self.now, &arrived) {
            let text = match notice {
                Notice::Paused => {
                    self.feeder.held = true;
                    "paused".to_string()
                }
                Notice::Resumed => {
                    self.feeder.held = false;
                    "resumed".to_string()
                }
                Notice::Stop => {
                    self.feeder.held = true;
                    "stop".to_string()
                }
                Notice::Track(track) => track.tags.title,
                Notice::Away => {
                    self.feeder.held = true;
                    self.paused_for_other = true;
                    "away".to_string()
                }
                // Kept apart, so the other tests need not list them.
                Notice::Restarted(track) => {
                    if std::mem::take(&mut self.paused_for_other) {
                        self.feeder.held = false;
                    }
                    self.restarts.push((self.now, track.map(|track| track.tags.title)));
                    continue;
                }
            };
            self.notices.push((self.now, text));
        }
        self.feeder.step(&self.feed);
    }

    fn run(&mut self, seconds: f64) {
        let until = self.now + seconds;
        while self.now < until {
            self.step();
        }
    }

    fn plan(&mut self, number: i16, seconds: f64) {
        self.feeder.plan.push_back((number, (seconds * RATE as f64) as u64));
    }

    /// The sample pairs of track `number` that were heard, checked to be in order with
    /// none missing between the first and the last.
    fn heard_of(&self, number: i16) -> u64 {
        let mut expected = None;
        let mut count = 0;
        for pair in self.console.heard.chunks_exact(2).filter(|pair| pair[1] == number) {
            if let Some(expected) = expected {
                assert_eq!(pair[0], expected, "track {number}: a jump after {count} pairs");
            }
            expected = Some(1 + (pair[0] as u64 % 20000) as i16);
            count += 1;
        }
        count
    }

    fn names(&self) -> Vec<&str> {
        self.notices.iter().map(|(_, text)| text.as_str()).collect()
    }
}

/// The file rewritten in place, with the long lead that needs. The tests of the growing
/// file say so.
fn config() -> Config {
    Config { idle_limit: 30.0, hours: 0.5, grow: false, lead: 11.0, slack: 0.5, ..Config::default() }
}

#[test]
fn tracks_follow_each_other_without_a_break() {
    let mut rig = Rig::new(config());
    rig.run(1.0);
    assert!(matches!(rig.stream.player, Player::Waiting));
    rig.console.play(STREAM, rig.now);
    rig.run(3.0);
    rig.plan(1, 25.0);
    rig.plan(2, 40.0);
    rig.run(100.0);
    assert_eq!(rig.heard_of(1), 25 * RATE);
    assert_eq!(rig.heard_of(2), 40 * RATE);
    // The second track's first sample comes straight after the first track's last.
    let pairs: Vec<&[i16]> = rig.console.heard.chunks_exact(2).collect();
    let change = pairs.iter().position(|pair| pair[1] == 2).unwrap();
    assert_eq!(pairs[change - 1][1], 1);
    // Each track is reported when it is heard, not when it is received.
    assert_eq!(rig.names(), ["Track 1", "Track 2"]);
    let heard_at = |n: usize| rig.notices[n].0;
    assert!((heard_at(1) - heard_at(0) - 25.0).abs() < 0.5, "{:?}", rig.notices);
    // The player started the file once and is still in it.
    assert!(matches!(rig.console.state, State::Playing(_)));
}

#[test]
fn an_idle_stream_stops_the_player_and_can_be_started_again() {
    let mut rig = Rig::new(config());
    rig.run(1.0);
    rig.console.play(STREAM, rig.now);
    rig.plan(1, 10.0);
    rig.run(120.0);
    assert_eq!(rig.heard_of(1), 10 * RATE);
    // It did not go round and round its own file.
    assert!(matches!(rig.console.state, State::Stopped));
    assert!(matches!(rig.stream.player, Player::Waiting), "{}", rig.stream.status(rig.now));

    rig.console.play(STREAM, rig.now);
    rig.plan(2, 10.0);
    rig.run(60.0);
    assert_eq!(rig.heard_of(2), 10 * RATE);
}

#[test]
fn a_pause_at_the_console_loses_nothing() {
    let mut rig = Rig::new(config());
    rig.run(1.0);
    rig.console.play(STREAM, rig.now);
    rig.plan(1, 60.0);
    rig.run(40.0);
    rig.console.pause(rig.now);
    rig.run(20.0);
    rig.console.resume(rig.now);
    rig.run(80.0);
    assert_eq!(rig.names(), ["Track 1", "paused", "resumed"]);
    assert_eq!(rig.heard_of(1), 60 * RATE);
}

#[test]
fn the_stop_file_stops_the_stream() {
    let mut rig = Rig::new(config());
    rig.run(1.0);
    rig.console.play(STREAM, rig.now);
    rig.plan(1, 300.0);
    rig.run(40.0);
    rig.console.switch_to(STOP, rig.now);
    rig.run(20.0);
    assert_eq!(rig.names(), ["Track 1", "stop"]);
    // The player did not keep playing the stop file round and round.
    assert!(matches!(rig.console.state, State::Stopped));
    assert!(matches!(rig.stream.player, Player::Waiting), "{}", rig.stream.status(rig.now));
    // Nothing of what was played is left in the stream file: after the opening silence and
    // the silence kept ahead of the write head, it is blank.
    {
        let files = rig.store.files.lock().unwrap();
        let after = flac::frame_offset(frames_in(14.0 + 31.0)) as usize;
        assert!(files[STREAM][after..].iter().all(|&byte| byte == 0), "old audio is still in the file");
        assert_eq!(&files[STREAM][MARK_AT as usize..MARK_AT as usize + 4], MARK);
    }
    // Both files are as they were at the start: the stop file plays, the stream starts.
    assert_eq!(rig.store.files.lock().unwrap()[STOP], stop_file());
    rig.feeder.held = false;
    rig.console.play(STREAM, rig.now);
    rig.run(30.0);
    assert!(matches!(rig.console.state, State::Playing(_)));
    let pairs = rig.console.heard.chunks_exact(2).filter(|pair| pair[1] == 1).count() as u64;
    assert!(pairs > 35 * RATE, "{pairs}");
}

#[test]
fn starting_the_file_again_carries_on_with_what_is_playing_now() {
    let mut rig = Rig::new(config());
    rig.run(1.0);
    rig.console.play(STREAM, rig.now);
    rig.plan(1, 300.0);
    rig.run(60.0);
    let before = rig.console.heard.len();
    // Previous or next in a folder of one: the same file from its beginning.
    rig.console.switch_to(STREAM, rig.now);
    rig.run(40.0);
    assert!(matches!(rig.console.state, State::Playing(_)));
    let after: Vec<&[i16]> = rig.console.heard[before..].chunks_exact(2).collect();
    // Silence first, then the track from where Spotify is now: nothing from the start of
    // the file is heard a second time.
    let first = after.iter().find(|pair| pair[1] == 1).expect("the track carries on");
    assert!(first[0] as u64 > 1 + (40 * RATE) % 20000, "old audio was heard again: {}", first[0]);
    let silent = after.iter().take_while(|pair| pair[1] == 0).count() as u64;
    assert!(silent >= 12 * RATE && silent <= 16 * RATE, "{silent}");
    assert_eq!(rig.names(), ["Track 1", "Track 1"]);
}

#[test]
fn a_pause_soon_after_a_resume_is_not_taken_for_the_stop_file() {
    let mut rig = Rig::new(config());
    rig.run(1.0);
    rig.console.play(STREAM, rig.now);
    rig.plan(1, 120.0);
    rig.run(40.0);
    rig.console.pause(rig.now);
    rig.run(2.0);
    rig.console.resume(rig.now);
    rig.run(2.04);
    rig.console.pause(rig.now);
    rig.run(5.0);
    rig.console.resume(rig.now);
    rig.run(20.0);
    assert!(!rig.names().contains(&"stop"), "{:?}", rig.names());
    assert!(matches!(rig.console.state, State::Playing(_)));
}

#[test]
fn a_growing_file_survives_pauses_at_the_console() {
    let mut rig = Rig::new(Config { grow: true, lead: 2.0, slack: 0.3, ..config() });
    rig.run(1.0);
    rig.console.play(STREAM, rig.now);
    rig.plan(1, 90.0);
    rig.run(40.0);
    for held in [6.0, 1.15, 3.0] {
        rig.console.pause(rig.now);
        rig.run(held);
        rig.console.resume(rig.now);
        // The player reports a resume as a start, an end and another start.
        rig.console.said.extend([Signal::End, Signal::Start]);
        rig.run(2.04);
    }
    // Until the whole track has been heard (after that the stream goes idle, and the
    // player waiting then is as intended).
    for _ in 0..150 {
        rig.run(1.0);
        if rig.console.heard.chunks_exact(2).filter(|pair| pair[1] == 1).count() as u64 == 90 * RATE {
            break;
        }
    }
    assert!(!rig.names().contains(&"stop"), "{:?}", rig.names());
    assert_eq!(rig.heard_of(1), 90 * RATE);
    // In step again after each pause: the player never waits long, and is not far behind.
    assert!(rig.console.waited < 3.0, "the player waited {} s in all", rig.console.waited);
    // Spotify was told of the pause, and of the resume once the tape needed audio again.
    assert_eq!(rig.names(), ["Track 1", "paused", "resumed"]);
}

#[test]
fn a_growing_file_keeps_the_delay_short() {
    let mut rig = Rig::new(Config { grow: true, lead: 2.0, slack: 0.3, ..config() });
    rig.run(1.0);
    let full = rig.store.len(STREAM).unwrap();
    rig.console.play(STREAM, rig.now);
    rig.run(20.0);
    // The file has been cut off and ends a couple of seconds past what is heard.
    assert!(rig.store.len(STREAM).unwrap() < flac::frame_offset(frames_in(25.0)));
    rig.plan(1, 30.0);
    let asked = rig.now;
    rig.plan(2, 20.0);
    rig.run(70.0);
    assert_eq!(rig.heard_of(1), 30 * RATE);
    assert_eq!(rig.heard_of(2), 20 * RATE);
    // Audio that has been played does not stay in the file, and what replaces it is
    // silence in valid frames, all the way from the start: the player looks through the
    // file when it resumes, and must not meet anything else.
    {
        let files = rig.store.files.lock().unwrap();
        let mut at = AUDIO_START as usize;
        for index in 0..frames_in(50.0) {
            let (number, samples, size) = flac::read_frame(&files[STREAM][at..]).expect("a valid frame");
            assert_eq!(number, index);
            assert!(samples.iter().all(|&sample| sample == 0), "played audio is still in frame {index}");
            at += size;
        }
    }
    // The first track is heard about two seconds after it arrives, not eleven.
    assert_eq!(rig.names(), ["Track 1", "Track 2"]);
    let delay = rig.notices[0].0 - asked;
    assert!(delay > 1.0 && delay < 3.0, "{delay}");
    assert!(rig.console.waited < 0.2, "the player ran dry for {} s", rig.console.waited);

    // Nothing more arrives: after the idle time nothing more is written, the file stays
    // as it is, and the player waits at its end.
    rig.run(60.0);
    assert!(matches!(rig.console.state, State::Playing(_)));
    assert!(matches!(rig.stream.player, Player::Starved), "{}", rig.stream.status(rig.now));
    let length = rig.store.len(STREAM).unwrap();
    assert!(length < full);
    rig.run(30.0);
    assert_eq!(rig.store.len(STREAM).unwrap(), length, "written to while idle");
    // When something arrives the stream goes on, in step with the player.
    let asked = rig.now;
    rig.plan(3, 10.0);
    rig.run(40.0);
    assert_eq!(rig.heard_of(3), 10 * RATE);
    let delay = rig.notices.last().unwrap().0 - asked;
    assert!(delay > 1.0 && delay < 3.5, "{delay}");
}

/// The pretend player has teeth: with less written ahead than it reads ahead, it meets
/// unwritten tape.
#[test]
#[should_panic(expected = "left: 0")]
fn too_short_a_lead_is_caught() {
    let mut rig = Rig::new(Config { lead: 6.0, ..config() });
    rig.run(1.0);
    rig.console.play(STREAM, rig.now);
    rig.plan(1, 60.0);
    rig.run(120.0);
    assert_eq!(rig.heard_of(1), 60 * RATE);
}

#[test]
fn the_start_is_announced_once_the_console_has_said_what_started() {
    let mut rig = Rig::new(Config { grow: true, lead: 2.0, slack: 0.3, ..config() });
    rig.run(1.0);
    // Someone's own music first: no "ready", and nothing to report when the stream stops
    // for it, since it never played.
    rig.console.play(OTHER, rig.now);
    rig.run(10.0);
    assert!(rig.restarts.is_empty(), "{:?}", rig.restarts);
    assert!(rig.notices.is_empty(), "{:?}", rig.notices);
    assert!(matches!(rig.stream.player, Player::Waiting), "{}", rig.stream.status(rig.now));
    // Now the stream file: "ready" as soon as the console's notice is in.
    let started = rig.now;
    rig.console.switch_to(STREAM, rig.now);
    rig.run(5.0);
    assert_eq!(rig.restarts.len(), 1, "{:?}", rig.restarts);
    assert_eq!(rig.restarts[0].1, None);
    assert!(rig.restarts[0].0 - started < 1.0, "{:?}", rig.restarts);
    rig.plan(1, 20.0);
    rig.run(40.0);
    assert_eq!(rig.heard_of(1), 20 * RATE);
}

#[test]
fn other_music_stops_the_stream_and_it_carries_on_when_started_again() {
    let mut rig = Rig::new(Config { grow: true, lead: 2.0, slack: 0.3, ..config() });
    rig.run(1.0);
    rig.console.play(STREAM, rig.now);
    rig.plan(1, 300.0);
    rig.run(40.0);
    let before = rig.console.heard.len();
    // Someone's own track, picked while the stream plays.
    rig.console.switch_to(OTHER, rig.now);
    rig.run(20.0);
    assert_eq!(rig.names(), ["Track 1", "away"]);
    assert!(matches!(rig.stream.player, Player::Waiting), "{}", rig.stream.status(rig.now));
    assert!(!rig.feed.accepting(), "audio would go nowhere");
    // The file is whole again, for a start from its beginning, with nothing of what was
    // played left in it: silence in valid frames up to where the writing had got to.
    {
        let files = rig.store.files.lock().unwrap();
        assert_eq!(files[STREAM].len() as u64, flac::frame_offset(frames_in(0.5 * 3600.0)));
        let mut at = AUDIO_START as usize;
        let mut index = 0;
        while let Some((number, samples, size)) = flac::read_frame(&files[STREAM][at..]) {
            assert_eq!(number, index);
            assert!(samples.iter().all(|&sample| sample == 0), "played audio is still in frame {index}");
            at += size;
            index += 1;
        }
        assert!(index >= frames_in(40.0), "silence only to frame {index}");
    }
    // Back to the stream file, from the list: it starts from its beginning, and Spotify
    // goes on after the opening silence.
    rig.console.switch_to(STREAM, rig.now);
    rig.run(40.0);
    assert!(matches!(rig.console.state, State::Playing(_)));
    assert_eq!(rig.restarts.last().unwrap().1, None, "{:?}", rig.restarts);
    let after: Vec<&[i16]> = rig.console.heard[before..].chunks_exact(2).collect();
    let silent = after.iter().take_while(|pair| pair[1] == 0).count() as u64;
    assert!(silent >= 12 * RATE && silent <= 17 * RATE, "{silent}");
    let audio = after.iter().filter(|pair| pair[1] == 1).count() as u64;
    assert!(audio > 20 * RATE, "{audio}");
    assert!(rig.console.waited < 1.0, "the player waited {} s", rig.console.waited);
}

#[test]
fn other_music_after_a_pause_at_the_console() {
    let mut rig = Rig::new(Config { grow: true, lead: 2.0, slack: 0.3, ..config() });
    rig.run(1.0);
    rig.console.play(STREAM, rig.now);
    rig.plan(1, 300.0);
    rig.run(40.0);
    rig.console.pause(rig.now);
    rig.run(10.0);
    // Someone's own track instead of a resume.
    rig.console.state = State::Stopped;
    rig.console.play(OTHER, rig.now);
    rig.run(20.0);
    assert_eq!(rig.names(), ["Track 1", "paused", "away"]);
    assert!(matches!(rig.stream.player, Player::Waiting), "{}", rig.stream.status(rig.now));
    // And back.
    rig.console.switch_to(STREAM, rig.now);
    rig.run(40.0);
    assert!(matches!(rig.console.state, State::Playing(_)));
    assert!(rig.console.heard.chunks_exact(2).rev().take(5 * RATE as usize).all(|pair| pair[1] == 1));
}
