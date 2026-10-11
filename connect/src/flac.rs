//! FLAC whose frames hold the samples as they are (no compression), 16-bit stereo at
//! 44.1 kHz.
//!
//! The console's USB music player lists and plays these, and because nothing is compressed
//! every frame has a size known in advance: any moment in a file maps to a byte offset, and
//! a later part can be written without touching what comes before it. The part before the
//! first frame is padded to a fixed length, so the tags can be rewritten without moving the
//! audio.

use std::sync::atomic::{AtomicUsize, Ordering};

pub const RATE: u64 = 44_100;
static BLOCK: AtomicUsize = AtomicUsize::new(4096);

/// Sample pairs (one left and one right sample) in a full FLAC frame. The console's player
/// reads a number of frames ahead, so smaller frames may mean it reads less time ahead.
pub fn block() -> usize {
    BLOCK.load(Ordering::Relaxed)
}

/// Chooses the frame size for everything written from now on: 256, 512, 1024, 2048 or
/// 4096 sample pairs. Call before any file is written, and after [`set_format`].
pub fn set_block(pairs: usize) {
    let allowed = matches!(pairs, 256 | 512 | 1024 | 2048 | 4096) && pairs * format().repeat <= 16384;
    BLOCK.store(if allowed { pairs } else { 4096 }, Ordering::Relaxed);
}
/// Offset of the first frame in every file written here.
pub const AUDIO_START: u64 = 8192;

/// How the samples are stored. The audio handed over is always 16-bit at 44.1 kHz; a file
/// can hold each sample several times over and in three bytes, which changes nothing that
/// is heard but makes the file carry more bytes per second. The console's player reads a
/// fixed amount of data ahead, so a fatter file means it reads less time ahead.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Format {
    /// Times each sample is repeated: 1, 2 or 4 (44.1, 88.2 or 176.4 kHz).
    pub repeat: usize,
    /// Bytes per sample: 2 or 3.
    pub bytes: usize,
}

pub const PLAIN: Format = Format { repeat: 1, bytes: 2 };

static FORMAT: AtomicUsize = AtomicUsize::new(0x102);

/// Chooses the format for everything written from now on. Call before any file is
/// written. Values that are not supported are replaced by the plain ones.
pub fn set_format(repeat: usize, bits: usize) {
    let repeat = if matches!(repeat, 1 | 2 | 4) { repeat } else { 1 };
    let bytes = if bits == 24 { 3 } else { 2 };
    FORMAT.store(repeat << 8 | bytes, Ordering::Relaxed);
}

pub fn format() -> Format {
    let packed = FORMAT.load(Ordering::Relaxed);
    Format { repeat: packed >> 8, bytes: packed & 0xff }
}

impl Format {
    /// The frame header's bytes for the block size and rate, and for the sample size.
    fn codes(self, full: bool) -> [u8; 2] {
        // Stored samples per frame: 256 is code 8, and each doubling is one more.
        let stored = block() * self.repeat;
        let block = if full { 8 + (stored / 256).trailing_zeros() as u8 } else { 0x7 };
        let rate = match self.repeat {
            1 => 0x9,
            2 => 0x1,
            _ => 0x2,
        };
        [block << 4 | rate, if self.bytes == 3 { 0x1c } else { 0x18 }]
    }
}

/// Longest tag value kept, in bytes. Three of these fit in the padded header with room over.
const TAG_LIMIT: usize = 600;

#[derive(Clone, Debug, Default, PartialEq)]
pub struct Tags {
    pub title: String,
    pub artist: String,
    pub album: String,
}

const fn table8() -> [u8; 256] {
    let mut table = [0u8; 256];
    let mut value = 0;
    while value < 256 {
        let mut crc = value as u8;
        let mut bit = 0;
        while bit < 8 {
            crc = if crc & 0x80 != 0 { (crc << 1) ^ 0x07 } else { crc << 1 };
            bit += 1;
        }
        table[value] = crc;
        value += 1;
    }
    table
}

const fn table16() -> [u16; 256] {
    let mut table = [0u16; 256];
    let mut value = 0;
    while value < 256 {
        let mut crc = (value as u16) << 8;
        let mut bit = 0;
        while bit < 8 {
            crc = if crc & 0x8000 != 0 { (crc << 1) ^ 0x8005 } else { crc << 1 };
            bit += 1;
        }
        table[value] = crc;
        value += 1;
    }
    table
}

static CRC8: [u8; 256] = table8();
static CRC16: [u16; 256] = table16();

fn crc8(data: &[u8]) -> u8 {
    data.iter().fold(0, |crc, byte| CRC8[(crc ^ byte) as usize])
}

fn crc16(data: &[u8]) -> u16 {
    data.iter()
        .fold(0, |crc, byte| (crc << 8) ^ CRC16[((crc >> 8) as u8 ^ byte) as usize])
}

/// Bytes the frame number takes in the UTF-8 style coding the format uses.
fn number_len(index: u32) -> u64 {
    match index {
        0..=0x7f => 1,
        0x80..=0x7ff => 2,
        0x800..=0xffff => 3,
        0x1_0000..=0x1f_ffff => 4,
        0x20_0000..=0x3ff_ffff => 5,
        _ => 6,
    }
}

fn push_number(out: &mut Vec<u8>, index: u32) {
    match number_len(index) {
        1 => out.push(index as u8),
        2 => out.extend([0xc0 | (index >> 6) as u8, 0x80 | (index & 0x3f) as u8]),
        3 => out.extend([
            0xe0 | (index >> 12) as u8,
            0x80 | ((index >> 6) & 0x3f) as u8,
            0x80 | (index & 0x3f) as u8,
        ]),
        length => {
            // Four, five or six bytes: a first byte with the top bits, then six bits each.
            let first = [0xf0u8, 0xf8, 0xfc][length as usize - 4];
            out.push(first | (index >> (6 * (length - 1))) as u8);
            for position in (0..length - 1).rev() {
                out.push(0x80 | ((index >> (6 * position)) & 0x3f) as u8);
            }
        }
    }
}

/// Size in bytes of full frame number `index`.
pub fn frame_size(index: u32) -> u64 {
    // Sync and description (4), frame number, CRC-8, two subframes of one type byte and
    // block() samples each, CRC-16.
    let format = format();
    4 + number_len(index) + 1 + 2 * (1 + (format.bytes * format.repeat * block()) as u64) + 2
}

/// Offset in the file of frame number `index`, when every frame before it is full.
pub fn frame_offset(index: u32) -> u64 {
    let index = index as u64;
    let between = |low: u64, high: u64| index.clamp(low, high) - low;
    AUDIO_START
        + index * (frame_size(0) - 1)
        + between(0, 0x80)
        + 2 * between(0x80, 0x800)
        + 3 * between(0x800, 0x1_0000)
        + 4 * between(0x1_0000, 0x20_0000)
        + 5 * between(0x20_0000, 0x400_0000)
        + 6 * between(0x400_0000, u64::MAX)
}

/// Appends one frame to `out`. `samples` is interleaved left and right, at most [`block`]
/// pairs. A frame shorter than a full block carries its own length and must be the last
/// one of its file.
pub fn push_frame(out: &mut Vec<u8>, index: u32, samples: &[i16]) {
    push_frame_as(format(), out, index, samples)
}

/// Whether the two bytes look like the start of a frame to the console's player.
fn looks_like_sync(first: u8, second: u8) -> bool {
    first == 0xff && second & 0xfe == 0xf8
}

/// Changes samples by one step where their bytes, as stored in a frame, would otherwise
/// read as the start of a frame.
///
/// Nothing is compressed here, so the samples' own bytes are in the file, and quiet music
/// is full of the values -8 and -7, which are stored as `FF F8` and `FF F9`: the two bytes
/// a frame begins with. The console's player (read from `SystemMusicCore.elf`, the search
/// at 0x4a470) takes the first such pair it finds after looking for its place in a file,
/// remembers the two bytes that follow as what every frame must start with, and only then
/// checks the pair. If the pair was inside the audio, no real frame matches what it
/// remembered; it reads on collecting one enormous "frame" until its memory runs out, and
/// crashes (2026-10-11, on pauses at the console). With no such pair anywhere but at the
/// start of a frame, it cannot go wrong that way. A change of one step is far below hearing.
pub fn make_safe(samples: &mut [i16]) {
    let count = samples.len() / 2;
    for channel in 0..2 {
        for frame in 0..count {
            let mut sample = samples[frame * 2 + channel];
            if sample == -8 || sample == -7 {
                sample = -6;
            }
            // The low byte of one sample followed by the high byte of the next.
            if frame + 1 < count {
                let next_high = (samples[(frame + 1) * 2 + channel] as u16 >> 8) as u8;
                if looks_like_sync(sample as u8, next_high) {
                    sample -= 1;
                }
            }
            samples[frame * 2 + channel] = sample;
        }
    }
}

fn push_frame_as(format: Format, out: &mut Vec<u8>, index: u32, samples: &[i16]) {
    let mut samples = samples.to_vec();
    make_safe(&mut samples);
    let start = out.len();
    for _ in 0..16 {
        out.truncate(start);
        push_frame_raw(format, out, index, &samples);
        // What is left: the last sample's low byte with the first byte of the check value
        // after it, and the check value's own two bytes. Another value for the last
        // sample gives another check value.
        let end = out.len();
        if !looks_like_sync(out[end - 3], out[end - 2]) && !looks_like_sync(out[end - 2], out[end - 1]) {
            break;
        }
        let last = samples.len() - 1;
        samples[last] = samples[last].wrapping_add(3);
        make_safe(&mut samples);
    }
}

fn push_frame_raw(format: Format, out: &mut Vec<u8>, index: u32, samples: &[i16]) {
    let count = samples.len() / 2;
    assert!(count >= 1 && count <= block() && samples.len() % 2 == 0);
    let start = out.len();
    let [block_and_rate, size] = format.codes(count == block());
    out.extend([0xff, 0xf8, block_and_rate, size]);
    push_number(out, index);
    if count != block() {
        out.extend(((count * format.repeat - 1) as u16).to_be_bytes());
    }
    out.push(crc8(&out[start..]));
    for channel in 0..2 {
        out.push(0x02); // verbatim subframe, no wasted bits
        for pair in samples.chunks_exact(2) {
            let bytes = pair[channel].to_be_bytes();
            for _ in 0..format.repeat {
                out.extend(bytes);
                if format.bytes == 3 {
                    out.push(0);
                }
            }
        }
    }
    let crc = crc16(&out[start..]);
    out.extend(crc.to_be_bytes());
}

fn clipped(text: &str) -> &str {
    let mut end = text.len().min(TAG_LIMIT);
    while !text.is_char_boundary(end) {
        end -= 1;
    }
    &text[..end]
}

/// Everything before the first frame: the stream description, the tags, and padding up to
/// [`AUDIO_START`]. `total_samples` is the length the player shows for the file.
pub fn header(total_samples: u64, tags: &Tags) -> Vec<u8> {
    header_as(format(), total_samples, tags)
}

fn header_as(format: Format, total_samples: u64, tags: &Tags) -> Vec<u8> {
    let mut out = Vec::with_capacity(AUDIO_START as usize);
    out.extend(b"fLaC");

    let repeat = format.repeat as u64;
    out.extend([0, 0, 0, 34]); // stream description, 34 bytes
    out.extend(((block() * format.repeat) as u16).to_be_bytes());
    out.extend(((block() * format.repeat) as u16).to_be_bytes());
    out.extend([0; 6]); // smallest and largest frame size: not stated
    // Rate (20 bits), channels - 1 (3), bits per sample - 1 (5), sample count (36).
    let packed = (RATE * repeat << 44)
        | (1 << 41)
        | ((format.bytes as u64 * 8 - 1) << 36)
        | (total_samples * repeat & 0xf_ffff_ffff);
    out.extend(packed.to_be_bytes());
    out.extend([0; 16]); // MD5 of the audio: not stated

    let vendor = b"spotifyps5";
    let mut comments = Vec::new();
    comments.extend((vendor.len() as u32).to_le_bytes());
    comments.extend(vendor);
    let fields = [("TITLE", &tags.title), ("ARTIST", &tags.artist), ("ALBUM", &tags.album)];
    let present = fields.iter().filter(|(_, value)| !value.is_empty());
    comments.extend((present.clone().count() as u32).to_le_bytes());
    for (key, value) in present {
        let value = clipped(value);
        comments.extend(((key.len() + 1 + value.len()) as u32).to_le_bytes());
        comments.extend(key.as_bytes());
        comments.push(b'=');
        comments.extend(value.as_bytes());
    }
    out.push(4);
    out.extend(&(comments.len() as u32).to_be_bytes()[1..]);
    out.extend(comments);

    let padding = AUDIO_START as usize - out.len() - 4;
    out.push(0x80 | 1); // padding, and the last block
    out.extend(&(padding as u32).to_be_bytes()[1..]);
    out.resize(AUDIO_START as usize, 0);
    out
}

/// Reads the frame at the start of `data`: its number, its samples (interleaved) and its
/// size in bytes. `None` if there is no valid frame there, which is what the player meets
/// where a file has not been written yet.
pub fn read_frame(data: &[u8]) -> Option<(u32, Vec<i16>, usize)> {
    let format = format();
    let [full, size] = format.codes(true);
    let [short, _] = format.codes(false);
    if data.len() < 6 || data[0] != 0xff || data[1] != 0xf8 || data[3] != size {
        return None;
    }
    let lead = data[4];
    let extra = match lead {
        0x00..=0x7f => 0,
        0xc0..=0xdf => 1,
        0xe0..=0xef => 2,
        0xf0..=0xf7 => 3,
        0xf8..=0xfb => 4,
        0xfc..=0xfd => 5,
        _ => return None,
    };
    let mut index = match extra {
        0 => lead as u32,
        1 => (lead & 0x1f) as u32,
        2 => (lead & 0x0f) as u32,
        3 => (lead & 0x07) as u32,
        4 => (lead & 0x03) as u32,
        _ => (lead & 0x01) as u32,
    };
    let mut at = 5;
    for _ in 0..extra {
        index = (index << 6) | (*data.get(at)? & 0x3f) as u32;
        at += 1;
    }
    let count = if data[2] == full {
        block()
    } else if data[2] == short {
        let stored = u16::from_be_bytes([*data.get(at)?, *data.get(at + 1)?]) as usize + 1;
        at += 2;
        stored / format.repeat
    } else {
        return None;
    };
    let step = format.bytes * format.repeat;
    if *data.get(at)? != crc8(&data[..at]) {
        return None;
    }
    at += 1;
    let size = at + 2 * (1 + step * count) + 2;
    if data.len() < size {
        return None;
    }
    let mut samples = vec![0i16; count * 2];
    for channel in 0..2 {
        if data[at] != 0x02 {
            return None;
        }
        at += 1;
        for frame in 0..count {
            samples[frame * 2 + channel] = i16::from_be_bytes([data[at], data[at + 1]]);
            at += step;
        }
    }
    if data[at..at + 2] != crc16(&data[..at]).to_be_bytes() {
        return None;
    }
    Some((index, samples, size))
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Random samples, already free of the byte pairs that `make_safe` removes, so that
    /// what is read back can be compared with them.
    fn noise(pairs: usize, seed: u32) -> Vec<i16> {
        let mut state = seed;
        let mut samples: Vec<i16> = (0..pairs * 2)
            .map(|_| {
                state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
                (state >> 16) as i16
            })
            .collect();
        make_safe(&mut samples);
        samples
    }

    /// Quiet music is the hard case: small values either side of zero, among them the
    /// ones whose bytes are those a frame starts with. No frame written may contain those
    /// two bytes anywhere but at its start, and no sample may move by more than a step
    /// or two.
    #[test]
    fn audio_never_looks_like_the_start_of_a_frame() {
        let mut state = 7u32;
        let mut next = || {
            state = state.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
            state >> 16
        };
        for index in 0..400 {
            let samples: Vec<i16> = (0..block() * 2)
                .map(|_| match next() % 4 {
                    0 => -8 + (next() % 3) as i16,       // -8, -7, -6
                    1 => (next() % 512) as i16 - 256,    // near zero
                    2 => -1793 - (next() % 512) as i16,  // high byte F8 or F9
                    _ => (next() as i16) | 0x00ff,        // low byte FF
                })
                .collect();
            let mut frame = Vec::new();
            push_frame(&mut frame, index, &samples);
            let pairs = frame.windows(2).skip(1).filter(|pair| looks_like_sync(pair[0], pair[1])).count();
            assert_eq!(pairs, 0, "frame {index}");
            let (_, read, _) = read_frame(&frame).expect("a valid frame");
            for (written, heard) in samples.iter().zip(&read) {
                assert!((*written as i32 - *heard as i32).abs() <= 4, "{written} became {heard}");
            }
        }
    }

    fn tags(title: &str) -> Tags {
        Tags {
            title: title.to_string(),
            artist: "Somebody".to_string(),
            album: "Something".to_string(),
        }
    }

    #[test]
    fn offsets_match_the_frames_written() {
        let silence = vec![0i16; block() * 2];
        let mut offset = AUDIO_START;
        for index in (0..300).chain(2040..2060).chain(65_530..65_540) {
            if index == 2040 || index == 65_530 {
                offset = frame_offset(index);
            }
            assert_eq!(frame_offset(index), offset, "frame {index}");
            let mut frame = Vec::new();
            push_frame(&mut frame, index, &silence);
            assert_eq!(frame.len() as u64, frame_size(index), "frame {index}");
            offset += frame.len() as u64;
        }
        assert_eq!(frame_size(0), 16394);
        assert_eq!(frame_size(70_000), 16397);
        // Checked against a sum over every frame, into the five-byte frame numbers.
        let total: u64 = (0..2_200_000).map(frame_size).sum();
        assert_eq!(frame_offset(2_200_000), AUDIO_START + total);
        assert_eq!(frame_offset(0x400_0005) - frame_offset(0x400_0004), frame_size(0x400_0004));

    }

    /// Small frames, as the stream may be set to use. Changes a setting the other tests
    /// share, so it is run by itself: cargo test small_frames -- --ignored
    #[test]
    #[ignore]
    fn small_frames_decode() {
        for pairs in [256, 1024] {
            set_block(pairs);
            assert_eq!(block(), pairs);
            decode_with_ffmpeg(PLAIN);
            decode_with_ffmpeg(Format { repeat: 2, bytes: 3 });
        }
        set_block(4096);
    }

    #[test]
    fn frames_read_back() {
        let samples = noise(block(), 1);
        for index in [0, 127, 128, 2047, 2048, 65_535, 65_536, 0x1f_ffff, 0x20_0000, 0x400_0000] {
            let mut frame = Vec::new();
            push_frame(&mut frame, index, &samples);
            let (number, read, size) = read_frame(&frame).expect("a valid frame");
            assert_eq!((number, size), (index, frame.len()));
            assert_eq!(read, samples);
        }
        let short = noise(1000, 2);
        let mut frame = Vec::new();
        push_frame(&mut frame, 9, &short);
        assert_eq!(read_frame(&frame).expect("a valid frame").1, short);
        assert!(read_frame(&vec![0u8; 20_000]).is_none());
        frame[100] ^= 1;
        assert!(read_frame(&frame).is_none());
    }

    #[test]
    fn new_tags_leave_the_audio_where_it_was() {
        let first = header(1_000_000, &tags("Short"));
        let second = header(1_000_000, &tags(&"A much longer title ".repeat(200)));
        assert_eq!(first.len() as u64, AUDIO_START);
        assert_eq!(second.len() as u64, AUDIO_START);
        assert_ne!(first, second);
        // The last block header says "last" and its length runs exactly to the first frame.
        for head in [&first, &second] {
            let mut at = 4;
            loop {
                let last = head[at] & 0x80 != 0;
                let length = u32::from_be_bytes([0, head[at + 1], head[at + 2], head[at + 3]]);
                at += 4 + length as usize;
                if last {
                    break;
                }
            }
            assert_eq!(at as u64, AUDIO_START);
        }
    }

    /// ffmpeg decodes a file written here to exactly the samples that went in, in every
    /// format. Skipped when ffmpeg is not installed.
    #[test]
    fn ffmpeg_decodes_the_same_samples() {
        for (repeat, bytes) in [(1, 2), (2, 2), (2, 3), (4, 3)] {
            decode_with_ffmpeg(Format { repeat, bytes });
        }
    }

    fn decode_with_ffmpeg(format: Format) {
        let pairs = block() * 5 + 777;
        let samples = noise(pairs, 3);
        let mut file = header_as(format, pairs as u64, &tags("Decode test"));
        for (index, block) in samples.chunks(block() * 2).enumerate() {
            push_frame_as(format, &mut file, index as u32, block);
        }
        let name = format!("flac-test-{}-{}{}.flac", std::process::id(), format.repeat, format.bytes);
        let path = std::env::temp_dir().join(name);
        std::fs::write(&path, &file).expect("write the test file");
        let run = |arguments: &[&str]| {
            std::process::Command::new("ffmpeg")
                .args(["-v", "error", "-i"])
                .arg(&path)
                .args(arguments)
                .output()
        };
        let decoded = run(&["-f", "s16le", "-"]);
        let described = run(&["-f", "ffmetadata", "-"]);
        let probed = std::process::Command::new("ffmpeg").arg("-i").arg(&path).output();
        let _ = std::fs::remove_file(&path);
        let (Ok(decoded), Ok(described), Ok(probed)) = (decoded, described, probed) else {
            eprintln!("ffmpeg not found: skipped");
            return;
        };
        assert!(decoded.stderr.is_empty(), "{}", String::from_utf8_lossy(&decoded.stderr));
        let expected: Vec<u8> = samples
            .chunks_exact(2)
            .flat_map(|pair| std::iter::repeat(pair).take(format.repeat))
            .flat_map(|pair| [pair[0].to_le_bytes(), pair[1].to_le_bytes()])
            .flatten()
            .collect();
        assert!(decoded.stdout == expected, "decoded audio differs for {format:?}");
        let rate = format!("{} Hz", RATE as usize * format.repeat);
        let probe = String::from_utf8_lossy(&probed.stderr).into_owned();
        assert!(probe.contains(&rate), "{format:?}: {probe}");
        let described = String::from_utf8_lossy(&described.stdout).to_lowercase();
        assert!(described.contains("title=decode test"), "{described}");
        assert!(described.contains("artist=somebody"), "{described}");
    }
}
