//! Decoded PCM -> stateful 44.1/48 kHz resampling -> the native ShellUI transport.
use std::collections::VecDeque;
use librespot::playback::{audio_backend::{Sink as AudioSink, SinkError, SinkResult},
    convert::Converter, decoder::AudioPacket};

const TAPS: usize = 32;
const HALF: i64 = (TAPS / 2) as i64;
pub struct Resampler {
    coefficients: Vec<[f64; TAPS]>,
    frames: VecDeque<[f64; 2]>,
    first: i64,
    next: u64,
}
impl Default for Resampler {
    fn default() -> Self {
        let mut coefficients = Vec::with_capacity(160);
        for phase in 0..160 {
            let fraction = phase as f64 / 160.0;
            let mut c = [0.0; TAPS];
            for (tap, weight) in c.iter_mut().enumerate() {
                let x = tap as f64 - HALF as f64 + 1.0 - fraction;
                let a = std::f64::consts::PI * x;
                let sinc = if a.abs() < 1e-12 { 0.94 } else { (0.94 * a).sin() / a };
                let window = 0.42 + 0.5 * (a / HALF as f64).cos()
                    + 0.08 * (2.0 * a / HALF as f64).cos();
                *weight = sinc * window;
            }
            let sum: f64 = c.iter().sum();
            for weight in &mut c { *weight /= sum; }
            coefficients.push(c);
        }
        let mut value = Self { coefficients, frames: VecDeque::new(), first: 0, next: 0 };
        value.reset();
        value
    }
}
impl Resampler {
    fn reset(&mut self) {
        self.frames.clear();
        self.frames.extend(std::iter::repeat_n([0.0; 2], HALF as usize));
        self.first = -HALF;
        self.next = 0;
    }
    fn push(&mut self, samples: &[f64]) -> Vec<f64> {
        self.frames.extend(samples.chunks_exact(2).map(|s| [s[0], s[1]]));
        let mut output = Vec::with_capacity(samples.len() * 160 / 147 + 4);
        loop {
            let centre = (self.next / 160) as i64;
            if centre + HALF >= self.first + self.frames.len() as i64 { break; }
            let c = &self.coefficients[(self.next % 160) as usize];
            let mut pair = [0.0; 2];
            for (tap, weight) in c.iter().enumerate() {
                let index = (centre - HALF + 1 + tap as i64 - self.first) as usize;
                for (channel, value) in pair.iter_mut().enumerate() {
                    *value += self.frames[index][channel] * weight;
                }
            }
            output.extend_from_slice(&pair);
            self.next += 147;
            let keep = (self.next / 160) as i64 - HALF + 1;
            while self.first < keep { self.frames.pop_front(); self.first += 1; }
        }
        output
    }
}

#[cfg(target_os="freebsd")]
extern "C" {
    fn pcm_transport_gate(playing: i32);
    fn pcm_transport_flush();
    fn pcm_transport_intent(playing: i32);
    fn pcm_transport_generation() -> u64;
    fn pcm_transport_write(samples: *const i16, count: usize, generation: u64) -> i32;
}
pub fn intent(playing: bool) {
    #[cfg(target_os="freebsd")] unsafe { pcm_transport_intent(i32::from(playing)); }
    gate(playing);
}
/// Failure, or paused at the console (7) while Spotify plays. Our own pause
/// reports 4, which arrives late and must not pause a newer Play.
pub fn pause_for_transition(previous: i32, current: i32, wanted: bool) -> bool {
    current != previous && (current < 0 || current == 7) && wanted
}
/// The Service was resumed from the console while Spotify was paused (state 6).
pub fn resume_for_transition(previous: i32, current: i32, wanted: bool) -> bool {
    current != previous && current == 6 && !wanted
}
/// After a seek: audio queued for the old position is discarded.
pub fn flush() {
    #[cfg(target_os="freebsd")] unsafe { pcm_transport_flush(); }
}
pub fn gate(playing: bool) {
    #[cfg(target_os="freebsd")] unsafe { pcm_transport_gate(i32::from(playing)); }
    #[cfg(not(target_os="freebsd"))] let _ = playing;
}
fn generation() -> u64 {
    #[cfg(target_os="freebsd")] unsafe { return pcm_transport_generation(); }
    #[cfg(not(target_os="freebsd"))] { 1 }
}
#[derive(Default)]
pub struct Sink { resampler: Resampler, started: bool, generation: u64 }
impl AudioSink for Sink {
    fn start(&mut self) -> SinkResult<()> {
        if !self.started {
            self.started = true;
            gate(crate::transport::wanted());
        }
        Ok(())
    }
    fn stop(&mut self) -> SinkResult<()> {
        self.started = false;
        gate(false);
        self.resampler.reset();
        Ok(())
    }
    fn write(&mut self, packet: AudioPacket, converter: &mut Converter) -> SinkResult<()> {
        self.start()?;
        let current = generation();
        if self.generation != current { self.resampler.reset(); self.generation = current; }
        let samples = packet.samples().map_err(|e| SinkError::OnWrite(e.to_string()))?;
        let output = self.resampler.push(samples);
        let pcm = converter.f64_to_s16(&output);
        #[cfg(target_os="freebsd")]
        let code = unsafe { pcm_transport_write(pcm.as_ptr(), pcm.len(), current) };
        #[cfg(not(target_os="freebsd"))]
        let code = { let _ = pcm; 0 };
        if code == 0 { Ok(()) } else { Err(SinkError::OnWrite(format!("ShellUI PCM transport returned {code}"))) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn a_reported_failure_does_not_cancel_a_new_explicit_play() {
        assert!(pause_for_transition(2,-3,true));
        assert!(!pause_for_transition(-3,-3,true));
        assert!(!pause_for_transition(-3,1,true));
        assert!(pause_for_transition(2,7,true));
        assert!(!pause_for_transition(2,7,false));
        assert!(!pause_for_transition(5,4,true)); // late report of our own pause
    }
    #[test]
    fn a_console_resume_resumes_spotify_once() {
        assert!(resume_for_transition(4,6,false));
        assert!(!resume_for_transition(6,6,false));
        assert!(!resume_for_transition(4,6,true));
        assert!(!pause_for_transition(4,6,true));
    }
    fn signal(frames: usize) -> Vec<f64> {
        (0..frames).flat_map(|i| {
            let v = (i as f64 * 1000.0 * std::f64::consts::TAU / 44100.0).sin() * 0.5;
            [v, -v]
        }).collect()
    }
    #[test]
    fn resampling_is_independent_of_packet_boundaries() {
        let input = signal(44100);
        let whole = Resampler::default().push(&input);
        let mut r = Resampler::default();
        let mut split = Vec::new();
        for block in input.chunks(358) { split.extend(r.push(block)); }
        assert_eq!(split, whole);
        assert!((47975..=48000).contains(&(whole.len()/2)));
    }
    #[test]
    fn stereo_pitch_and_level_survive_resampling() {
        let out = Resampler::default().push(&signal(44100));
        let mut error = 0.0;
        for (i, frame) in out.chunks_exact(2).enumerate().skip(100) {
            let wanted = (i as f64 * 1000.0 * std::f64::consts::TAU / 48000.0).sin() * 0.5;
            error += (frame[0]-wanted).powi(2);
            assert!((frame[0]+frame[1]).abs() < 1e-12);
        }
        assert!((error/(out.len()/2-100) as f64).sqrt() < 0.001);
    }
    #[test]
    fn reset_discards_history_and_dc_gain_is_unity() {
        let mut r = Resampler::default();
        let dc = r.push(&vec![0.5; 2000]);
        assert!(dc.iter().skip(100).all(|x| (x-0.5).abs() < 1e-10));
        r.reset();
        assert!(r.push(&vec![0.0; 2000]).iter().all(|&x| x == 0.0));
        assert!(r.frames.len() <= TAPS);
    }
}
