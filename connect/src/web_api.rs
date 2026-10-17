//! Shared Web API cache and cooldown. A 429 on one endpoint holds all Web API calls.
use std::collections::HashMap;
use std::sync::OnceLock;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use http_body_util::BodyExt;
use librespot::core::session::Session;
use serde_json::Value;
use tokio::sync::Mutex;

const SCOPES: &str = "playlist-read-private,playlist-read-collaborative,user-library-read";
const COOLDOWN_FILE: &str = "/data/spotifyps5/web-api-cooldown.json";
const CACHE_SECONDS: u64 = 300;
#[derive(Default)]
struct State {
    cache: HashMap<String, (u64, Value)>,
    failures: HashMap<String, (u64, String)>,
    until: u64,
    quota: bool,
    client_service:bool,
}
fn seconds() -> u64 { SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs() }
fn remaining(until: u64, now: u64) -> u64 { until.saturating_sub(now) }
fn retry_seconds(header: Option<&str>, quota: bool) -> u64 {
    // Spotify documents delta-seconds. Never shorten a server-supplied wait.
    header.and_then(|s| s.trim().parse::<u64>().ok()).filter(|&n| n>0)
        .unwrap_or(if quota {3600} else {60})
}
fn key(account: &str, path: &str) -> String { format!("{account}\n{path}") }
fn state() -> &'static Mutex<State> {
    static STATE: OnceLock<Mutex<State>> = OnceLock::new();
    STATE.get_or_init(|| {
        let saved: Value=std::fs::read(COOLDOWN_FILE).ok().and_then(|b|serde_json::from_slice(&b).ok()).unwrap_or(Value::Null);
        Mutex::new(State { until:saved["until"].as_u64().unwrap_or(0),quota:saved["quota"].as_bool().unwrap_or(false),..State::default() })
    })
}
fn blocked(state: &State, now: u64) -> String {
    let seconds=remaining(state.until,now);
    let service=if state.client_service{"Spotify client service"}else{"Spotify Web API"};
    if state.quota {format!("{service} client quota exceeded; retry in {seconds}s. Playback is available.")}
    else {format!("{service} rate limited; retry in {seconds}s. Playback is available.")}
}
/// One in-flight Web API request. Simultaneous panel refreshes share the result cache.
pub async fn get(session: &Session, path: &str) -> Result<Value,String> {
    get_with(state(),key(&session.username(),path),Some(COOLDOWN_FILE),||fetch(session,path)).await
}
pub(crate) async fn guarded<F,Fut>(session:&Session,key_text:&str,fetch:F)->Result<Value,String>
where F:FnOnce()->Fut,Fut:std::future::Future<Output=Result<(u16,Option<String>,bytes::Bytes),String>> {
    get_with(state(),key(&session.username(),key_text),Some(COOLDOWN_FILE),fetch).await
}
// Quotas belong to their service. Never rotate identity or fall back to the
// public Web API after a client-service failure.
pub(crate) async fn client_guarded<F,Fut>(session:&Session,key_text:&str,fetch:F)->Result<Value,String>
where F:FnOnce()->Fut,Fut:std::future::Future<Output=Result<(u16,Option<String>,bytes::Bytes),String>>{
    const FILE:&str="/data/spotifyps5/client-api-cooldown.json";
    static GATE:OnceLock<Mutex<State>>=OnceLock::new();
    let gate=GATE.get_or_init(||{let saved:Value=std::fs::read(FILE).ok().and_then(|b|serde_json::from_slice(&b).ok()).unwrap_or(Value::Null);
        Mutex::new(State{client_service:true,until:saved["until"].as_u64().unwrap_or(0),quota:saved["quota"].as_bool().unwrap_or(false),..Default::default()})});
    get_with(gate,key(&session.username(),key_text),Some(FILE),fetch).await.map_err(|e|e.replace("Spotify Web API","Spotify client service"))
}
async fn get_with<F,Fut>(gate: &Mutex<State>,key: String,cooldown_file: Option<&str>,fetch: F)->Result<Value,String>
where F:FnOnce()->Fut,Fut:std::future::Future<Output=Result<(u16,Option<String>,bytes::Bytes),String>> {
    let mut state=gate.lock().await;
    let now=seconds();
    if let Some((at,value))=state.cache.get(&key) {
        if now.saturating_sub(*at)<CACHE_SECONDS {return Ok(value.clone());}
    }
    if remaining(state.until,now)>0 {
        // Keep the last successful view usable while the client is cooling down.
        return state.cache.get(&key).map(|(_,v)|v.clone()).ok_or_else(||blocked(&state,now));
    }
    if let Some((until,error))=state.failures.get(&key) {
        if now<*until {return Err(error.clone());}
    }
    let result=fetch().await;
    match result {
        Ok((status,retry_after,body)) if status==429 => {
            let json: Value=serde_json::from_slice(&body).unwrap_or(Value::Null);
            let reason=json["error"]["reason"].as_str().or_else(||json["reason"].as_str()).unwrap_or("");
            state.quota=reason=="QUOTA_EXCEEDED";
            let wait=retry_seconds(retry_after.as_deref(),state.quota);
            state.until=seconds().saturating_add(wait);
            let saved=serde_json::json!({"until":state.until,"quota":state.quota});
            // Contains only the deadline, never a token or account data. Survives reloads.
            if let Some(path)=cooldown_file {
                if let Err(e)=std::fs::write(path,saved.to_string()) {log::warn!("Web API: cannot save cooldown: {e}");}
            }
            let error=blocked(&state,seconds());
            log::warn!("{error}");
            state.cache.get(&key).map(|(_,v)|v.clone()).ok_or(error)
        }
        Ok((status,_,body)) if (200..300).contains(&status) => {
            let value: Value=serde_json::from_slice(&body).map_err(|e|e.to_string())?;
            if state.cache.len()>=128 && !state.cache.contains_key(&key) {
                if let Some(oldest)=state.cache.iter().min_by_key(|(_, (at,_))|*at).map(|(k,_)|k.clone()) {state.cache.remove(&oldest);}
            }
            state.failures.remove(&key);
            state.cache.insert(key,(seconds(),value.clone()));
            Ok(value)
        }
        other => {
            let error=match other {
                Ok((401,_,_))=>"Spotify Web API did not accept this login token (401)".to_string(),
                Ok((403,_,_))=>"Spotify Web API denied this client's access (403)".to_string(),
                Ok((status,_,_))=>format!("Spotify Web API answered HTTP {status}"),
                Err(e)=>e,
            };
            if state.failures.len()>=128 {state.failures.clear();}
            state.failures.insert(key.clone(),(seconds().saturating_add(30),error.clone()));
            state.cache.get(&key).map(|(_,v)|v.clone()).ok_or(error)
        }
    }
}
async fn fetch(session: &Session,path: &str)->Result<(u16,Option<String>,bytes::Bytes),String> {
    tokio::time::timeout(Duration::from_secs(15),async {
        let token=match session.login5().auth_token().await {
            Ok(token)=>token,
            Err(login5)=>session.token_provider().get_token(SCOPES).await
                .map_err(|e|format!("no access token (login5: {login5}; keymaster: {e})"))?,
        };
        let request=http::Request::get(format!("https://api.spotify.com/v1/{path}"))
            .header("Authorization",format!("Bearer {}",token.access_token))
            .header("Accept","application/json").body(bytes::Bytes::new()).map_err(|e|e.to_string())?;
        // request_body retries 429 internally and drops its headers. The raw future
        // lets this shared gate retain Retry-After and hold other endpoints as well.
        let response=session.http_client().request_fut(request).map_err(|e|e.to_string())?.await.map_err(|e|e.to_string())?;
        let status=response.status().as_u16();
        let retry=response.headers().get("retry-after").and_then(|v|v.to_str().ok()).map(str::to_owned);
        let body=response.into_body().collect().await.map_err(|e|e.to_string())?.to_bytes();
        Ok((status,retry,body))
    }).await.map_err(|_|"Spotify Web API took too long to answer".to_string())?
}
#[cfg(test)]
mod tests {
    use super::*;
    #[tokio::test]
    async fn a_429_holds_other_endpoints_without_making_another_request() {
        let gate=Mutex::new(State::default());
        let first=get_with(&gate,key("alice","me/playlists"),None,||async {
            Ok((429,Some("3600".to_string()),bytes::Bytes::from_static(br#"{"error":{"reason":"QUOTA_EXCEEDED"}}"#)))
        }).await.unwrap_err();
        assert!(first.contains("quota exceeded"));
        let second=get_with(&gate,key("alice","me/albums"),None,||async {
            panic!("a secondary endpoint must not make a network request during cooldown")
        }).await.unwrap_err();
        assert!(second.contains("quota exceeded"));
    }
    #[tokio::test]
    async fn concurrent_refreshes_share_one_response_and_stale_data_survives_429() {
        let gate=Mutex::new(State::default());
        let calls=std::sync::atomic::AtomicUsize::new(0);
        let request=||get_with(&gate,key("alice","me/playlists"),None,||async {
            calls.fetch_add(1,std::sync::atomic::Ordering::Relaxed);
            tokio::time::sleep(Duration::from_millis(5)).await;
            Ok((200,None,bytes::Bytes::from_static(br#"{"items":[{"name":"Saved playlist"}]}"#)))
        });
        let (a,b)=tokio::join!(request(),request());
        assert_eq!(a.unwrap(),b.unwrap());assert_eq!(calls.load(std::sync::atomic::Ordering::Relaxed),1);
        gate.lock().await.cache.get_mut(&key("alice","me/playlists")).unwrap().0=0;
        let stale=get_with(&gate,key("alice","me/playlists"),None,||async {
            Ok((429,Some("120".into()),bytes::Bytes::new()))
        }).await.unwrap();
        assert_eq!(stale["items"][0]["name"],"Saved playlist");
        let same=get_with(&gate,key("alice","me/playlists"),None,||async {panic!("cooldown")}).await.unwrap();
        assert_eq!(same,stale);
    }
    #[test] fn server_cooldown_is_never_shortened() {
        assert_eq!(retry_seconds(Some("86400"),false),86400);
        assert_eq!(retry_seconds(Some("2"),false),2);
        assert_eq!(retry_seconds(Some("0"),false),60);
        assert_eq!(retry_seconds(None,true),3600);
        assert_eq!(retry_seconds(Some("bad"),false),60);
    }
    #[test] fn cooldown_boundary_and_cache_account_isolation() {
        assert_eq!(remaining(400,399),1);assert_eq!(remaining(400,400),0);
        assert_eq!(remaining(400,401),0);
        assert_ne!(key("alice","me/playlists"),key("bob","me/playlists"));
        assert_ne!(key("alice","me/playlists"),key("alice","me/albums"));
    }
}
