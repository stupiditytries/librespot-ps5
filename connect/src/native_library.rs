//! Spotify's own mixed Library/Recents view, using the authenticated client protocol.
use librespot::core::session::Session;
use serde_json::{json, Value};
use std::collections::HashSet;

// Verified against Spotify's public desktop bundle on 2026-10-16:
// https://open.spotifycdn.com/cdn/build/web-player/web-player.75bb3c66.js
// A rejected persisted query falls back to the existing root-list/collection loader.
const LIBRARY_QUERY: &str = "390c78e5b951029bad359785e69b07b536a509c581cbcd0aded5e5067f187455";
const ENDPOINT: &str = "https://api-partner.spotify.com/pathfinder/v2/query";
pub const LIKED_COVER: &str = "https://misc.scdn.co/liked-songs/liked-songs-300.jpg";

fn variables(offset: usize) -> Value {
    json!({"filters":[],"order":"Recents","textFilter":"","features":["LIKED_SONGS"],
        "limit":100,"offset":offset,"flatten":true,"expandedFolders":[],
        "folderUri":null,"includeFoldersWhenFlattening":false})
}

fn cover(sources: &Value) -> String {
    sources.as_array().into_iter().flatten().filter_map(|source| {
        let address=crate::client_api::picture_url(source["url"].as_str()?)?;
        let width=source["width"].as_u64().unwrap_or(300);
        Some((width.abs_diff(300),address))
    }).min_by_key(|(distance,_)|*distance).map(|(_,address)|address).unwrap_or_default()
}

fn text(value: &Value) -> &str { value.as_str().unwrap_or_default() }

fn item(row: &Value, username: &str) -> Option<Value> {
    let data=row.pointer("/item/data")?;
    let raw_uri=data["uri"].as_str().or_else(||row.pointer("/item/_uri").and_then(Value::as_str))?;
    let (kind,uri,subtitle,picture)=match text(&data["__typename"]) {
        "Playlist"=>("playlist",raw_uri.to_owned(),format!("Playlist · {}",text(&data["ownerV2"]["data"]["name"])),
            cover(&data["images"]["items"][0]["sources"])),
        "Album"=>{
            let artists=data["artists"]["items"].as_array().into_iter().flatten()
                .filter_map(|artist|artist["profile"]["name"].as_str()).collect::<Vec<_>>().join(", ");
            ("album",raw_uri.to_owned(),format!("Album · {artists}"),cover(&data["coverArt"]["sources"]))
        }
        "PseudoPlaylist" if raw_uri=="spotify:collection:tracks"=>{
            let picture=cover(&data["image"]["sources"]);
            ("collection",format!("spotify:user:{username}:collection"),"Playlist".into(),
                if picture.is_empty(){LIKED_COVER.into()}else{picture})
        }
        _=>return None,
    };
    let name=text(&data["name"]);
    if name.is_empty(){return None;}
    Some(json!({"kind":kind,"uri":uri,"name":name,"subtitle":subtitle,"picture":picture,"year":data["date"]["year"].as_i64().unwrap_or(0)}))
}

fn page(body: &[u8], offset: usize, username: &str) -> Result<(Vec<Value>,usize,usize),String> {
    let root:Value=serde_json::from_slice(body).map_err(|e|e.to_string())?;
    if let Some(errors)=root["errors"].as_array().filter(|errors|!errors.is_empty()) {
        let message=errors.iter().filter_map(|e|e["message"].as_str()).collect::<Vec<_>>().join("; ");
        return Err(format!("Spotify Library query rejected: {message}"));
    }
    let library=&root["data"]["me"]["libraryV3"];
    if library["pagingInfo"]["offset"].as_u64()!=Some(offset as u64){return Err("Unexpected Library page offset".into());}
    let rows=library["items"].as_array().ok_or("No Library rows returned")?;
    let total=library["totalCount"].as_u64().ok_or("No Library total returned")? as usize;
    Ok((rows.iter().filter_map(|row|item(row,username)).collect(),rows.len(),total))
}

pub async fn fetch(session: &Session) -> Result<Value,String> {
    let began=millis();
    let token=session.login5().auth_token().await.map_err(|e|e.to_string())?;
    let client=session.spclient().client_token().await.map_err(|e|e.to_string())?;
    let username=session.username();
    let mut items=Vec::new();
    let mut seen=HashSet::new();
    let mut offset=0;
    for _ in 0..20 {
        let payload=json!({"operationName":"libraryV3","variables":variables(offset),
            "extensions":{"persistedQuery":{"version":1,"sha256Hash":LIBRARY_QUERY}}});
        let request=http::Request::post(ENDPOINT)
            .header(http::header::ACCEPT,"application/json")
            .header(http::header::CONTENT_TYPE,"application/json")
            .header(http::header::AUTHORIZATION,format!("{} {}",token.token_type,token.access_token))
            .header("client-token",&client).header("app-platform","WebPlayer")
            .header("spotify-app-version","896000000")
            .body(bytes::Bytes::from(payload.to_string())).map_err(|e|e.to_string())?;
        let bytes=session.http_client().request_body(request).await.map_err(|e|e.to_string())?;
        let (rows,count,total)=page(&bytes,offset,&username)?;
        items.extend(rows.into_iter().filter(|row|seen.insert(text(&row["uri"]).to_owned())));
        offset+=count;
        if offset>=total {
            if offset>0&&items.is_empty(){return Err("Spotify Library contained no supported album or playlist rows".into());}
            crate::browse_cache::DiskCache::for_account(&username).store("recents-server-ms",&json!(began));
            log::info!("client library: {} albums/playlists in Spotify Recents order",items.len());
            return Ok(json!({"items":items}));
        }
        if count==0 {return Err("Spotify Library stopped paging before its total".into());}
    }
    Err("Spotify Library exceeded 2000 rows".into())
}

pub fn millis() -> u64 {
    std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_millis() as u64
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn mixed_recents_keep_server_order_and_their_own_artwork() {
        let album=json!({"item":{"data":{"__typename":"Album","uri":"spotify:album:A","name":"Album","artists":{"items":[{"profile":{"name":"Artist"}}]},"coverArt":{"sources":[{"url":"https://i.scdn.co/image/album","width":300}]}}}});
        let playlist=json!({"item":{"data":{"__typename":"Playlist","uri":"spotify:playlist:P","name":"Mix","ownerV2":{"data":{"name":"Spotify"}},"images":{"items":[{"sources":[{"url":"https://image-cdn-ak.spotifycdn.com/image/mix.jpg","width":300}]}]}}}});
        let liked=json!({"item":{"data":{"__typename":"PseudoPlaylist","uri":"spotify:collection:tracks","name":"Liked Songs"}}});
        let root=json!({"data":{"me":{"libraryV3":{"items":[album,playlist,liked],"totalCount":3,"pagingInfo":{"offset":0}}}}});
        let (rows,count,total)=page(root.to_string().as_bytes(),0,"user").unwrap();
        assert_eq!((count,total),(3,3));
        assert_eq!(rows.iter().map(|row|text(&row["kind"])).collect::<Vec<_>>(),vec!["album","playlist","collection"]);
        assert_eq!(rows[1]["picture"],"https://image-cdn-ak.spotifycdn.com/image/mix.jpg");
        assert_eq!(rows[2]["uri"],"spotify:user:user:collection");
        assert_eq!(rows[2]["picture"],LIKED_COVER);
        assert_eq!(variables(100)["order"],"Recents");
        assert_eq!(variables(100)["offset"],100);
    }
    #[test]
    fn query_errors_and_wrong_pages_are_not_empty_libraries() {
        assert!(page(br#"{"errors":[{"message":"PersistedQueryNotFound"}]}"#,0,"u").unwrap_err().contains("PersistedQueryNotFound"));
        assert!(page(br#"{"data":{"me":{"libraryV3":{"items":[],"totalCount":0,"pagingInfo":{"offset":2}}}}}"#,0,"u").is_err());
    }
}
