#![warn(missing_docs)]
#![doc=include_str!("../README.md")]

#[macro_use]
extern crate log;

use librespot_core as core;
use librespot_playback as playback;
use librespot_protocol as protocol;

mod context_resolver;
mod model;
mod shuffle_vec;
mod spirc;
mod state;

pub use model::*;
pub use spirc::*;
pub use state::*;

// librespot-ps5 addition: the queue (the next tracks' URIs) and the context they come from,
// each time the connect state is sent. librespot keeps both inside Spirc otherwise.
/// A next track and whether Spotify explicitly queued it outside the current context.
#[derive(Clone, Debug)]
pub struct NextTrack {
    /// Spotify URI.
    pub uri: String,
    /// This track came from the user's queue rather than the context.
    pub queued: bool,
}
type NextTracksHandler = Box<dyn Fn(String, Vec<NextTrack>) + Send + Sync>;
static NEXT_TRACKS: std::sync::OnceLock<NextTracksHandler> = std::sync::OnceLock::new();

/// Calls `handler` with the context's URI and the URIs of the tracks that play next (in
/// order, without the delimiters between context pages) whenever the connect state is sent.
/// Set once.
pub fn on_next_tracks(handler: impl Fn(String, Vec<NextTrack>) + Send + Sync + 'static) {
    let _ = NEXT_TRACKS.set(Box::new(handler));
}

type ContextPlayedHandler=Box<dyn Fn(&str)+Send+Sync>;
static CONTEXT_PLAYED:std::sync::OnceLock<ContextPlayedHandler>=std::sync::OnceLock::new();
/// Reports a matched current-track Playing event with its album/playlist context.
/// Metadata, queue updates, prefetch and position corrections do not report a play.
pub fn on_context_played(handler:impl Fn(&str)+Send+Sync+'static){let _=CONTEXT_PLAYED.set(Box::new(handler));}
pub(crate) fn report_context_played(uri:&str){if let Some(handler)=CONTEXT_PLAYED.get(){handler(uri);}}

type PlaybackGate=Box<dyn Fn()->bool+Send+Sync>;
static PLAYBACK_GATE:std::sync::OnceLock<PlaybackGate>=std::sync::OnceLock::new();
/// A host may reject playback before a load or resume reaches the player.
pub fn on_playback_gate(gate:impl Fn()->bool+Send+Sync+'static){let _=PLAYBACK_GATE.set(Box::new(gate));}
pub(crate) fn playback_allowed()->bool{PLAYBACK_GATE.get().map_or(true,|gate|gate())}

type PlaybackIntentHandler = Box<dyn Fn(bool) + Send + Sync>;
static PLAYBACK_INTENT: std::sync::OnceLock<PlaybackIntentHandler> = std::sync::OnceLock::new();
/// Reports explicit remote playback requests and current-track loads, excluding prefetch.
/// Internal output-readiness pauses sent with `Spirc::pause` are intentionally excluded.
pub fn on_playback_intent(handler: impl Fn(bool) + Send + Sync + 'static) {
    let _ = PLAYBACK_INTENT.set(Box::new(handler));
}
pub(crate) fn report_playback_intent(playing: bool) {
    if let Some(handler) = PLAYBACK_INTENT.get() { handler(playing); }
}

pub(crate) fn report_next_tracks(context: &str, tracks: &[protocol::player::ProvidedTrack]) {
    if let Some(handler) = NEXT_TRACKS.get() {
        handler(
            context.to_string(),
            tracks
                .iter()
                .filter(|track| !track.uri.is_empty() && track.uri != "spotify:delimiter")
                .map(|track| NextTrack {
                    uri: track.uri.clone(),
                    queued: track.provider == "queue" || track.metadata.get("is_queued").is_some_and(|v| v == "true"),
                })
                .collect(),
        );
    }
}
