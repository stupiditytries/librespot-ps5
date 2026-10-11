//! The console's "track change" notification: a small panel that slides in, also over a
//! game, with a picture and two lines of text. The console's own shell shows one when its
//! music changes track; this builds the same description, which the app hands to
//! `sceNotificationSend`.
//!
//! The Music card cannot show a cover for music played from a USB drive, so this is where
//! the cover goes.

use std::sync::atomic::{AtomicU8, Ordering};
use std::time::{SystemTime, UNIX_EPOCH};

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
pub fn payload(title: &str, artist: &str, icon: Option<&str>, now: SystemTime) -> String {
    let icon = match icon {
        Some(path) => format!(
            "\"icon\":{{\"type\":\"Url\",\"parameters\":{{\"url\":\"{}\"}}}},",
            escaped(path)
        ),
        None => String::new(),
    };
    let id = now.duration_since(UNIX_EPOCH).unwrap_or_default().as_millis();
    let kind = match STYLE.load(Ordering::Relaxed) {
        0 => concat!(
            "\"viewTemplateType\":\"ToastTemplateA\",\"toastOverwriteType\":\"Always\",",
            "\"channelType\":\"MusicTrackChange\",\"useCaseId\":\"NUC59\",",
            "\"isImmediate\":false,\"priority\":2,"
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
