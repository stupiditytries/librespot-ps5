//! USB transport lifetime, with a clock supplied by the caller for deterministic tests.
pub const PAUSE_MS: u64 = 300_000;
#[derive(Default, Debug)]
pub struct Policy {
    pub wanted: bool,
    pub holding: bool,
    internal_pause: bool,
    paused_at: Option<u64>,
    start_sent: bool,
    stop_sent: bool,
    pub reset_needed: bool,
}
#[derive(Default, Debug, PartialEq)]
pub struct Decision { pub native: Option<i32>, pub resume: bool, pub reset: bool }
impl Policy {
    /// Explicit Play/Pause from Connect or the overlay, never metadata or prefetch.
    pub fn intent(&mut self, play: bool, now: u64) {
        self.wanted = play;
        if play {
            self.paused_at = None;
            self.start_sent = false;
        } else {
            self.paused_at.get_or_insert(now);
            self.holding = false;
        }
    }
    /// Holds Spotify while the console opens the file. Returns whether to pause Spotify.
    pub fn playing(&mut self, ready: bool) -> bool {
        if !self.wanted || !ready {
            self.holding = self.wanted;
            self.internal_pause = true;
            true
        } else { self.holding = false; false }
    }
    pub fn paused(&mut self, _now: u64) {
        // Player events can trail a newer request or a current-track load. Explicit
        // remote/panel/console/terminal-stop hooks own intent, so a stale Paused or
        // Stopped event must never cancel a newer Play or start the idle timer.
        self.internal_pause = false;
    }
    pub fn tick(&mut self, now: u64, state: i32, feed_ready: bool, prepared: bool) -> Decision {
        let mut d = Decision::default();
        if self.stop_sent {
            if state == 3 {
                if self.reset_needed { d.reset = true; self.reset_needed = false; return d; }
                if prepared { self.stop_sent = false; self.start_sent = false; }
            }
            if self.stop_sent { return d; }
        }
        if self.wanted {
            if state == 2 && feed_ready {
                if self.holding { self.holding = false; d.resume = true; }
            } else if !self.start_sent && prepared {
                self.start_sent = true;
                d.native = Some(1);
            }
        } else if self.paused_at.is_some_and(|at| now.saturating_sub(at) >= PAUSE_MS)
            && matches!(state, 1 | 2 | 4 | 5 | 6 | 7 | -3) {
            self.stop_sent = true;
            self.reset_needed = true;
            self.start_sent = false;
            d.native = Some(2);
        }
        d
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn an_old_paused_event_cannot_cancel_a_new_track_play_request() {
        let mut p=Policy::default();p.intent(false,0);p.intent(true,10);p.paused(11);
        assert!(p.wanted);assert_eq!(p.tick(12,0,false,true).native,Some(1));
    }
    #[test] fn no_start_for_discovery_and_prefetch() {
        let mut p=Policy::default();assert_eq!(p.tick(999_999,0,false,true),Decision::default());
    }
    #[test] fn short_pauses_keep_usb_running_and_timer_is_continuous() {
        let mut p=Policy::default();p.intent(true,0);p.intent(false,1_000);
        assert_eq!(p.tick(300_999,2,true,true).native,None);
        p.intent(true,301_000);p.intent(false,400_000);
        assert_eq!(p.tick(699_999,2,true,true).native,None);
        assert_eq!(p.tick(700_000,2,true,true).native,Some(2));
        assert_eq!(p.tick(700_001,2,true,true).native,None);
    }
    #[test] fn internal_pause_does_not_start_idle_timer() {
        let mut p=Policy::default();p.intent(true,0);assert!(p.playing(false));p.paused(1);
        assert_eq!(p.tick(1,0,false,true).native,Some(1));
        assert_eq!(p.tick(400_000,1,false,true).native,None);
        assert!(p.tick(400_001,2,true,true).resume);
        assert!(!p.tick(400_002,2,true,true).resume);
    }
    #[test] fn user_pause_during_startup_cancels_late_resume() {
        let mut p=Policy::default();p.intent(true,0);assert!(p.playing(false));
        p.intent(false,20);p.paused(21);
        assert!(!p.tick(30,2,true,true).resume);
        assert_eq!(p.tick(300_020,2,true,true).native,Some(2));
    }
    #[test] fn play_during_stop_waits_for_confirmed_stop_and_file_reset() {
        let mut p=Policy::default();p.intent(false,0);
        assert_eq!(p.tick(300_000,2,true,true).native,Some(2));
        p.intent(true,300_001);
        assert_eq!(p.tick(300_002,1,false,true).native,None);
        assert!(p.tick(300_003,3,false,true).reset);
        assert_eq!(p.tick(300_004,3,false,false).native,None);
        assert_eq!(p.tick(300_005,3,false,true).native,Some(1));
    }
    #[test] fn paused_service_resumes_on_play_and_stops_after_five_minutes() {
        let mut p=Policy::default();p.intent(false,100);
        assert_eq!(p.tick(300_099,4,false,true).native,None);
        assert_eq!(p.tick(300_100,4,false,true).native,Some(2));
        let mut p=Policy::default();p.intent(false,100);p.intent(true,200);
        assert!(p.playing(false));
        assert_eq!(p.tick(201,4,false,true).native,Some(1));
        assert!(p.tick(202,2,true,true).resume);
    }
    #[test] fn explicit_play_can_retry_an_acknowledged_transport_error() {
        let mut p=Policy::default();p.intent(true,0);
        assert_eq!(p.tick(0,0,false,true).native,Some(1));
        p.intent(false,10);p.intent(true,20);
        assert_eq!(p.tick(21,-3,false,true).native,Some(1));
    }
    #[test] fn failed_start_waits_for_another_explicit_play() {
        let mut p=Policy::default();p.intent(true,0);
        assert_eq!(p.tick(0,0,false,true).native,Some(1));
        assert_eq!(p.tick(50_000,-1,false,true).native,None);
        p.intent(true,50_001);assert_eq!(p.tick(50_002,-1,false,true).native,Some(1));
    }
}
