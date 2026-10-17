//! The user's saved playlists and albums, for the console overlay's Library tab.
//!
//! Everything uses librespot's client protocol (see client_api.rs); the Web API's
//! shared client sat in a 24 h quota cooldown and showed as a permanent error.
//! Answers go to every overlay client as their own lines (see control.rs):
//!
//! ```json
//! {"v":1,"library":{"items":[{"kind":"playlist","uri":"spotify:playlist:…","name":"…",
//!  "subtitle":"…","cover":"/user/data/spotifyps5/library-covers/….jpg"}],"error":""}}
//! {"v":1,"tracks":{"uri":"spotify:album:…","items":[{"uri":"spotify:track:…","title":"…",
//!  "artist":"…"}],"error":""}}
//! ```
//!
//! Covers are fetched after the list is first sent, and the list is sent again as they arrive.

use librespot::core::session::Session;
use tokio::sync::{broadcast, Mutex};
use std::sync::OnceLock;
use crate::browse_cache::DiskCache;

use crate::picture_file;
use crate::client_api::get as browse_api;

const COVER_DIR: &str = "/user/data/spotifyps5/library-covers";
const COVERS_KEPT: usize = 1024;

/// The smallest picture still sharp at the panel's sizes, from a Web API `images` list.
fn picture_address(images: &serde_json::Value) -> String {
    let Some(list) = images.as_array() else { return String::new() };
    let mut best: Option<(u64, &str)> = None;
    for image in list {
        let Some(url) = image["url"].as_str() else { continue };
        let width = image["width"].as_u64().unwrap_or(640);
        if width >= 160 && best.map_or(true, |(w, _)| width < w) {
            best = Some((width, url));
        }
    }
    best.map(|(_, url)| url.to_string())
        .or_else(|| list.first().and_then(|image| image["url"].as_str()).map(str::to_string))
        .unwrap_or_default()
}

fn text(value: &serde_json::Value) -> String {
    value.as_str().unwrap_or_default().to_string()
}

fn artists(value: &serde_json::Value) -> String {
    value
        .as_array()
        .map(|list| list.iter().filter_map(|a| a["name"].as_str()).collect::<Vec<_>>().join(", "))
        .unwrap_or_default()
}

#[derive(Clone)]
struct Item {
    kind: &'static str,
    uri: String,
    name: String,
    subtitle: String,
    picture: String,
    cover: String,
    year:i32,
}

fn sort_recents(items: &mut [Item], history: &serde_json::Value, server_at: u64) {
    // Stable ties preserve Spotify's own Recents order. Local plays only override a
    // server snapshot if they happened after that snapshot's request began.
    items.sort_by_key(|item|std::cmp::Reverse(history[&item.uri].as_u64().filter(|&at|at>server_at).unwrap_or(0)));
}
fn library_line(items: &[Item], error: &str, cache: &DiskCache, epoch:u64) -> String {
    let mut items=items.to_vec();
    let history=cache.load("played-contexts-v1").map(|hit|hit.value).unwrap_or_default();
    let server_at=cache.load("recents-server-ms").and_then(|hit|hit.value.as_u64()).unwrap_or(0);
    sort_recents(&mut items,&history,server_at);
    serde_json::json!({
        "v": 1,"media_session":epoch,
        "library": {
            "items": items.iter().map(|item| serde_json::json!({
                "kind": item.kind, "uri": item.uri, "name": item.name,
                "subtitle": item.subtitle, "cover": item.cover,
            })).collect::<Vec<_>>(),
            "error": error,
        }
    })
    .to_string()
}

fn app_library(lines:&broadcast::Sender<String>,items:&[Item],error:&str,cache:&DiskCache,epoch:u64){
    log::info!("app library: playlists={} albums={} collections={}",items.iter().filter(|i|i.kind=="playlist").count(),items.iter().filter(|i|i.kind=="album").count(),items.iter().filter(|i|i.kind=="collection").count());
    let mut value:serde_json::Value=serde_json::from_str(&library_line(items,error,cache,epoch)).unwrap();
    value["v"]=serde_json::json!(2);
    if let Some(rows)=value["library"]["items"].as_array_mut(){for row in rows{
        row["year"]=serde_json::json!(items.iter().find(|i|row["uri"]==i.uri).map_or(0,|i|i.year));
    }}
    let _=lines.send(value.to_string());
}

// Keep the URI-to-image mapping with the snapshot, so a process restart can
// show the cover before making a single network request.
fn snapshot(items: &[Item]) -> serde_json::Value {
    serde_json::Value::Array(items.iter().map(|i|serde_json::json!({
        "kind":i.kind,"uri":i.uri,"name":i.name,"subtitle":i.subtitle,"picture":i.picture,"year":i.year,
    })).collect())
}
fn restore(value: &serde_json::Value) -> Vec<Item> {
    value.as_array().into_iter().flatten().filter_map(|i| {
        let kind=match i["kind"].as_str()? {"album"=>"album","playlist"=>"playlist","collection"=>"collection",_=>return None};
        Some(Item {kind,uri:text(&i["uri"]),name:text(&i["name"]),subtitle:text(&i["subtitle"]),picture:text(&i["picture"]),cover:String::new(),year:i["year"].as_i64().unwrap_or(0) as i32})
    }).collect()
}
fn hydrate(items: &mut [Item], cache: &DiskCache) -> usize {
    let mut hits=0;
    for item in items {
        if item.kind=="collection"&&item.picture.is_empty(){item.picture=crate::native_library::LIKED_COVER.into();}
        if item.picture.is_empty() {
            item.picture=cache.load(&format!("cover-v8:{}",item.uri)).and_then(|hit|hit.value.as_str().map(str::to_owned)).unwrap_or_default();
        }
        item.cover=crate::cached_picture_file(&item.picture,COVER_DIR).unwrap_or_default();
        if !item.cover.is_empty(){hits+=1;}
    }
    hits
}

/// Sends the library: Liked Songs, then the user's playlists, then their saved albums.
pub async fn send_library(session: Session, lines: broadcast::Sender<String>,epoch:u64,force:bool) {
    let cache=DiskCache::for_account(&session.username());
    if let Some(saved)=cache.load("library-snapshot-v8") {
        let mut items=restore(&saved.value);
        let hits=hydrate(&mut items,&cache);
        if !items.is_empty() {
            let _=lines.send(library_line(&items,"",&cache,epoch));app_library(&lines,&items,"",&cache,epoch);
            log::info!("library cache: {} items, {hits} covers reused",items.len());
            if !force&&saved.fresh(crate::browse_cache::now(),30)&&items.iter().all(|i|i.picture.is_empty()||!i.cover.is_empty()){return;}
        }
    }
    // Repeated button presses share the running refresh, which broadcasts its result.
    type Flight=std::sync::Weak<Mutex<()>>;
    static REFRESH:OnceLock<Mutex<std::collections::HashMap<u64,Flight>>>=OnceLock::new();
    let flight={let mut flights=REFRESH.get_or_init(Default::default).lock().await;
        flights.retain(|_,v|v.strong_count()>0);
        if let Some(f)=flights.get(&epoch).and_then(std::sync::Weak::upgrade){f}else{
            let f=std::sync::Arc::new(Mutex::new(()));flights.insert(epoch,std::sync::Arc::downgrade(&f));f}};
    let _refresh=if force {flight.lock_owned().await}else{let Ok(guard)=flight.try_lock_owned() else{return;};guard};
    let mut error=String::new();
    let mut items=match if force {crate::client_api::get_fresh(&session,"library/recents").await}else{browse_api(&session,"library/recents").await} {
        Ok(page)=>restore(&page["items"]),
        Err(e)=>{
            log::warn!("library: Spotify Recents unavailable, retaining collection fallback: {e}");
    let mut items = Vec::new();
    let user = session.username();
    if !user.is_empty() {
        items.push(Item {
            kind: "collection",
            uri: format!("spotify:user:{user}:collection"),
            name: "Liked Songs".to_string(),
            subtitle: "Playlist".to_string(),
            picture: String::new(),
            cover: String::new(),year:0,
        });
    }
    match browse_api(&session, "me/playlists?limit=50").await {
        Ok(page) => {
            for playlist in page["items"].as_array().into_iter().flatten() {
                let uri = text(&playlist["uri"]);
                if uri.is_empty() {
                    continue;
                }
                let owner = text(&playlist["owner"]["display_name"]);
                items.push(Item {
                    kind: "playlist",
                    uri,
                    name: text(&playlist["name"]),
                    subtitle: if owner.is_empty() { "Playlist".to_string() } else { format!("Playlist · {owner}") },
                    picture: picture_address(&playlist["images"]),
                    cover: String::new(),year:0,
                });
            }
        }
        Err(e) => {
            log::warn!("library: playlists: {e}");
            error = e;
        }
    }
    match browse_api(&session, "me/albums?limit=50").await {
        Ok(page) => {
            for saved in page["items"].as_array().into_iter().flatten() {
                let album = &saved["album"];
                let uri = text(&album["uri"]);
                if uri.is_empty() {
                    continue;
                }
                items.push(Item {
                    kind: "album",
                    uri,
                    name: text(&album["name"]),
                    subtitle: format!("Album · {}", artists(&album["artists"])),
                    picture: picture_address(&album["images"]),
                    cover: String::new(),year:album["year"].as_i64().unwrap_or(0) as i32,
                });
            }
        }
        // Playlists still show; a missing album shelf is logged, not a banner.
        Err(e) => log::warn!("library: albums: {e}"),
    }
            items
        }
    };
    let hits=hydrate(&mut items,&cache);
    log::info!("library: {} items, {hits} covers reused", items.len());
    let _ = lines.send(library_line(&items, &error,&cache,epoch));
    app_library(&lines,&items,&error,&cache,epoch);
    // Covers, sent again every few as they arrive.
    let mut fetched = 0;
    // Warm both first screens before downloading the remainder of either kind.
    let mut order=Vec::new();
    for kind in ["collection","playlist","album"] {
        order.extend(items.iter().enumerate().filter(|(_,i)|i.kind==kind).take(8).map(|(n,_)|n));
    }
    for n in 0..items.len(){if !order.contains(&n){order.push(n);}}
    for index in order {
        // Refresh generated artwork at most once daily; render its old image first.
        if items[index].kind=="playlist" && cache.load(&format!("cover-v8:{}",items[index].uri))
            .is_some_and(|hit|!hit.fresh(crate::browse_cache::now(),86400)) {
            if let Some(address)=crate::client_api::playlist_cover(&session,&items[index].uri).await {
                if address!=items[index].picture {
                    items[index].picture=address;
                    items[index].cover=crate::cached_picture_file(&items[index].picture,COVER_DIR).unwrap_or_default();
                }
            }
        }
        // Skip both the image download and playlist metadata lookup on a cache hit.
        if !items[index].cover.is_empty(){continue;}
        if items[index].picture.is_empty() && items[index].kind == "playlist" {
            if let Some(address) = crate::client_api::playlist_cover(&session, &items[index].uri).await {
                items[index].picture = address;
            }
        }
        if items[index].picture.is_empty() {
            continue;
        }
        if let Some(path) = picture_file(&session, &items[index].picture, COVER_DIR, COVERS_KEPT).await {
            items[index].cover = path;
            let _=lines.send(serde_json::json!({"v":2,"media_session":epoch,"cover_ready":{"uri":items[index].uri,"cover":items[index].cover}}).to_string());
            fetched += 1;
            if fetched % 8 == 0 {
                cache.store("library-snapshot-v8",&snapshot(&items));
                let _ = lines.send(library_line(&items, &error,&cache,epoch));
            }
        }
    }
    if error.is_empty(){cache.store("library-snapshot-v8",&snapshot(&items));}
    let _ = lines.send(library_line(&items, &error,&cache,epoch));
    app_library(&lines,&items,&error,&cache,epoch);
}

/// Remember only actual playback contexts, and update any connected library view.
pub(crate) fn record_context_played(session:&Session,uri:&str,lines:Option<&broadcast::Sender<String>>,epoch:u64) {
    let user=session.username();
    let uri=if uri=="spotify:collection:tracks"{format!("spotify:user:{user}:collection")}else{uri.to_owned()};
    if !uri.starts_with("spotify:playlist:")&&!uri.starts_with("spotify:album:")&&!uri.ends_with(":collection"){return;}
    let cache=DiskCache::for_account(&user);
    let mut history=cache.load("played-contexts-v1").map(|hit|hit.value).filter(|value|value.is_object()).unwrap_or_else(||serde_json::json!({}));
    let at=crate::native_library::millis().max(history[&uri].as_u64().unwrap_or(0).saturating_add(1));
    history[&uri]=serde_json::json!(at);
    if history.as_object().is_some_and(|map|map.len()>1024) {
        let mut entries=history.as_object().unwrap().iter().map(|(uri,at)|(uri.clone(),at.as_u64().unwrap_or(0))).collect::<Vec<_>>();
        entries.sort_by_key(|(_,at)|std::cmp::Reverse(*at));
        history=serde_json::Value::Object(entries.into_iter().take(1024).map(|(uri,at)|(uri,serde_json::json!(at))).collect());
    }
    cache.store("played-contexts-v1",&history);
    if let (Some(lines),Some(saved))=(lines,cache.load("library-snapshot-v8")) {
        let mut items=restore(&saved.value);
        hydrate(&mut items,&cache);
        let _=lines.send(library_line(&items,"",&cache,epoch));app_library(&lines,&items,"",&cache,epoch);
    }
}

/// Sends a playlist's, an album's or Liked Songs' tracks.
pub async fn send_tracks(session: Session, uri: String, lines: broadcast::Sender<String>,epoch:u64) {
    tracks(session,uri,0,100,true,String::new(),0,lines,epoch).await;
}
pub async fn send_tracks_page(session:Session,uri:String,offset:usize,client:String,id:u64,lines:broadcast::Sender<String>,epoch:u64){
    tracks(session,uri,offset,30,false,client,id,lines,epoch).await;
}
async fn tracks(session:Session,uri:String,offset:usize,limit:usize,legacy:bool,client:String,request_id:u64,lines:broadcast::Sender<String>,epoch:u64){
    let id = uri.rsplit(':').next().unwrap_or_default().to_string();
    let path = if uri.ends_with(":collection") {
        "me/tracks?limit=50".to_string()
    } else if uri.starts_with("spotify:playlist:") {
        format!("playlists/{id}/tracks?limit=100&fields=items(track(name,uri,artists(name)))")
    } else if uri.starts_with("spotify:album:") {
        format!("albums/{id}/tracks?limit=50")
    } else {
        return;
    };
    let path=format!("{}?offset={offset}&limit={limit}",path.split('?' ).next().unwrap_or(&path));
    let (page_result,header_result)=tokio::join!(
        tokio::time::timeout(std::time::Duration::from_secs(10),browse_api(&session,&path)),
        tokio::time::timeout(std::time::Duration::from_secs(10),crate::client_api::list_info(&session,&uri)));
    let page_result=page_result.map_err(|_|"List request timed out".to_string()).and_then(|v|v);
    let (mut items, error,total,next) = match page_result {
        Ok(page) => {
            let mut items = Vec::new();
            for entry in page["items"].as_array().into_iter().flatten() {
                // Playlists and Liked Songs wrap each track; an album's items are tracks.
                let track = if entry.get("track").is_some() { &entry["track"] } else { entry };
                let track_uri = text(&track["uri"]);
                if !track_uri.starts_with("spotify:track:") && !track_uri.starts_with("spotify:episode:") {
                    continue;
                }
                items.push(serde_json::json!({
                    "uri": track_uri, "title": text(&track["name"]), "artist": artists(&track["artists"]),
                    "duration_ms":track["duration_ms"],"explicit":track["explicit"],"cover":track["cover"],
                }));
            }
            (items, String::new(),page["total"].as_u64().unwrap_or(0),page["next"].as_u64().unwrap_or(0))
        }
        Err(e) => {
            log::warn!("library: tracks of {uri}: {e}");
            (Vec::new(), e,0,offset as u64)
        }
    };
    let line = serde_json::json!({"v": 1,"media_session":epoch, "tracks": {"uri": uri, "items": items.iter().map(|t|serde_json::json!({"uri":t["uri"],"title":t["title"],"artist":t["artist"]})).collect::<Vec<_>>(), "error": error}});
    if legacy {let _ = lines.send(line.to_string());}
    let uri=line["tracks"]["uri"].as_str().unwrap_or("");
    let header_result=header_result.map_err(|_|"List details timed out".to_string()).and_then(|v|v);
    let metadata_error=header_result.as_ref().err().map(|_|"Some list information is unavailable. Reopen this list to retry.").unwrap_or("");
    let mut header=header_result.unwrap_or_else(|_|serde_json::json!({"uri":uri}));
    header["metadata_error"]=serde_json::json!(metadata_error);
    header["uri"]=serde_json::json!(uri);
    if header["kind"].as_str().unwrap_or("").is_empty(){header["kind"]=serde_json::json!(if uri.starts_with("spotify:album:"){"album"}else{"playlist"}); }
    // Resolve only the first visible row covers before publishing; stragglers follow
    // as small updates, never remote URLs handed to the foreground app.
    let addresses=items.iter().map(|t|t["cover"].as_str().unwrap_or("").to_owned()).collect::<Vec<_>>();
    for (row,address) in items.iter_mut().zip(&addresses) {
        row["cover"]=serde_json::json!(crate::cached_picture_file(address,COVER_DIR).unwrap_or_default());
    }
    header["items"]=serde_json::json!(items);header["error"]=line["tracks"]["error"].clone();
    header["count"]=serde_json::json!(total);header["next"]=serde_json::json!(next);
    header["offset"]=serde_json::json!(offset);
    let _=lines.send(serde_json::json!({"v":2,"media_session":epoch,"client":client,"id":request_id,"list":header}).to_string());
    for (row,address) in items.iter().zip(addresses) {
        if !row["cover"].as_str().unwrap_or("").is_empty(){continue;}
        if let Some(path)=picture_file(&session,&address,COVER_DIR,COVERS_KEPT).await {
            let _=lines.send(serde_json::json!({"v":2,"media_session":epoch,"cover_ready":{"uri":row["uri"],"cover":path}}).to_string());
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn albums_and_playlists_share_one_last_played_order() {
        let row=|kind,uri:&str|Item{kind,uri:uri.into(),name:uri.into(),subtitle:String::new(),picture:String::new(),cover:String::new(),year:0};
        let mut rows=vec![row("playlist","P1"),row("album","A1"),row("playlist","P2"),row("album","A2")];
        sort_recents(&mut rows,&serde_json::json!({"P1":5,"A1":20,"P2":30,"A2":10}),0);
        assert_eq!(rows.iter().map(|row|row.uri.as_str()).collect::<Vec<_>>(),vec!["P2","A1","A2","P1"]);
        // Fresh server order outranks older local history; a new local play wins.
        sort_recents(&mut rows,&serde_json::json!({"A2":200,"P1":5}),100);
        assert_eq!(rows.iter().map(|row|row.uri.as_str()).collect::<Vec<_>>(),vec!["A2","P2","A1","P1"]);
    }
    #[test]
    fn snapshots_restore_cover_addresses_without_trusting_old_file_paths() {
        let rows=vec![Item {kind:"album",uri:"spotify:album:A".into(),name:"Album".into(),subtitle:"Artist".into(),picture:"https://i.scdn.co/image/abc".into(),cover:"/an/old/file.jpg".into(),year:2024}];
        let restored=restore(&snapshot(&rows));
        assert_eq!(restored.len(),1);
        assert_eq!(restored[0].picture,rows[0].picture);
        assert!(restored[0].cover.is_empty());
        assert!(restore(&serde_json::json!([{"kind":"unexpected"}])).is_empty());
    }
    #[test]
    fn the_smallest_sharp_picture_is_chosen() {
        let images = serde_json::json!([
            {"url": "https://i.scdn.co/image/big", "width": 640},
            {"url": "https://i.scdn.co/image/mid", "width": 300},
            {"url": "https://i.scdn.co/image/small", "width": 64},
        ]);
        assert_eq!(picture_address(&images), "https://i.scdn.co/image/mid");
        // Playlists' pictures can come without sizes.
        let no_sizes = serde_json::json!([{"url": "https://mosaic.scdn.co/640/abc", "width": null}]);
        assert_eq!(picture_address(&no_sizes), "https://mosaic.scdn.co/640/abc");
        assert_eq!(picture_address(&serde_json::json!(null)), "");
    }
}
