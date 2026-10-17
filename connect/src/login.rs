//! Typed reconnect decisions; only explicit AP credential rejection requires pairing.
use librespot::core::{Error,ApAuthenticationError as AuthenticationError};
use librespot::protocol::keyexchange::ErrorCode;
pub fn needs_pairing(error:&Error)->bool{
    let mut cause:Option<&(dyn std::error::Error+'static)>=Some(error.error.as_ref());
    while let Some(e)=cause{
        if let Some(AuthenticationError::LoginFailed(code))=e.downcast_ref::<AuthenticationError>(){
            return matches!(code,ErrorCode::BadCredentials|ErrorCode::ExtraVerificationRequired|ErrorCode::PremiumAccountRequired|ErrorCode::TravelRestriction|ErrorCode::InvalidAppKey|ErrorCode::ApplicationBanned);
        }cause=e.source();
    }false
}
#[cfg(test)]mod tests{use super::*;
#[test]fn temporary_errors_keep_saved_login(){assert!(!needs_pairing(&Error::unavailable("offline")));assert!(!needs_pairing(&Error::permission_denied("login5")));let e=AuthenticationError::LoginFailed(ErrorCode::TryAnotherAP).into();assert!(!needs_pairing(&e));}
#[test]fn explicit_ap_rejection_stops_retry(){let e=AuthenticationError::LoginFailed(ErrorCode::BadCredentials).into();assert!(needs_pairing(&e));}
}
