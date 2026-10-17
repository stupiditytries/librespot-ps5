//! The console's "track change" notification: a small panel that slides in, also over a
//! game, with a picture and two lines of text. The console's own shell shows one when its
//! music changes track; this builds the same description, which the app hands to
//! `sceNotificationSend`.
//!
//! The Music card cannot show a cover for music played from a USB drive, so this is where
//! the cover goes.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicU8, Ordering};
use std::sync::Mutex;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

/// Which kind of notification is sent:
/// 0, the console's own "music track change" kind, as its shell sends it. It was seen on
///    the home screen but not over a game (seen 2026-10-11).
/// 1, the same, marked immediate and with the highest priority.
///    Also not shown over a game.
/// 2, the general-purpose kind other homebrew on the console uses (a "downloads" notice),
///    at priority 2, one step below the 100 it was first tried with.
/// 3, the same shape on the console's "service feedback" channel, which its settings for
///    pop-ups and do-not-disturb do not hold back.
static STYLE: AtomicU8 = AtomicU8::new(0);

pub fn set_style(style: u8) {
    STYLE.store(style.min(3), Ordering::Relaxed);
}

/// The kind used for messages about the stream itself (ready, started, stopped, "play
/// Librespot Streamer"). Not 0: the console puts up its own music notice at every start
/// and pause of its player, on the same channel, and on 2026-10-14 that took the place of
/// ours about half the time.
static STATUS_STYLE: AtomicU8 = AtomicU8::new(2);

pub fn set_status_style(style: u8) {
    STATUS_STYLE.store(style.min(3), Ordering::Relaxed);
}

/// Sizes of the notifications this program sent lately. The console's log has a line for
/// every notification, ours included, with only its size; these are left out when the
/// console's own notices are looked for.
static SENT: Mutex<VecDeque<(Instant, usize)>> = Mutex::new(VecDeque::new());
const SENT_KEPT: Duration = Duration::from_secs(10);
/// The console logs a notification as somewhat longer than the text handed to it (37 and
/// 38 bytes longer on 2026-10-14).
const LOGGED_EXTRA: std::ops::RangeInclusive<usize> = 20..=60;

/// Notes that a notification of `length` bytes was handed to the console.
pub fn note_sent(length: usize) {
    let mut sent = SENT.lock().unwrap();
    while sent.front().is_some_and(|(at, _)| at.elapsed() > SENT_KEPT) {
        sent.pop_front();
    }
    sent.push_back((Instant::now(), length));
}

/// Whether a notification the console's log reports as `logged` bytes is one of ours.
pub fn sent_by_us(logged: usize) -> bool {
    SENT.lock().unwrap().iter().any(|&(at, length)| {
        at.elapsed() <= SENT_KEPT && LOGGED_EXTRA.contains(&logged.saturating_sub(length)) && logged > length
    })
}

fn escaped(text: &str) -> String {
    let mut out = String::with_capacity(text.len() + 2);
    for character in text.chars() {
        match character {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

/// A time as the console wants it: UTC, `2026-10-11T12:34:56.000Z`.
fn timestamp(time: SystemTime) -> String {
    let since = time.duration_since(UNIX_EPOCH).unwrap_or_default();
    let seconds = since.as_secs();
    let (days, rest) = (seconds / 86_400, seconds % 86_400);
    // Days since 1970 to a date in the Gregorian calendar (Howard Hinnant's method).
    let shifted = days as i64 + 719_468;
    let era = shifted.div_euclid(146_097);
    let day_of_era = shifted.rem_euclid(146_097);
    let year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36_524 - day_of_era / 146_096) / 365;
    let day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    let month_index = (5 * day_of_year + 2) / 153;
    let day = day_of_year - (153 * month_index + 2) / 5 + 1;
    let month = if month_index < 10 { month_index + 3 } else { month_index - 9 };
    let year = year_of_era + era * 400 + i64::from(month <= 2);
    format!(
        "{year:04}-{month:02}-{day:02}T{:02}:{:02}:{:02}.{:03}Z",
        rest / 3600,
        rest % 3600 / 60,
        rest % 60,
        since.subsec_millis()
    )
}

/// The notification for a track: `icon` is the path of a picture file on the console, if
/// there is one. The template, channel and use case are the ones the console's shell uses
/// for its own music; a later one replaces an earlier one, so quick skips do not pile up.
pub fn song_payload(title:&str,artist:&str,icon:Option<&str>,now:SystemTime)->String{styled(0,title,artist,icon,now)}

pub fn payload(title: &str, artist: &str, icon: Option<&str>, now: SystemTime) -> String {
    styled(STYLE.load(Ordering::Relaxed), title, artist, icon, now)
}

/// A message about the stream (see `STATUS_STYLE`).
pub fn status_payload(first: &str, second: &str, now: SystemTime) -> String {
    styled(STATUS_STYLE.load(Ordering::Relaxed), first, second, None, now)
}

fn styled(style: u8, title: &str, artist: &str, icon: Option<&str>, now: SystemTime) -> String {
    let icon = match icon {
        Some(path) => format!(
            "\"icon\":{{\"type\":\"Url\",\"parameters\":{{\"url\":\"{}\"}}}},",
            escaped(path)
        ),
        None => String::new(),
    };
    let id = now.duration_since(UNIX_EPOCH).unwrap_or_default().as_millis();
    let kind = match style {
        0 => concat!(
            "\"viewTemplateType\":\"ToastTemplateA\",\"toastOverwriteType\":\"Always\",",
            "\"channelType\":\"MusicTrackChange\",\"useCaseId\":\"NUC59\",",
            "\"isImmediate\":false,\"priority\":1,"
        ),
        1 => concat!(
            "\"viewTemplateType\":\"ToastTemplateA\",\"toastOverwriteType\":\"Always\",",
            "\"channelType\":\"MusicTrackChange\",\"useCaseId\":\"NUC59\",",
            "\"isImmediate\":true,\"priority\":100,"
        ),
        2 => concat!(
            "\"viewTemplateType\":\"InteractiveToastTemplateB\",\"toastOverwriteType\":\"Always\",",
            "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\",",
            "\"isImmediate\":true,\"priority\":2,"
        ),
        _ => concat!(
            "\"viewTemplateType\":\"InteractiveToastTemplateB\",\"toastOverwriteType\":\"Always\",",
            "\"channelType\":\"ServiceFeedback\",\"useCaseId\":\"IDC\",",
            "\"isImmediate\":true,\"priority\":2,"
        ),
    };
    format!(
        concat!(
            "{{\"rawData\":{{{}",
            "\"viewData\":{{{}\"message\":{{\"body\":\"{}\"}},\"subMessage\":{{\"body\":\"{}\"}}}}",
            "}},\"createdDateTime\":\"{}\",\"localNotificationId\":\"{}\"}}"
        ),
        kind,
        icon,
        escaped(title),
        escaped(artist),
        timestamp(now),
        id
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::Duration;

    #[test]
    fn dates() {
        let at = |seconds: u64| timestamp(UNIX_EPOCH + Duration::from_millis(seconds * 1000 + 7));
        assert_eq!(at(0), "1970-01-01T00:00:00.007Z");
        assert_eq!(at(951_782_400), "2000-02-29T00:00:00.007Z");
        assert_eq!(at(1_791_722_096), "2026-10-11T12:34:56.007Z");
        assert_eq!(at(1_798_761_599), "2026-12-31T23:59:59.007Z");
    }

    #[test]
    fn text_is_escaped() {
        let text = payload("Say \"Hi\"\\", "A\nB", Some("/user/data/x.jpg"), UNIX_EPOCH);
        assert!(text.contains(r#""message":{"body":"Say \"Hi\"\\"}"#), "{text}");
        assert!(text.contains(r#""subMessage":{"body":"A\nB"}"#), "{text}");
        assert!(text.contains(r#""icon":{"type":"Url","parameters":{"url":"/user/data/x.jpg"}},"#));
        assert!(text.starts_with(r#"{"rawData":{"viewTemplateType":"ToastTemplateA","#));
        assert!(text.ends_with(r#""createdDateTime":"1970-01-01T00:00:00.000Z","localNotificationId":"0"}"#));
        assert_eq!(text.matches('{').count(), text.matches('}').count());
        assert!(!payload("T", "A", None, UNIX_EPOCH).contains("icon"));
    }
}
