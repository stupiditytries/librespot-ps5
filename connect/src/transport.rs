//! The receiver's optional native USB worker. No native calls from Tokio's event loop.
use std::sync::{Mutex, OnceLock};
use std::time::Instant;
use crate::usb_policy::{Decision, Policy};
static POLICY: OnceLock<Mutex<Policy>> = OnceLock::new();
pub fn now() -> u64 {
    static BEGAN: OnceLock<Instant> = OnceLock::new();
    BEGAN.get_or_init(Instant::now).elapsed().as_millis() as u64
}
fn policy() -> &'static Mutex<Policy> { POLICY.get_or_init(|| Mutex::new(Policy::default())) }
pub fn intent(play: bool) {
    #[cfg(feature="kabuki-provider")]
    if play&&!crate::kabuki::ready(){return;}
    policy().lock().unwrap().intent(play,now());
    #[cfg(feature="shell-pcm")] crate::pcm::intent(play); }
pub fn wanted() -> bool { policy().lock().unwrap().wanted }
pub fn holding() -> bool { policy().lock().unwrap().holding }
pub fn playing(ready: bool) -> bool { policy().lock().unwrap().playing(ready) }
pub fn paused() { policy().lock().unwrap().paused(now()); }
pub fn tick(ready: bool, prepared: bool) -> Decision {
    policy().lock().unwrap().tick(now(),state(),ready,prepared)
}
#[cfg(target_os="freebsd")]
extern "C" { fn usb_transport_request(action: i32); fn usb_transport_state() -> i32; }
pub fn state() -> i32 {
    #[cfg(target_os="freebsd")] unsafe { return usb_transport_state(); }
    #[cfg(not(target_os="freebsd"))] { 0 }
}
pub fn request(action: i32) {
    log::info!("USB transport: {}",if action==1 {"ensure Streamer playing"} else {"stop Streamer after five minutes paused"});
    #[cfg(target_os="freebsd")] unsafe { usb_transport_request(action); }
}
