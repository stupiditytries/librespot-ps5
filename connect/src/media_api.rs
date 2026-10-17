//! App-only metadata. Existing receiver identity, independent client-service quota.
use librespot::core::session::Session;
use serde_json::{json,Value};
use http_body_util::BodyExt;
use std::time::Duration;

// Official mobile Web Player mobile-web-player.de4c0766.js, inspected 2026-10-17.
// One multi-operation document supplies findTracks and findAlbums.
const SEARCH_HASH:&str="903df2a65d8121e27d73a2be03c01e88ebe6021bb6d4eb82a389e35d87e51d27";
// The Now Playing view's artist query (web-player.0079e2e5.js): its headerImage is the wide
// artist picture Spotify's own player shows behind a song.
const NPV_ARTIST_HASH:&str="e1ae46a21911a3075c1aa29bf09a6c60f9a45b6a2b1132429f10ad06c299b5d7";
const BACKGROUND_DIR:&str="/user/data/spotifyps5/backgrounds";
const LIBRARY_WRITE_HASH:&str="896ebcb47815681340860d121cb5d494e157e2a78d3950385cd54e0393c67148";
// A changed operation is a visible failure, never an empty success or a Web API retry.
async fn query(session:&Session,operation:&str,hash:&str,variables:Value)->Result<Value,String>{
    let payload=json!({"operationName":operation,"variables":variables,"extensions":{"persistedQuery":{"version":1,"sha256Hash":hash}}});
    let token=session.login5().auth_token().await.map_err(|e|e.to_string())?;
    let client=session.spclient().client_token().await.map_err(|e|e.to_string())?;
    let request=http::Request::post("https://api-partner.spotify.com/pathfinder/v2/query")
        .header("authorization",format!("{} {}",token.token_type,token.access_token)).header("client-token",client)
        .header("content-type","application/json").header("app-platform","WebPlayer")
        .body(bytes::Bytes::from(payload.to_string())).map_err(|e|e.to_string())?;
    let result=crate::web_api::client_guarded(session,&format!("client:{operation}:{}:{}",payload["variables"],if operation.ends_with("Library"){static SEQ:std::sync::atomic::AtomicU64=std::sync::atomic::AtomicU64::new(0);crate::native_library::millis().saturating_mul(1000)+SEQ.fetch_add(1,std::sync::atomic::Ordering::Relaxed)%1000}else{0}),||async{
        let response=session.http_client().request_fut(request).map_err(|e|e.to_string())?.await.map_err(|e|e.to_string())?;
        let status=response.status().as_u16();let retry=response.headers().get("retry-after").and_then(|v|v.to_str().ok()).map(str::to_owned);
        let mut body=response.into_body();let mut bytes=Vec::new();
        while let Some(frame)=body.frame().await{if let Ok(data)=frame.map_err(|_|"Search response read failed")?.into_data(){
            if bytes.len()+data.len()>1024*1024{return Err("Client response too large".into());}bytes.extend_from_slice(&data);
        }}
        log::info!("client query operation={} status={} bytes={}",operation,status,bytes.len());
        Ok((status,retry,bytes.into()))
    }).await?;
    if let Some(errors)=result["errors"].as_array().filter(|e|!e.is_empty()){
        let changed=errors.iter().any(|e|e["message"].as_str().is_some_and(|m|m.contains("PersistedQueryNotFound")||m.contains("Unknown operation")));
        log::warn!("client query rejected operation={} contract_changed={}",operation,changed);
        return Err(if changed{"Spotify search changed. A receiver update is needed."}else{"Spotify rejected this client request. Try again later."}.into());
    }
    Ok(result)
}
/// The artist's wide header picture for a song, saved where the app can read it. Empty when the
/// artist has none or the request fails; the app then uses the album cover.
pub async fn artist_background(session:&Session,track_uri:&str,artist_uri:&str)->String{
    if artist_uri.is_empty(){return String::new();}
    let vars=json!({"artistUri":artist_uri,"trackUri":track_uri,"contributorsLimit":10,"contributorsOffset":0,
        "enableRelatedVideos":false,"enableRelatedAudioTracks":false});
    let result=match tokio::time::timeout(Duration::from_secs(8),query(session,"queryNpvArtist",NPV_ARTIST_HASH,vars)).await{
        Ok(Ok(v))=>v,
        Ok(Err(e))=>{log::warn!("artist background: {e}");return String::new();}
        Err(_)=>{log::warn!("artist background: timed out");return String::new();}
    };
    let sources=result["data"]["artistUnion"]["headerImage"]["data"]["sources"].as_array().cloned().unwrap_or_default();
    // Sharp enough for a 1080p screen, small enough to download: the widest up to 2200 px, else the smallest.
    // ImageV2 sources name their size maxWidth/maxHeight; older ones width/height. Reading only the
    // second gave every image width 0, so the smallest (a 320 px thumbnail) was picked.
    let width=|s:&Value|s["maxWidth"].as_u64().or_else(||s["width"].as_u64()).unwrap_or(0);
    let pick=sources.iter().filter(|s|width(s)<=2200&&s["url"].is_string()).max_by_key(|s|width(s))
        .or_else(||sources.iter().filter(|s|s["url"].is_string()).min_by_key(|s|width(s)));
    let Some(pick)=pick else{
        log::info!("artist background: none ({} sources)",sources.len());return String::new();
    };
    log::info!("artist background: {} sources ({}), using {}x{}",sources.len(),
        sources.iter().map(|s|width(s).to_string()).collect::<Vec<_>>().join("/"),
        width(pick),pick["maxHeight"].as_u64().or_else(||pick["height"].as_u64()).unwrap_or(0));
    crate::picture_file(session,pick["url"].as_str().unwrap_or(""),BACKGROUND_DIR,24).await.unwrap_or_default()
}
fn artists(v:&Value)->String{v.as_array().into_iter().flatten().filter_map(|a|a["name"].as_str().or_else(||a["profile"]["name"].as_str())).collect::<Vec<_>>().join(", ")}
fn image(v:&Value)->String{v.as_array().into_iter().flatten().filter_map(|v|v["url"].as_str()).find_map(crate::client_api::picture_url).unwrap_or_default()}
pub async fn search(session:Session,query:String,id:u64,lines:tokio::sync::broadcast::Sender<String>){
    let result=tokio::time::timeout(Duration::from_secs(8),async{
        query_search(&session,&query).await
    }).await;
    let (mut songs,mut albums,error)=match result{Ok(Ok((s,a)))=>(s,a,String::new()),Ok(Err(e))=>(vec![],vec![],e),Err(_)=>(vec![],vec![],"Search timed out; try again".into())};
    // The search payload does not always carry a length (every row showed 0:00). Missing
    // ones come from the track metadata the playlists already use, in one request.
    let missing=songs.iter().filter(|r|r["duration_ms"].as_u64().unwrap_or(0)==0).filter_map(|r|r["uri"].as_str().map(str::to_owned)).collect::<Vec<_>>();
    if !missing.is_empty(){
        let lengths=tokio::time::timeout(Duration::from_secs(4),crate::client_api::track_lengths(&session,&missing)).await.ok().unwrap_or_default();
        for row in songs.iter_mut(){
            if let Some(ms)=row["uri"].as_str().and_then(|u|lengths.get(u)){row["duration_ms"]=json!(ms);}
        }
    }
    // Art has its own 2.5 s budget: a slow image must not discard valid search results.
    let art_deadline=tokio::time::Instant::now()+Duration::from_millis(2500);
    for row in albums.iter_mut().take(4){
        let address=row["picture"].as_str().unwrap_or("").to_owned();
        let cover=tokio::time::timeout_at(art_deadline,crate::picture_file(&session,&address,"/user/data/spotifyps5/library-covers",1024)).await.ok().flatten().unwrap_or_default();
        row["cover"]=json!(cover);
    }
    let _=lines.send(json!({"v":2,"search":{"id":id,"query":query,"complete":true,"songs":songs,"albums":albums,"error":error}}).to_string());
    // Song results are text-only in the TV app; do not fetch invisible thumbnails.
    for row in &albums{
        if !row["cover"].as_str().unwrap_or("").is_empty(){continue;}
        if let Some(cover)=crate::picture_file(&session,row["picture"].as_str().unwrap_or(""),"/user/data/spotifyps5/library-covers",1024).await{
            let _=lines.send(json!({"v":2,"cover_ready":{"uri":row["uri"],"cover":cover,"search_id":id}}).to_string());
        }
    }
}
fn parse_search(tracks:&Value,albums:&Value)->Result<(Vec<Value>,Vec<Value>),String>{
    let rows=tracks["data"]["searchV2"]["tracksV2"]["items"].as_array().ok_or("Spotify search track format changed")?;
    let discs=albums["data"]["searchV2"]["albumsV2"]["items"].as_array().ok_or("Spotify search album format changed")?;
    let songs=rows.iter().filter_map(|r|{let t=&r["item"]["data"];let uri=t["uri"].as_str()?;
        if !uri.starts_with("spotify:track:")||!t["name"].is_string(){return None;}
        Some(json!({"uri":uri,"title":t["name"],"artist":artists(&t["artists"]["items"]),"duration_ms":t["duration"]["totalMilliseconds"],"explicit":t["contentRating"]["label"]=="EXPLICIT","picture":image(&t["albumOfTrack"]["coverArt"]["sources"])}))}).take(20).collect();
    let albums=discs.iter().filter_map(|r|{let a=&r["data"];let uri=a["uri"].as_str()?;
        if !uri.starts_with("spotify:album:")||!a["name"].is_string(){return None;}
        Some(json!({"uri":uri,"name":a["name"],"artist":artists(&a["artists"]["items"]),"year":a["date"]["year"],"picture":image(&a["coverArt"]["sources"])}))}).take(12).collect();
    Ok((songs,albums))
}
async fn query_search(session:&Session,search:&str)->Result<(Vec<Value>,Vec<Value>),String>{
    let vars=json!({"query":search,"offset":0,"limit":20});
    let tracks=query(session,"findTracks",SEARCH_HASH,vars.clone()).await?;
    let mut vars=vars;vars["includeAlbumPreReleases"]=json!(true);
    let albums=query(session,"findAlbums",SEARCH_HASH,vars).await?;
    parse_search(&tracks,&albums)
}
#[cfg(test)]mod search_tests{
 use super::*;
 #[test]fn contract_errors_are_not_empty_results(){assert!(parse_search(&json!({"data":null}),&json!({})).is_err());}
 #[test]fn empty_results_and_unavailable_entities_are_valid(){let tracks=json!({"data":{"searchV2":{"tracksV2":{"items":[{"item":{"data":{"__typename":"NotFound"}}}]}}}});let albums=json!({"data":{"searchV2":{"albumsV2":{"items":[]}}}});let(s,a)=parse_search(&tracks,&albums).unwrap();assert!(s.is_empty()&&a.is_empty());}
 #[test]fn normalize_real_wrappers(){let tracks=json!({"data":{"searchV2":{"tracksV2":{"items":[{"item":{"data":{"uri":"spotify:track:fixture","name":"Fixture","artists":{"items":[{"profile":{"name":"Artist"}}]},"duration":{"totalMilliseconds":1000}}}}]}}}});let albums=json!({"data":{"searchV2":{"albumsV2":{"items":[{"data":{"uri":"spotify:album:fixture","name":"Album","date":{"year":2026}}}]}}}});let(s,a)=parse_search(&tracks,&albums).unwrap();assert_eq!(s[0]["artist"],"Artist");assert_eq!(s[0]["duration_ms"],1000);assert_eq!(a[0]["year"],2026);}
}
pub async fn save(session:Session,uri:String,on:bool,lines:tokio::sync::broadcast::Sender<String>,panel:Option<std::sync::Arc<tokio::sync::watch::Sender<crate::control::State>>>){
    let result=tokio::time::timeout(Duration::from_secs(8),async{
        if uri.starts_with("spotify:playlist:"){
            query(&session,if on{"addToLibrary"}else{"removeFromLibrary"},LIBRARY_WRITE_HASH,json!({"uris":[uri]})).await.map(|_|())
        }else if uri.starts_with("spotify:track:")||uri.starts_with("spotify:album:"){
            crate::client_api::save(&session,&uri,on).await
        }else{Err("Unsupported saved-item URI".into())}
    }).await.map_err(|_|"Save timed out".to_string()).and_then(|v|v);
    let error=result.err().unwrap_or_default();
    if error.is_empty(){if let Some(panel)=panel{panel.send_modify(|s|{if s.uri==uri{s.liked=on;}});}}
    let _=lines.send(json!({"v":2,"saved":{"uri":uri,"on":on,"error":error}}).to_string());
}

// Only cache directories are exposed, over loopback. No arbitrary paths or uploads.
pub async fn serve_art()->std::io::Result<()> {
    use tokio::io::{AsyncReadExt,AsyncWriteExt};
    let listener=tokio::net::TcpListener::bind(("127.0.0.1",38577)).await?;
    let permits=std::sync::Arc::new(tokio::sync::Semaphore::new(4));
    loop{let(mut stream,_)=listener.accept().await?;let Ok(permit)=permits.clone().try_acquire_owned()else{continue;};
        tokio::spawn(async move{let _permit=permit;let _=tokio::time::timeout(Duration::from_secs(5),async{
            let mut head=vec![0;2048];let n=stream.read(&mut head).await?;
            let line=std::str::from_utf8(&head[..n]).unwrap_or("").lines().next().unwrap_or("");
            let parts=line.split_whitespace().collect::<Vec<_>>();
            if parts.len()==3&&parts[1].starts_with("/media/canvas/"){
                return crate::media_features::serve_asset(&mut stream,parts[0],parts[1],std::str::from_utf8(&head[..n]).unwrap_or("")).await;
            }
            let mut file=None;
            if parts.len()==3&&parts[0]=="GET" {let p=parts[1].trim_start_matches('/').split('/').collect::<Vec<_>>();
                if p.len()==2&&["covers","queue-covers","library-covers","backgrounds"].contains(&p[0])&&p[1].len()<128&&
                    p[1].bytes().all(|c|c.is_ascii_alphanumeric()||b".-_".contains(&c))&&!p[1].contains("..") {
                    let path=format!("/user/data/spotifyps5/{}/{}",p[0],p[1]);
                    if std::fs::symlink_metadata(&path).is_ok_and(|m|m.is_file()&&m.len()<=2*1024*1024){file=std::fs::read(path).ok();}
                }
            }
            let (status,body)=if let Some(bytes)=file{("200 OK",bytes)}else{("404 Not Found",Vec::new())};
            let head=format!("HTTP/1.1 {status}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n",body.len());
            stream.write_all(head.as_bytes()).await?;stream.write_all(&body).await?;Ok::<_,std::io::Error>(())
        }).await;});
    }
}
