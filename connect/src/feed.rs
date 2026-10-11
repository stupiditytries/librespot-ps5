//! What passes between the receiver and the stream writer: the queue of decoded audio and
//! track changes, and the two small interfaces the writer is built on (the files it writes
//! to, and what the console's log says about its player).

use std::collections::VecDeque;
use std::io;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use crate::flac::{Tags, RATE};

/// The files the stream writes to, by number (see `stream::STREAM` and `stream::STOP`).
pub trait Store: Send + Sync + 'static {
    fn len(&self, file: usize) -> io::Result<u64>;
    fn set_len(&self, file: usize, length: u64) -> io::Result<()>;
    fn read_at(&self, file: usize, offset: u64, buffer: &mut [u8]) -> io::Result<usize>;
    fn write_at(&self, file: usize, offset: u64, data: &[u8]) -> io::Result<()>;
}

/// What the console's log says about its player.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Signal {
    /// The player started something.
    Start,
    /// The player ended or paused something.
    End,
    /// The player's process crashed. It will be started afresh by the console.
    Crashed,
}

#[derive(Clone, Debug, Default)]
pub struct Track {
    pub tags: Tags,
    /// Length in seconds, 0 if not known.
    pub seconds: f64,
    /// Address of the cover picture, empty if there is none.
    pub cover: String,
}


enum Item {
    Samples(Vec<i16>),
    Track(Track),
}

struct FeedState {
    items: VecDeque<Item>,
    /// Sample pairs waiting in `items`.
    queued: usize,
    /// Whether the stream is taking audio. When it is not, audio is thrown away.
    accepting: bool,
    last_track: Option<Track>,
}

/// Samples and track changes on their way to the stream file, in the order they happened.
pub struct Feed {
    state: Mutex<FeedState>,
}

/// What [`Feed::pop_into`] found at the front of the queue.
pub(crate) enum Popped {
    /// Samples were moved into the block.
    Samples,
    /// The samples that follow belong to this track.
    Track(Track),
    Empty,
}

pub enum Offer {
    Taken,
    Full,
    NotPlaying,
}

/// The receiver is held back once this many sample pairs are waiting.
const QUEUE_HIGH: usize = RATE as usize / 2;

impl Feed {
    pub fn new() -> Arc<Feed> {
        Arc::new(Feed {
            state: Mutex::new(FeedState {
                items: VecDeque::new(),
                queued: 0,
                accepting: false,
                last_track: None,
            }),
        })
    }

    /// The samples that follow belong to this track.
    pub fn track(&self, track: Track) {
        let mut state = self.state.lock().unwrap();
        state.last_track = Some(track.clone());
        // An announcement that nothing followed is replaced, so the queue cannot grow
        // while audio is not being taken.
        if matches!(state.items.back(), Some(Item::Track(_))) {
            state.items.pop_back();
        }
        state.items.push_back(Item::Track(track));
    }

    /// Whether audio is waiting to be taken.
    pub(crate) fn has_audio(&self) -> bool {
        self.state.lock().unwrap().queued > 0
    }

    /// Hands over interleaved samples without waiting.
    pub fn offer(&self, samples: &[i16]) -> Offer {
        let mut state = self.state.lock().unwrap();
        if !state.accepting {
            return Offer::NotPlaying;
        }
        if state.queued >= QUEUE_HIGH {
            return Offer::Full;
        }
        state.queued += samples.len() / 2;
        state.items.push_back(Item::Samples(samples.to_vec()));
        Offer::Taken
    }

    /// Hands over interleaved samples, waiting while the queue is full: this is what paces
    /// the receiver. When the stream is not running the samples are dropped after the time
    /// they would have taken to play, so the receiver keeps its normal pace.
    pub fn push(&self, samples: &[i16], pause: &dyn Fn(Duration)) {
        loop {
            match self.offer(samples) {
                Offer::Taken => return,
                Offer::Full => pause(Duration::from_millis(5)),
                Offer::NotPlaying => {
                    pause(Duration::from_micros(samples.len() as u64 / 2 * 1_000_000 / RATE));
                    return;
                }
            }
        }
    }

    pub fn accepting(&self) -> bool {
        self.state.lock().unwrap().accepting
    }

    pub(crate) fn set_accepting(&self, accepting: bool) {
        self.state.lock().unwrap().accepting = accepting;
    }

    /// Moves up to `room` sample values from the front of the queue into `block`, or takes
    /// a track change off it.
    pub(crate) fn pop_into(&self, block: &mut Vec<i16>, room: usize) -> Popped {
        let mut state = self.state.lock().unwrap();
        match state.items.front_mut() {
            None => Popped::Empty,
            Some(Item::Track(_)) => {
                let Some(Item::Track(track)) = state.items.pop_front() else { unreachable!() };
                Popped::Track(track)
            }
            Some(Item::Samples(samples)) => {
                let count = room.min(samples.len());
                block.extend(samples.drain(..count));
                if samples.is_empty() {
                    state.items.pop_front();
                }
                state.queued = state.queued.saturating_sub(count / 2);
                Popped::Samples
            }
        }
    }

    /// Forgets queued audio. The track last announced is announced again, so audio that
    /// arrives later still gets its title.
    pub(crate) fn reset(&self) {
        let mut state = self.state.lock().unwrap();
        state.items.clear();
        state.queued = 0;
        if let Some(track) = state.last_track.clone() {
            state.items.push_back(Item::Track(track));
        }
    }
}
