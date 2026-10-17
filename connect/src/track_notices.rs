//! Song changes only; a resume, seek or repeat of the same track is not a change.
use crate::control::State;
use std::{sync::Arc,time::{Duration,Instant}};
#[derive(Default)]
struct Tracker{last:String}
impl Tracker {
    fn update(&mut self,s:&State,now:Instant)->bool {
        let (_,moving)=s.timeline.heard(now);
        if s.uri.is_empty()||!s.streaming||!s.waiting_for.is_empty()||s.loading||s.timeline.pending(now)||!moving||self.last==s.uri{return false;}
        self.last=s.uri.clone();true
    }
}
pub fn start(state:Arc<tokio::sync::watch::Sender<State>>){tokio::spawn(async move{
    let mut tracker=Tracker::default();let mut tick=tokio::time::interval(Duration::from_millis(100));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    loop{tick.tick().await;let snapshot=state.borrow().clone();if tracker.update(&snapshot,Instant::now()){
        // Native music channel, non-immediate, priority 1. Never use the Downloads
        // or ServiceFeedback channels for a song change.
        let payload=crate::toast::song_payload(&snapshot.title,&snapshot.artist,if snapshot.cover.is_empty(){None}else{Some(&snapshot.cover)},std::time::SystemTime::now());
        #[cfg(target_os="freebsd")] {
            extern "C"{fn librespot_song_notification(payload:*const std::ffi::c_char)->i32;}
            if let Ok(text)=std::ffi::CString::new(payload){crate::toast::note_sent(text.as_bytes().len());let code=unsafe{librespot_song_notification(text.as_ptr())};
                if code!=0{log::warn!("Song notification refused: {code:#x}");}}
        }
        #[cfg(not(target_os="freebsd"))] let _=payload;
    }}
});}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn only_a_new_audible_uri_notifies(){
        let now=Instant::now();let mut t=Tracker::default();let mut s=State::default();
        s.uri="spotify:track:A".into();s.streaming=true;s.timeline.record(now,0,true,false);
        assert!(t.update(&s,now));assert!(!t.update(&s,now));
        s.timeline.record(now,12345,false,false);assert!(!t.update(&s,now));
        s.timeline.record(now,12345,true,false);assert!(!t.update(&s,now));
        s.timeline.record(now,80000,true,false);assert!(!t.update(&s,now));
        s.uri="spotify:track:B".into();s.loading=true;assert!(!t.update(&s,now));
        s.loading=false;s.waiting_for="kabuki".into();assert!(!t.update(&s,now));
        s.waiting_for.clear();assert!(t.update(&s,now));
    }
}
