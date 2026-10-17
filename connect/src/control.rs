//! A channel for the console overlay's Now Playing panel (the custom-ps5-overlays project).
//!
//! Listens on 127.0.0.1:38574, the port after the instance lock, so only programs on the
//! console itself can connect. Any number of clients may. Each is sent the player's state as
//! one JSON object per line when it connects, and again whenever it changes (at most ten a
//! second):
//!
//! ```json
//! {"v":1,"state":{"connected":true,"streaming":true,"title":"…","artist":"…","album":"…",
//!  "cover":"/user/data/spotifyps5/covers/….jpg","duration_ms":233000,
//!  "position_ms":81000,"moving":true,"changes":[{"in_ms":1400,"position_ms":96000,"moving":true}],
//!  "playing":true,"pending":true,"loading":false,"shuffle":false,"repeat":"off","volume":72}}
//! ```
//!
//! Everything about the position is what is *heard*. The stream is a delay line: what is
//! heard at any moment is what librespot played `lead` earlier. So `position_ms` and `moving`
//! are what is heard as the line is sent, and `changes` are the changes already played that
//! will be heard in the next `in_ms` (a seek, a pause, a resume, a new track). A client moves
//! the position on by itself while `moving`, and applies each change when its time comes.
//! `playing` is what librespot itself is doing (for the play/pause button), `loading` is
//! true while it fetches a track, and `pending` while something it did is not heard yet.
//!
//! Clients send commands the same way, one per line:
//! `{"v":1,"cmd":"play_pause"}`, `"previous"`, `"next"`, `{"cmd":"seek","ms":96000}` (heard
//! time), `{"cmd":"volume","value":60}` (0 to 100), `{"cmd":"shuffle","on":true}` and
//! `{"cmd":"repeat","mode":"off"|"all"|"one"}`.

use std::sync::Arc;
use std::time::{Duration, Instant};

use tokio::io::{AsyncBufReadExt, AsyncReadExt, AsyncWriteExt, BufReader};
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::{mpsc, watch};

pub const PORT: u16 = 38574;

/// Longest command line read; anything longer ends that client.
const LINE_LIMIT: usize = 4096;
/// The shortest gap between two state lines to one client.
const GAP: Duration = Duration::from_millis(100);

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub enum Repeat {
    #[default]
    Off,
    All,
    One,
}

impl Repeat {
    fn name(self) -> &'static str {
        match self {
            Repeat::Off => "off",
            Repeat::All => "all",
            Repeat::One => "one",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Point {
    at: Instant,
    position_ms: u32,
    moving: bool,
    /// A correction of drift, not something anyone did: not shown as pending.
    quiet: bool,
}

/// What librespot played, change by change, and so what is heard `lead` later.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Timeline {
    pub lead: Duration,
    points: Vec<Point>,
}

impl Timeline {
    pub fn new(lead: Duration) -> Timeline {
        Timeline { lead, points: Vec::new() }
    }

    /// librespot is at `position_ms` at `at`, moving on or not.
    pub fn record(&mut self, at: Instant, position_ms: u32, moving: bool, quiet: bool) {
        self.points.push(Point { at, position_ms, moving, quiet });
        // Keep only the newest point already heard, and those still to be heard.
        if let Some(heard) = self.points.iter().rposition(|p| p.at + self.lead <= at) {
            self.points.drain(..heard);
        }
    }

    /// Whether librespot is playing now (not what is heard).
    pub fn moving(&self) -> bool {
        self.points.last().is_some_and(|p| p.moving)
    }

    /// librespot's own position now (not what is heard).
    pub fn played(&self, now: Instant) -> u32 {
        self.points.last().map_or(0, |p| Self::at(p, now))
    }

    fn at(p: &Point, t: Instant) -> u32 {
        let moved = if p.moving { t.saturating_duration_since(p.at).as_millis().min(u32::MAX as u128) as u32 } else { 0 };
        p.position_ms.saturating_add(moved)
    }

    /// What is heard at `t`: the position, and whether it is moving on.
    pub fn heard(&self, t: Instant) -> (u32, bool) {
        let played = t.checked_sub(self.lead);
        match played.and_then(|played| self.points.iter().rev().find(|p| p.at <= played).map(|p| (p, played))) {
            Some((p, played)) => (Self::at(p, played), p.moving),
            // Before anything was played: what was there first, still.
            None => self.points.first().map_or((0, false), |p| (p.position_ms, false)),
        }
    }

    /// Changes played but not heard by `now`: (ms from now, position, moving).
    pub fn upcoming(&self, now: Instant) -> Vec<(u32, u32, bool)> {
        self.points
            .iter()
            .filter(|p| p.at + self.lead > now)
            .map(|p| ((p.at + self.lead - now).as_millis() as u32, p.position_ms, p.moving))
            .collect()
    }

    /// Something librespot did is not heard yet.
    pub fn pending(&self, now: Instant) -> bool {
        self.points.iter().any(|p| !p.quiet && p.at + self.lead > now)
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct State {
    /// A Spotify app is connected.
    pub connected: bool,
    pub auth_status:String,
    pub auth_retry_seconds:u64,
    pub media_session:u64,
    pub media_generation:u64,
    pub uri:String,
    pub album_uri:String,
    pub context:serde_json::Value,
    pub artists:Vec<serde_json::Value>,
    pub cover_large:String,
    /// The artist's wide header picture, or empty.
    pub background:String,
    pub explicit:bool,
    pub liked:bool,
    pub waiting_for:String,
    /// The console is playing "Librespot Streamer", so what Spotify plays is heard.
    pub streaming: bool,
    /// The heard track.
    pub title: String,
    pub artist: String,
    pub album: String,
    /// A file the console can read, or empty.
    pub cover: String,
    pub duration_ms: u32,
    pub timeline: Timeline,
    /// librespot is fetching a track.
    pub loading: bool,
    pub shuffle: bool,
    pub repeat: Repeat,
    /// Spotify's volume, 0 to 100.
    pub volume: u32,
    /// What plays next, once known (`None` until the connect state has been sent once).
    pub queue: Option<Vec<QueueEntry>>,
}

/// One track of the queue, for the panel's Queue tab.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct QueueEntry {
    pub duration_ms:u32,
    pub explicit:bool,
    /// Spotify URI of this entry, and its position in the full queue.
    pub uri: String,
    pub index: u32,
    pub title: String,
    pub artist: String,
    /// A file the console can read, or empty.
    pub cover: String,
}

impl State {
    pub fn app_line(&self)->String {
        let mut j:serde_json::Value=serde_json::from_str(&self.line()).unwrap_or_default();
        j["v"]=serde_json::json!(2);
        let s=&mut j["state"];
        if let Some(queue)=&self.queue {s["queue"]=serde_json::json!(queue.iter().map(|q|serde_json::json!({"uri":q.uri,"index":q.index,"title":q.title,"artist":q.artist,"cover":q.cover,"duration_ms":q.duration_ms,"explicit":q.explicit})).collect::<Vec<_>>());}
        s["media_session"]=serde_json::json!(self.media_session);s["media_generation"]=serde_json::json!(self.media_generation);
        s["auth"]=serde_json::json!({"status":self.auth_status,"retry_seconds":self.auth_retry_seconds,"storage":librespot::core::cache::credentials_storage_status()});
        s["features"]=serde_json::json!({"lyrics":true,"canvas":true});
        s["uri"]=serde_json::json!(self.uri);s["album_uri"]=serde_json::json!(self.album_uri);
        s["artists"]=serde_json::json!(self.artists);s["context"]=self.context.clone();
        s["cover_large"]=serde_json::json!(self.cover_large);s["background"]=serde_json::json!(self.background);s["explicit"]=serde_json::json!(self.explicit);
        s["liked"]=serde_json::json!(self.liked);s["waiting_for"]=serde_json::json!(self.waiting_for);
        j.to_string()
    }
    pub fn line(&self) -> String {
        self.line_at(Instant::now())
    }

    fn line_at(&self, now: Instant) -> String {
        let (position, moving) = self.timeline.heard(now);
        let changes: Vec<serde_json::Value> = self
            .timeline
            .upcoming(now)
            .into_iter()
            .map(|(in_ms, position_ms, moving)| serde_json::json!({"in_ms": in_ms, "position_ms": position_ms, "moving": moving}))
            .collect();
        let mut line = serde_json::json!({
            "v": 1,
            "state": {
                "connected": self.connected,
                "streaming": self.streaming,
                "title": self.title,
                "artist": self.artist,
                "album": self.album,
                "cover": self.cover,
                "duration_ms": self.duration_ms,
                "position_ms": position,
                "moving": moving,
                "changes": changes,
                "playing": self.timeline.moving(),
                "pending": self.loading || self.timeline.pending(now),
                "loading": self.loading,
                "shuffle": self.shuffle,
                "repeat": self.repeat.name(),
                "volume": self.volume,
            }
        });
        if let Some(queue) = &self.queue {
            line["state"]["queue"] = queue
                .iter()
                .map(|entry| serde_json::json!({"uri": entry.uri, "index": entry.index, "title": entry.title, "artist": entry.artist, "cover": entry.cover}))
                .collect();
        }
        line.to_string()
    }
}

#[derive(Clone, Debug, PartialEq)]
pub enum Command {
    MediaFeature{feature:String,uri:String,client:String,id:u64,session:u64,generation:u64},
    Pause,
    PlayList{context:String},
    PlayTrack{track:String},
    Search{query:String,id:u64},
    Save{uri:String,on:bool},
    PlayPause,
    Previous,
    Next,
    /// Heard time, in milliseconds.
    Seek(u32),
    /// 0 to 100.
    Volume(u32),
    Shuffle(bool),
    Repeat(Repeat),
    /// Play a track from the queue: its URI, and where it was in the queue the panel showed.
    PlayQueued { uri: String, index: u32 },
    /// Send the user's saved playlists and albums (a `library` line).
    Library,
    /// Send a playlist's or album's tracks (a `tracks` line).
    OpenList { uri: String },
    OpenListPage { uri: String, offset: usize, client:String, id:u64 },
    /// Play `track` within the playlist or album `context`.
    Play { context: String, track: String },
}

/// A Spotify URI as the panel may send one: `spotify:` and plain characters only.
fn spotify_uri(text: &str) -> Option<String> {
    let ok = text.starts_with("spotify:")
        && text.len() <= 120
        && text.chars().all(|c| c.is_ascii_alphanumeric() || c == ':' || c == '_' || c == '-');
    ok.then(|| text.to_string())
}

pub fn parse(line: &str) -> Option<Command> {
    let value: serde_json::Value = serde_json::from_str(line).ok()?;
    let version=value.get("v")?.as_u64()?;
    if version==2 {
        match value.get("cmd")?.as_str()? {
            "lyrics"|"canvas"=>{
                let feature=value["cmd"].as_str()?.to_owned();let client=value["client"].as_str()?;
                if client.is_empty()||client.len()>64{return None;}
                return Some(Command::MediaFeature{feature,uri:spotify_uri(value.get("uri")?.as_str()?)?,client:client.into(),
                    id:value["id"].as_u64()?,session:value["session"].as_u64()?,generation:value["generation"].as_u64()?});
            },
            "play"=>{
                for key in ["context","track"]{if let Some(value)=value.get(key){if value.as_str().and_then(spotify_uri).is_none(){return None;}}}
                let context=value.get("context").and_then(|s|s.as_str()).and_then(spotify_uri);
                let track=value.get("track").and_then(|s|s.as_str()).and_then(spotify_uri);
                return match(context,track){(Some(context),Some(track))=>Some(Command::Play{context,track}),
                    (Some(context),None)=>Some(Command::PlayList{context}),(None,Some(track))=>Some(Command::PlayTrack{track}),_=>None};
            },
            "open_list"=>return Some(Command::OpenListPage{uri:spotify_uri(value.get("uri")?.as_str()?)?,offset:value.get("offset").and_then(|v|v.as_u64()).unwrap_or(0).min(20000) as usize,client:value.get("client").and_then(|v|v.as_str()).filter(|s|s.len()<=64).unwrap_or("").into(),id:value.get("id").and_then(|v|v.as_u64()).unwrap_or(0)}),
            "search"=>{let query=value.get("query")?.as_str()?.trim();if query.is_empty()||query.len()>160{return None;}
                return Some(Command::Search{query:query.into(),id:value.get("id")?.as_u64()?});},
            "like"|"save"=>return Some(Command::Save{uri:spotify_uri(value.get("uri")?.as_str()?)?,on:value.get("on")?.as_bool()?}),
            _=>return None
        }
    }
    if version!=1{return None;}
    let number = |key: &str| value.get(key)?.as_u64().map(|n| n.min(u32::MAX as u64) as u32);
    Some(match value.get("cmd")?.as_str()? {
        "play_pause" => Command::PlayPause,
        "previous" => Command::Previous,
        "next" => Command::Next,
        "seek" => Command::Seek(number("ms")?),
        "volume" => Command::Volume(number("value")?.min(100)),
        "shuffle" => Command::Shuffle(value.get("on")?.as_bool()?),
        "repeat" => Command::Repeat(match value.get("mode")?.as_str()? {
            "off" => Repeat::Off,
            "all" => Repeat::All,
            "one" => Repeat::One,
            _ => return None,
        }),
        "play_queued" => Command::PlayQueued {
            uri: spotify_uri(value.get("uri")?.as_str()?)?,
            index: number("index").unwrap_or(0),
        },
        "library" => Command::Library,
        "open_list" => Command::OpenList { uri: spotify_uri(value.get("uri")?.as_str()?)? },
        "play" => Command::Play {
            context: spotify_uri(value.get("context")?.as_str()?)?,
            track: spotify_uri(value.get("track")?.as_str()?)?,
        },
        _ => return None,
    })
}

/// The run loop's side of the channel: the state it publishes, other lines for every client
/// (`library` and `tracks` answers), and the commands it is sent.
pub struct Control {
    pub state: Arc<watch::Sender<State>>,
    pub lines: tokio::sync::broadcast::Sender<String>,
    pub commands: mpsc::UnboundedReceiver<Command>,
    pub sender:mpsc::UnboundedSender<Command>,
}

/// Starts listening. Clients are served by tasks of their own until the runtime stops.
pub async fn listen(lead: Duration) -> std::io::Result<Control> {
    let listener = TcpListener::bind(("127.0.0.1", PORT)).await?;
    Ok(serve(listener, lead))
}

pub fn serve(listener: TcpListener, lead: Duration) -> Control {
    let (state, _) = watch::channel(State { timeline: Timeline::new(lead), ..State::default() });
    let state = Arc::new(state);
    let (sender, commands) = mpsc::unbounded_channel();
    let (lines, _) = tokio::sync::broadcast::channel::<String>(16);
    let command_sender=sender.clone();
    let for_clients = state.clone();
    let lines_for_clients = lines.clone();
    tokio::spawn(async move {
        loop {
            match listener.accept().await {
                Ok((stream, _)) => {
                    log::info!("the overlay panel connected");
                    tokio::spawn(client(stream, for_clients.subscribe(), lines_for_clients.subscribe(), sender.clone()));
                }
                Err(e) => {
                    log::warn!("overlay channel: {e}");
                    tokio::time::sleep(Duration::from_secs(1)).await;
                }
            }
        }
    });
    Control { state, lines, commands, sender:command_sender }
}

async fn client(
    stream: TcpStream,
    mut state: watch::Receiver<State>,
    mut lines: tokio::sync::broadcast::Receiver<String>,
    commands: mpsc::UnboundedSender<Command>,
) {
    let _ = stream.set_nodelay(true);
    let (read, mut write) = stream.into_split();
    let mut read = BufReader::new(read).take(u64::MAX);
    let mut line = String::new();
    let mut send = true;
    loop {
        if send {
            let text = {let s=state.borrow_and_update();s.line()+"\n"+&s.app_line()+"\n"};
            if write.write_all(text.as_bytes()).await.is_err() {
                return;
            }
            send = false;
        }
        tokio::select! {
            changed = state.changed() => {
                if changed.is_err() {
                    return;
                }
                tokio::time::sleep(GAP).await;
                send = true;
            }
            other = lines.recv() => {
                match other {
                    Ok(text) => {
                        if write.write_all((text + "\n").as_bytes()).await.is_err() {
                            return;
                        }
                    }
                    Err(tokio::sync::broadcast::error::RecvError::Lagged(_)) => {}
                    Err(_) => return,
                }
            }
            // read_line keeps what it has read in `line` if the other branch wins, so a
            // line is only cleared once it is complete.
            read_result = async {
                read.set_limit(LINE_LIMIT.saturating_sub(line.len()) as u64);
                read.read_line(&mut line).await
            } => {
                match read_result {
                    Ok(0) | Err(_) => return,
                    Ok(_) if !line.ends_with('\n') => return, // longer than LINE_LIMIT
                    Ok(_) => {
                        match parse(line.trim_end()) {
                            Some(command) => {
                                let _ = commands.send(command);
                            }
                            None => log::warn!("overlay channel: unknown command {:?}", line.trim_end()),
                        }
                        line.clear();
                    }
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const LEAD: Duration = Duration::from_secs(2);

    #[test]
    fn library_commands_are_read() {
        assert_eq!(parse(r#"{"v":1,"cmd":"library"}"#), Some(Command::Library));
        assert_eq!(
            parse(r#"{"v":1,"cmd":"play","context":"spotify:album:2up3OPMp9Tb4dAKM2erWXQ","track":"spotify:track:6rqhFgbbKwnb9MLmUQDhG6"}"#),
            Some(Command::Play {
                context: "spotify:album:2up3OPMp9Tb4dAKM2erWXQ".into(),
                track: "spotify:track:6rqhFgbbKwnb9MLmUQDhG6".into()
            })
        );
        assert_eq!(
            parse(r#"{"v":1,"cmd":"play_queued","uri":"spotify:track:6rqhFgbbKwnb9MLmUQDhG6","index":3}"#),
            Some(Command::PlayQueued { uri: "spotify:track:6rqhFgbbKwnb9MLmUQDhG6".into(), index: 3 })
        );
        for line in [
            r#"{"v":1,"cmd":"open_list","uri":"https://example.com/x"}"#,
            r#"{"v":1,"cmd":"open_list","uri":"spotify:playlist:a/../b"}"#,
            r#"{"v":1,"cmd":"play","context":"spotify:album:x"}"#,
        ] {
            assert_eq!(parse(line), None, "{line}");
        }
    }

    #[test]
    fn commands_are_read() {
        assert_eq!(parse(r#"{"v":1,"cmd":"play_pause"}"#), Some(Command::PlayPause));
        assert_eq!(parse(r#"{"v":1,"cmd":"seek","ms":96000}"#), Some(Command::Seek(96000)));
        assert_eq!(parse(r#"{"v":1,"cmd":"volume","value":250}"#), Some(Command::Volume(100)));
        assert_eq!(parse(r#"{"v":1,"cmd":"repeat","mode":"one"}"#), Some(Command::Repeat(Repeat::One)));
        assert_eq!(parse(r#"{"v":1,"cmd":"shuffle","on":true}"#), Some(Command::Shuffle(true)));
        for line in [r#"{"v":2,"cmd":"next"}"#, r#"{"v":1,"cmd":"seek"}"#, r#"{"v":1,"cmd":"dance"}"#, "next", ""] {
            assert_eq!(parse(line), None, "{line}");
        }
    }

    #[test]
    fn a_resume_is_heard_a_lead_later() {
        let t0 = Instant::now();
        let mut timeline = Timeline::new(LEAD);
        timeline.record(t0, 30_000, false, false); // paused, long since heard
        let later = t0 + Duration::from_secs(10);
        timeline.record(later, 30_000, true, false); // play pressed
        assert!(timeline.moving(), "librespot plays at once");
        assert_eq!(timeline.heard(later + Duration::from_millis(1_500)), (30_000, false), "still silent");
        assert!(timeline.pending(later + Duration::from_millis(1_500)));
        assert_eq!(timeline.heard(later + Duration::from_millis(2_500)), (30_500, true));
        assert!(!timeline.pending(later + Duration::from_millis(2_500)));
    }

    #[test]
    fn a_seek_holds_until_it_is_heard() {
        let t0 = Instant::now();
        let mut timeline = Timeline::new(LEAD);
        timeline.record(t0, 10_000, true, false);
        let seek = t0 + Duration::from_secs(5);
        timeline.record(seek, 90_000, true, false);
        // The old audio is still heard for the lead, then the new position.
        assert_eq!(timeline.heard(seek + Duration::from_secs(1)), (14_000, true));
        assert_eq!(timeline.upcoming(seek + Duration::from_secs(1)), vec![(1_000, 90_000, true)]);
        assert_eq!(timeline.heard(seek + Duration::from_millis(2_100)), (90_100, true));
    }

    #[test]
    fn a_pause_is_heard_a_lead_later_and_corrections_are_quiet() {
        let t0 = Instant::now();
        let mut timeline = Timeline::new(LEAD);
        timeline.record(t0, 0, true, false);
        let pause = t0 + Duration::from_secs(20);
        timeline.record(pause, 20_000, false, false);
        assert!(!timeline.moving());
        assert_eq!(timeline.heard(pause + Duration::from_secs(1)), (19_000, true), "the lead plays out");
        assert_eq!(timeline.heard(pause + Duration::from_secs(5)), (20_000, false));
        let mut quiet = Timeline::new(LEAD);
        quiet.record(t0, 0, true, true);
        assert!(!quiet.pending(t0));
    }

    #[test]
    fn old_points_are_forgotten() {
        let t0 = Instant::now();
        let mut timeline = Timeline::new(LEAD);
        for i in 0..100 {
            timeline.record(t0 + Duration::from_secs(i), i as u32 * 1_000, true, true);
        }
        assert!(timeline.points.len() <= 4, "{}", timeline.points.len());
        assert_eq!(timeline.heard(t0 + Duration::from_secs(99)), (97_000, true));
    }

    #[test]
    fn queue_entries_keep_their_uri_and_original_index() {
        let mut state = State::default();
        state.queue = Some(vec![QueueEntry {
            uri: "spotify:track:test".into(), index: 7,
            title: "Track".into(), artist: "Artist".into(), cover: String::new(),duration_ms:0,explicit:false,
        }]);
        let line: serde_json::Value = serde_json::from_str(&state.line()).unwrap();
        assert_eq!(line["state"]["queue"][0]["uri"], "spotify:track:test");
        assert_eq!(line["state"]["queue"][0]["index"], 7);
    }

    #[tokio::test(flavor = "current_thread")]
    async fn a_client_gets_the_state_and_sends_commands() {
        let listener = TcpListener::bind(("127.0.0.1", 0)).await.unwrap();
        let address = listener.local_addr().unwrap();
        let mut control = serve(listener, LEAD);
        control.state.send_modify(|state| state.title = "Even If You Don't".to_string());

        let mut client = TcpStream::connect(address).await.unwrap();
        let mut first = vec![0u8; 1024];
        let n = client.read(&mut first).await.unwrap();
        let text = String::from_utf8_lossy(&first[..n]);
        assert!(text.ends_with('\n'), "{text}");
        assert!(text.contains(r#""title":"Even If You Don't""#), "{text}");

        client.write_all(b"{\"v\":1,\"cmd\":\"next\"}\n").await.unwrap();
        let command = tokio::time::timeout(Duration::from_secs(2), control.commands.recv()).await.unwrap();
        assert_eq!(command, Some(Command::Next));

        control.state.send_modify(|state| state.timeline.record(Instant::now(), 0, true, false));
        let n = tokio::time::timeout(Duration::from_secs(2), client.read(&mut first)).await.unwrap().unwrap();
        let text = String::from_utf8_lossy(&first[..n]);
        assert!(text.contains(r#""playing":true"#) && text.contains(r#""pending":true"#), "{text}");
    }
}

#[cfg(test)]
mod app_v2_tests {
    use super::*;
    #[test]
    fn optional_play_and_paging_are_v2_only() {
        assert_eq!(parse(r#"{"v":2,"cmd":"open_list","uri":"spotify:album:a","offset":120}"#),Some(Command::OpenListPage{uri:"spotify:album:a".into(),offset:120,client:String::new(),id:0}));
        assert_eq!(parse(r#"{"v":2,"cmd":"play","context":"spotify:album:a"}"#),Some(Command::PlayList{context:"spotify:album:a".into()}));
        assert_eq!(parse(r#"{"v":2,"cmd":"play","track":"spotify:track:a"}"#),Some(Command::PlayTrack{track:"spotify:track:a".into()}));
        assert!(parse(r#"{"v":1,"cmd":"play","context":"spotify:album:a"}"#).is_none());
        assert!(parse(r#"{"v":2,"cmd":"play","context":"spotify:album:a","track":"../../bad"}"#).is_none());
        assert!(parse(r#"{"v":2,"cmd":"open_list","uri":"https://example.com"}"#).is_none());
    }
}
