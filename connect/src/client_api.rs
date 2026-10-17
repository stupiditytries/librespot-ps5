//! Library reads using the same authenticated client protocol as Connect playback.
use std::{collections::HashMap, sync::OnceLock, time::{Duration, Instant}};
use librespot::{core::{session::Session, SpotifyId, SpotifyUri},
    metadata::{Metadata, album::Album}};
use protobuf::Message;
use serde_json::{json, Value};
use tokio::sync::Mutex;
use librespot::protocol::playlist4_external::SelectedListContent;
use librespot::protocol::{extended_metadata::{BatchedEntityRequest,EntityRequest,ExtensionQuery},
    extension_kind::ExtensionKind,metadata::{Album as AlbumData,ImageGroup,Track as TrackData}};
use protobuf::EnumOrUnknown;

#[derive(Default)]
struct Cache { pages: HashMap<String,(Instant,Value)>, failures: HashMap<String,(Instant,String)> }
fn cache() -> &'static Mutex<Cache> {
    static CACHE: OnceLock<Mutex<Cache>>=OnceLock::new();
    CACHE.get_or_init(||Mutex::new(Cache::default()))
}
pub async fn get(session: &Session,path: &str)->Result<Value,String> {
    get_inner(session,path,false).await
}
pub async fn get_fresh(session:&Session,path:&str)->Result<Value,String>{get_inner(session,path,true).await}
async fn get_inner(session:&Session,path:&str,force:bool)->Result<Value,String>{
    // Every view uses the client protocol. The Web API's shared client was in a
    // 24 h quota cooldown, which showed as a permanent library error.
    let key=format!("{}\n{path}",session.username());
    // Single flight per page, not one network-long lock for the whole library.
    // A background refresh must not block opening a selected album or playlist.
    type Flight=std::sync::Weak<Mutex<()>>;
    static FLIGHTS:OnceLock<Mutex<HashMap<String,Flight>>> = OnceLock::new();
    let flight={let mut flights=FLIGHTS.get_or_init(Default::default).lock().await;
        flights.retain(|_,v|v.strong_count()>0);
        if let Some(f)=flights.get(&key).and_then(std::sync::Weak::upgrade){f}else{let f=std::sync::Arc::new(Mutex::new(()));flights.insert(key.clone(),std::sync::Arc::downgrade(&f));f}};
    let _flight=flight.lock().await;
    let disk=crate::browse_cache::DiskCache::for_account(&session.username());
    let saved=disk.load(&format!("page:{path}"));
    if let Some(hit)=saved.as_ref().filter(|hit|!force&&hit.fresh(crate::browse_cache::now(),if path=="library/recents"{30}else{300})) {
        return Ok(hit.value.clone());
    }
    {
    let c=cache().lock().await;
    if let Some((at,page))=c.pages.get(&key) {
        if !force&&at.elapsed()<Duration::from_secs(if path=="library/recents"{30}else{300}){return Ok(page.clone());}
    }
    if let Some((at,error))=c.failures.get(&key) {
        if !force&&at.elapsed()<Duration::from_secs(30){return c.pages.get(&key).map(|(_,v)|v.clone()).or_else(||saved.as_ref().map(|hit|hit.value.clone())).ok_or_else(||error.clone());}
    }
    }
    let result=tokio::time::timeout(Duration::from_secs(40),fetch(session,path)).await
        .map_err(|_|"Spotify client library request timed out".to_string()).and_then(|r|r);
    let mut c=cache().lock().await;
    match result {
        Ok(value)=>{
            if c.pages.len()>=64 {c.pages.clear();}
            disk.store(&format!("page:{path}"),&value);
            c.failures.remove(&key);c.pages.insert(key,(Instant::now(),value.clone()));Ok(value)
        }
        Err(error)=>{
            log::warn!("client library: {error}");
            if c.failures.len()>=64{c.failures.clear();}
            c.failures.insert(key.clone(),(Instant::now(),error.clone()));
            c.pages.get(&key).map(|(_,v)|v.clone()).or_else(||saved.map(|hit|hit.value)).ok_or(error)
        }
    }
}
fn root_items(page: &SelectedListContent)->Vec<Value> {
    let contents=page.contents.get_or_default();
    contents.items.iter().enumerate().filter_map(|(index,item)| {
        let uri=SpotifyUri::from_uri(item.uri()).ok()?;
        if !matches!(uri,SpotifyUri::Playlist{..}){return None;}
        let meta=contents.meta_items.get(index);
        let attr=meta.map(|m|m.attributes.get_or_default());
        let name=attr.map(|a|a.name()).filter(|s|!s.is_empty()).unwrap_or("Playlist");
        // A playlist's own picture is an image id; picture_size is usually empty
        // in the root list, which is why no playlist ever showed a cover.
        let mut images=attr.and_then(|a|image_address(a.picture()))
            .map(|url|vec![json!({"url":url,"width":300})]).unwrap_or_default();
        images.extend(attr.into_iter().flat_map(|a|a.picture_size.iter())
            .filter(|p|p.url().starts_with("https://"))
            .map(|p|json!({"url":p.url(),"width":300})));
        Some(json!({"uri":uri.to_uri().ok()?,"name":name,"images":images,
            "owner":{"display_name":meta.map(|m|m.owner_username()).unwrap_or("")}}))
    }).collect()
}
pub(crate) fn picture_url(value:&str)->Option<String> {
    let value=value.trim();
    if value.contains(['{','}']){return None;}
    if crate::cover_address_allowed(value){return Some(value.to_owned());}
    if let Some(id)=value.strip_prefix("spotify:image:") {
        if id.len()==40&&id.bytes().all(|b|b.is_ascii_hexdigit()){return Some(format!("https://i.scdn.co/image/{id}"));}
    }
    if let Some(parts)=value.strip_prefix("spotify:mosaic:") {
        let ids=parts.split(':').collect::<Vec<_>>();
        if !ids.is_empty()&&ids.len()<=4&&ids.iter().all(|id|id.len()==40&&id.bytes().all(|b|b.is_ascii_hexdigit())) {
            return Some(format!("https://mosaic.scdn.co/300/{}",ids.join("")));
        }
    }
    None
}
fn image_address(id: &[u8])->Option<String> {
    if id.len()==20 {return Some(format!("https://i.scdn.co/image/{}",hex::encode(id)));}
    std::str::from_utf8(id).ok().and_then(picture_url)
}
fn playlist_picture(page:&SelectedListContent)->Option<String> {
    let attributes=page.attributes.as_ref()?;
    attributes.picture_size.iter().find_map(|picture|picture_url(picture.url()))
        .or_else(||image_address(attributes.picture()))
}
/// The 300 px picture of an image group, else any.
fn group_address(group: Option<&ImageGroup>)->Option<String> {
    let images=&group?.image;
    images.iter().find(|i|i.width()==300||i.size()==librespot::protocol::metadata::image::Size::DEFAULT)
        .or_else(||images.first()).and_then(|i|image_address(i.file_id()))
}
/// One extended-metadata request for many entities, keyed by URI.
async fn batch(session: &Session,kind: ExtensionKind,uris: &[String])->Result<HashMap<String,Vec<u8>>,String> {
    let request=BatchedEntityRequest{entity_request:uris.iter().map(|uri|EntityRequest{
        entity_uri:uri.clone(),
        query:vec![ExtensionQuery{extension_kind:EnumOrUnknown::new(kind),..Default::default()}],
        ..Default::default()}).collect(),..Default::default()};
    let response=tokio::time::timeout(Duration::from_secs(15),session.spclient().get_extended_metadata(request)).await
        .map_err(|_|"track details timed out".to_string())?.map_err(|e|e.to_string())?;
    let mut found=HashMap::new();
    for array in response.extended_metadata {
        for data in array.extension_data {
            if let Some(any)=data.extension_data.into_option(){found.insert(data.entity_uri,any.value);}
        }
    }
    Ok(found)
}
/// Track rows in order, from one batched request per 100 tracks. v5 fetched each
/// track separately and failed the whole list on one bad row, so opening a
/// playlist took tens of seconds or showed nothing.
async fn rows(session: &Session,uris: Vec<String>,offset:usize,limit:usize)->Result<Value,String> {
    let total=uris.len();let next=(offset+limit).min(total);
    let uris=uris.into_iter().skip(offset).take(limit).filter(|u|u.starts_with("spotify:track:")).collect::<Vec<_>>();
    if uris.is_empty(){return Ok(json!({"items":[],"total":total,"next":next}));}
    let found=batch(session,ExtensionKind::TRACK_V4,&uris).await?;
    let items=uris.iter().filter_map(|uri| {
        let track=TrackData::parse_from_bytes(found.get(uri)?).ok()?;
        let artists=track.artist.iter().map(|a|json!({"name":a.name()})).collect::<Vec<_>>();
        let cover=group_address(track.album.as_ref().and_then(|a|a.cover_group.as_ref()));
        Some(json!({"uri":uri,"name":track.name(),"artists":artists,"duration_ms":track.duration(),"explicit":track.explicit(),"cover":cover.unwrap_or_default()}))
    }).collect::<Vec<_>>();
    log::info!("client library: {} of {} track rows",items.len(),uris.len());
    if items.is_empty(){return Err("Spotify returned no track details".into());}
    Ok(json!({"items":items,"total":total,"next":next}))
}
/// Track lengths in milliseconds, by URI, from one metadata request. Failures give fewer entries.
pub async fn track_lengths(session: &Session,uris: &[String])->HashMap<String,u64> {
    let mut lengths=HashMap::new();
    if let Ok(found)=batch(session,ExtensionKind::TRACK_V4,uris).await {
        for (uri,bytes) in found {
            if let Ok(track)=TrackData::parse_from_bytes(&bytes){
                let ms=track.duration();
                if ms>0 {lengths.insert(uri,ms as u64);}
            }
        }
    }
    lengths
}
/// The cover of a playlist without its own picture: its first track's album.
pub async fn playlist_cover(session: &Session,uri: &str)->Option<String> {
    let disk=crate::browse_cache::DiskCache::for_account(&session.username());
    let key=format!("cover-v8:{uri}");
    let saved=disk.load(&key);
    if let Some(hit)=saved.as_ref() {
        let ttl=if hit.value.as_str().unwrap_or_default().is_empty(){600}else{86400};
        if hit.fresh(crate::browse_cache::now(),ttl){return hit.value.as_str().filter(|s|!s.is_empty()).map(str::to_owned);}
    }
    let result=tokio::time::timeout(Duration::from_secs(15),fetch_playlist_cover(session,uri)).await;
    match result {
        Ok(Ok(address))=>{disk.store(&key,&json!(address.clone().unwrap_or_default()));address}
        _=>saved.and_then(|hit|hit.value.as_str().filter(|s|!s.is_empty()).map(str::to_owned)),
    }
}
async fn fetch_playlist_cover(session: &Session,uri: &str)->Result<Option<String>,String> {
    let id=SpotifyId::from_base62(uri.strip_prefix("spotify:playlist:").ok_or("Invalid playlist URI")?).map_err(|e|e.to_string())?;
    let endpoint=format!("/playlist/v2/playlist/{}?decorate=attributes,length&from=0&length=4",id.to_base62().map_err(|e|e.to_string())?);
    let mut uris=Vec::new();
    if let Ok(bytes)=session.spclient().request(&http::Method::GET,&endpoint,None,None).await {
        if let Ok(page)=SelectedListContent::parse_from_bytes(&bytes) {
            if let Some(address)=playlist_picture(&page){return Ok(Some(address));}
            uris=page.contents.get_or_default().items.iter().map(|i|i.uri().to_owned())
                .filter(|u|u.starts_with("spotify:track:")).take(4).collect();
        }
    }
    if let Ok(context)=session.spclient().get_context(uri).await {
        for key in ["image_url","image","picture"] {
            if let Some(address)=context.metadata.get(key).and_then(|value|picture_url(value)){return Ok(Some(address));}
        }
        if uris.is_empty(){
            uris=context.pages.into_iter().flat_map(|page|page.tracks).filter_map(|track|track.uri)
                .filter(|uri|uri.starts_with("spotify:track:")).take(4).collect();
        }
    }
    if uris.is_empty(){return Ok(None);}
    let found=batch(session,ExtensionKind::TRACK_V4,&uris).await?;
    Ok(uris.iter().find_map(|uri| {
        let track=TrackData::parse_from_bytes(found.get(uri)?).ok()?;
        group_address(track.album.as_ref().and_then(|a|a.cover_group.as_ref()))
    }))
}
// collection2v2 PageRequest/PageResponse. librespot ships the .proto but does not
// compile it, and the messages are small enough to encode by hand.
fn put_varint(out: &mut Vec<u8>,mut v: u64){while v>=0x80{out.push(v as u8|0x80);v>>=7;}out.push(v as u8);}
fn put_text(out: &mut Vec<u8>,field: u64,text: &str){
    if text.is_empty(){return;}
    put_varint(out,field<<3|2);put_varint(out,text.len() as u64);out.extend_from_slice(text.as_bytes());
}
fn page_request(username: &str,set: &str,token: &str,limit: u64)->Vec<u8> {
    let mut out=Vec::new();put_text(&mut out,1,username);put_text(&mut out,2,set);put_text(&mut out,3,token);
    put_varint(&mut out,4<<3);put_varint(&mut out,limit);out
}
struct Reader<'a>{data:&'a [u8],at:usize}
impl<'a> Reader<'a> {
    fn varint(&mut self)->Option<u64>{
        let mut v=0u64;
        for shift in (0..64).step_by(7){let b=*self.data.get(self.at)?;self.at+=1;v|=u64::from(b&0x7f)<<shift;if b<0x80{return Some(v);}}
        None
    }
    /// (field, wire type, varint value or length-delimited bytes)
    fn field(&mut self)->Option<(u64,Result<u64,&'a [u8]>)> {
        if self.at>=self.data.len(){return None;}
        let key=self.varint()?;
        match key&7 {
            0=>Some((key>>3,Ok(self.varint()?))),
            2=>{let n=self.varint()? as usize;let end=self.at.checked_add(n).filter(|&e|e<=self.data.len())?;
                let bytes=&self.data[self.at..end];self.at=end;Some((key>>3,Err(bytes)))}
            1=>{self.at+=8;Some((key>>3,Ok(0)))}
            5=>{self.at+=4;Some((key>>3,Ok(0)))}
            _=>None,
        }
    }
}
/// (items as (uri, added_at, removed), next page token)
fn page_response(data: &[u8])->(Vec<(String,u64,bool)>,String) {
    let (mut items,mut next)=(Vec::new(),String::new());
    let mut r=Reader{data,at:0};
    while let Some((field,value))=r.field() {
        match (field,value) {
            (1,Err(bytes))=>{
                let (mut uri,mut added,mut removed)=(String::new(),0,false);
                let mut item=Reader{data:bytes,at:0};
                while let Some((f,v))=item.field() {
                    match (f,v){(1,Err(b))=>uri=String::from_utf8_lossy(b).into_owned(),(2,Ok(n))=>added=n,(3,Ok(n))=>removed=n!=0,_=>{}}
                }
                items.push((uri,added,removed));
            }
            (2,Err(bytes))=>next=String::from_utf8_lossy(bytes).into_owned(),
            _=>{}
        }
    }
    (items,next)
}
// Collection service uses its own protobuf media type, unlike metadata.
fn collection_headers()->http::HeaderMap {
    let mut headers=http::HeaderMap::new();
    let media=http::HeaderValue::from_static("application/vnd.collection-v2.spotify.proto");
    headers.insert(http::header::CONTENT_TYPE,media.clone());
    headers.insert(http::header::ACCEPT,media);
    headers
}
/// Saved albums from the user's collection (newest first), named in one batch.
async fn saved_albums(session: &Session)->Result<Value,String> {
    let mut saved=Vec::new();let mut token=String::new();let mut seen=0usize;
    for _ in 0..20 {
        let body=page_request(&session.username(),"collection",&token,300);
        let bytes=session.spclient().request(&http::Method::POST,"/collection/v2/paging",Some(collection_headers()),Some(&body)).await
            .map_err(|e|e.to_string())?;
        let (items,next)=page_response(&bytes);
        seen+=items.len();
        saved.extend(items.into_iter().filter(|(uri,_,removed)|!removed&&uri.starts_with("spotify:album:")).map(|(uri,at,_)|(at,uri)));
        log::info!("client library: collection page items={} saved_albums={} more={}",seen,saved.len(),!next.is_empty());
        if next.is_empty(){break;}
        if next==token{return Err("Collection paging repeated its token".into());}
        token=next;
    }
    saved.sort_by(|a,b|b.0.cmp(&a.0));
    let mut unique=std::collections::HashSet::new();
    let uris=saved.into_iter().map(|(_,u)|u).filter(|uri|unique.insert(uri.clone())).take(100).collect::<Vec<_>>();
    log::info!("client library: {} saved albums in {seen} collection items",uris.len());
    if uris.is_empty(){return Ok(json!({"items":[]}));}
    let found=batch(session,ExtensionKind::ALBUM_V4,&uris).await?;
    let items=uris.iter().filter_map(|uri| {
        let album=AlbumData::parse_from_bytes(found.get(uri)?).ok()?;
        let artists=album.artist.iter().map(|a|json!({"name":a.name()})).collect::<Vec<_>>();
        let images=group_address(album.cover_group.as_ref()).map(|url|vec![json!({"url":url,"width":300})]).unwrap_or_default();
        Some(json!({"album":{"uri":uri,"name":album.name(),"artists":artists,"images":images,"year":album.date.as_ref().map_or(0,|d|d.year())}}))
    }).collect::<Vec<_>>();
    Ok(json!({"items":items}))
}
async fn fetch(session: &Session,path: &str)->Result<Value,String> {
    let param=|key:&str|path.split_once('?').into_iter().flat_map(|(_,q)|q.split('&')).filter_map(|p|p.split_once('=')).find(|(k,_)|*k==key).and_then(|(_,v)|v.parse::<usize>().ok());
    let offset=param("offset").unwrap_or(0).min(20000);let limit=param("limit").unwrap_or(100).clamp(1,100);
    if path=="library/recents"{return crate::native_library::fetch(session).await;}
    if path.starts_with("me/albums"){return saved_albums(session).await;}
    if path.starts_with("me/playlists") {
        let mut items=Vec::new();
        for from in (0..600).step_by(120) {
            let bytes=session.spclient().get_rootlist(from,Some(120)).await.map_err(|e|e.to_string())?;
            let page=SelectedListContent::parse_from_bytes(&bytes).map_err(|e|e.to_string())?;
            let count=page.contents.get_or_default().items.len();
            let truncated=page.contents.get_or_default().truncated();
            items.extend(root_items(&page));
            if !truncated||count<120{break;}
        }
        log::info!("client library: {} root-list playlists",items.len());
        return Ok(json!({"items":items}));
    }
    if path.starts_with("me/tracks") {
        let uri=format!("spotify:user:{}:collection",session.username());
        let context=session.spclient().get_context(&uri).await.map_err(|e|e.to_string())?;
        let uris=context.pages.into_iter().flat_map(|p|p.tracks).filter_map(|t| {
            t.uri.filter(|s|!s.is_empty()).or_else(||t.gid.as_deref()
                .and_then(|g|SpotifyId::from_raw(g).ok())
                .and_then(|id|(SpotifyUri::Track{id}).to_uri().ok()))
        }).collect();
        return rows(session,uris,offset,limit).await;
    }
    if let Some(id)=path.strip_prefix("playlists/").and_then(|s|s.split('/').next()) {
        let id=SpotifyId::from_base62(id).map_err(|e|e.to_string())?;
        let bytes=session.spclient().get_playlist(&id).await.map_err(|e|e.to_string())?;
        let page=SelectedListContent::parse_from_bytes(&bytes).map_err(|e|e.to_string())?;
        let uris=page.contents.get_or_default().items.iter().map(|i|i.uri().to_string()).collect();
        return rows(session,uris,offset,limit).await;
    }
    if let Some(id)=path.strip_prefix("albums/").and_then(|s|s.split('/').next()) {
        let uri=SpotifyUri::from_uri(&format!("spotify:album:{id}")).map_err(|e|e.to_string())?;
        let album=Album::get(session,&uri).await.map_err(|e|e.to_string())?;
        return rows(session,album.tracks().filter_map(|u|u.to_uri().ok()).collect(),offset,limit).await;
    }
    Err("Unsupported client library request".into())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn collection_paging_round_trips_by_hand() {
        let request=page_request("user","collection","",300);
        assert_eq!(request,[10,4,b'u',b's',b'e',b'r',18,10,b'c',b'o',b'l',b'l',b'e',b'c',b't',b'i',b'o',b'n',32,0xac,0x02]);
        // Two items (an album, a removed track) and a next-page token.
        let mut item=Vec::new();put_text(&mut item,1,"spotify:album:A");put_varint(&mut item,2<<3);put_varint(&mut item,1700000000);
        let mut gone=Vec::new();put_text(&mut gone,1,"spotify:track:B");put_varint(&mut gone,3<<3);put_varint(&mut gone,1);
        let mut page=Vec::new();
        for i in [&item,&gone]{put_varint(&mut page,1<<3|2);put_varint(&mut page,i.len() as u64);page.extend_from_slice(i);}
        put_text(&mut page,2,"next");put_text(&mut page,3,"sync");
        let (items,next)=page_response(&page);
        assert_eq!(items,vec![("spotify:album:A".to_string(),1700000000,false),("spotify:track:B".to_string(),0,true)]);
        assert_eq!(next,"next");
        assert!(page_response(&[0x0a,0x50]).0.is_empty()); // truncated input is not a panic
    }
    #[test]
    fn collection_requests_negotiate_the_service_media_type() {
        let headers=collection_headers();
        assert_eq!(headers[http::header::CONTENT_TYPE],"application/vnd.collection-v2.spotify.proto");
        assert_eq!(headers[http::header::ACCEPT],headers[http::header::CONTENT_TYPE]);
    }
    #[test]
    fn image_ids_become_scdn_addresses() {
        assert_eq!(image_address(&[0xab;20]).unwrap(),format!("https://i.scdn.co/image/{}","ab".repeat(20)));
        assert!(image_address(&[1,2,3]).is_none());
    }
    #[test]
    fn spotify_playlist_artwork_supports_urls_image_uris_and_mosaics() {
        use librespot::protocol::playlist4_external::{ListAttributes,PictureSize};
        let id="ab".repeat(20);
        assert_eq!(picture_url(&format!("spotify:image:{id}")),Some(format!("https://i.scdn.co/image/{id}")));
        assert_eq!(picture_url(&format!("spotify:mosaic:{id}:{id}")),Some(format!("https://mosaic.scdn.co/300/{id}{id}")));
        assert!(picture_url("https://example.com/cover.jpg").is_none());
        assert!(picture_url("https://i.scdn.co/image/{file_id}").is_none());
        let mut attr=ListAttributes::new();
        attr.set_picture(format!("spotify:image:{id}").into_bytes());
        let mut page=SelectedListContent::new();page.attributes=Some(attr.clone()).into();
        assert_eq!(playlist_picture(&page),Some(format!("https://i.scdn.co/image/{id}")));
        let mut picture=PictureSize::new();picture.set_url("https://image-cdn-ak.spotifycdn.com/image/blend.jpg".into());
        attr.picture_size.push(picture);page.attributes=Some(attr).into();
        assert_eq!(playlist_picture(&page),Some("https://image-cdn-ak.spotifycdn.com/image/blend.jpg".into()));
        assert_eq!(image_address(b"https://misc.scdn.co/liked-songs/liked-songs-300.jpg"),Some(crate::native_library::LIKED_COVER.into()));
    }
    #[test]
    fn root_list_uses_decorated_names_and_skips_folder_markers() {
        use librespot::protocol::playlist4_external::{Item,ListItems,ListAttributes,MetaItem};
        let mut playlist=Item::new();playlist.set_uri("spotify:playlist:0123456789012345678901".into());
        let mut folder=Item::new();folder.set_uri("spotify:start-group:folder".into());
        let mut attr=ListAttributes::new();attr.set_name("My playlist".into());
        let mut meta=MetaItem::new();meta.attributes=Some(attr).into();meta.set_owner_username("Owner".into());
        let mut contents=ListItems::new();contents.set_pos(0);contents.set_truncated(false);
        contents.items=vec![playlist,folder];contents.meta_items=vec![meta,MetaItem::new()];
        let mut page=SelectedListContent::new();page.contents=Some(contents).into();
        let items=root_items(&page);assert_eq!(items.len(),1);
        assert_eq!(items[0]["name"],"My playlist");assert_eq!(items[0]["owner"]["display_name"],"Owner");
    }
}


/// Collection writes use the same authenticated protocol as collection paging.
pub async fn save(session:&Session,uri:&str,on:bool)->Result<(),String>{
    let mut body=Vec::new();put_text(&mut body,1,&session.username());put_text(&mut body,2,"collection");
    let mut item=Vec::new();put_text(&mut item,1,uri);
    if on {put_varint(&mut item,2<<3);put_varint(&mut item,crate::browse_cache::now());}
    else {put_varint(&mut item,3<<3);put_varint(&mut item,1);}
    put_varint(&mut body,3<<3|2);put_varint(&mut body,item.len() as u64);body.extend(item);
    session.spclient().request(&http::Method::POST,"/collection/v2/write",Some(collection_headers()),Some(&body)).await.map_err(|e|e.to_string())?;
    // Metadata and saved-library snapshots must be refreshed after a successful write.
    cache().lock().await.pages.clear();Ok(())
}
async fn contains(session:&Session,uri:&str)->Result<bool,String>{
    let mut body=Vec::new();put_text(&mut body,1,&session.username());put_text(&mut body,2,"collection");
    let mut item=Vec::new();put_text(&mut item,1,uri);put_varint(&mut body,3<<3|2);put_varint(&mut body,item.len() as u64);body.extend(item);
    let bytes=session.spclient().request(&http::Method::POST,"/collection/v2/contains",Some(collection_headers()),Some(&body)).await.map_err(|e|e.to_string())?;
    let mut r=Reader{data:&bytes,at:0};while let Some((f,v))=r.field(){if f==1{return Ok(match v{Ok(n)=>n!=0,Err(b)=>b.first().is_some_and(|b|*b!=0)});}}Ok(false)
}
pub async fn track_info(session:&Session,uri:&str)->Result<Value,String>{
    let found=batch(session,ExtensionKind::TRACK_V4,&[uri.to_owned()]).await?;
    let track=TrackData::parse_from_bytes(found.get(uri).ok_or("No track metadata")?).map_err(|e|e.to_string())?;
    let album_uri=track.album.as_ref().and_then(|a|SpotifyId::from_raw(a.gid()).ok()).and_then(|id|(SpotifyUri::Album{id}).to_uri().ok()).unwrap_or_default();
    let artists=track.artist.iter().map(|a|json!({"name":a.name(),"uri":SpotifyId::from_raw(a.gid()).ok().and_then(|id|(SpotifyUri::Artist{id}).to_uri().ok()).unwrap_or_default()})).collect::<Vec<_>>();
    Ok(json!({"album_uri":album_uri,"artists":artists,"liked":contains(session,uri).await.unwrap_or(false)}))
}
pub async fn list_info(session:&Session,uri:&str)->Result<Value,String>{
    let disk=crate::browse_cache::DiskCache::for_account(&session.username());
    let snapshot=disk.load("library-snapshot-v8").map(|v|v.value).unwrap_or_default();
    let saved=snapshot.as_array().into_iter().flatten().find(|v|v["uri"]==uri);
    let mut info=json!({"uri":uri,"kind":if uri.starts_with("spotify:album:"){"album"}else{"playlist"},
        "name":saved.and_then(|v|v["name"].as_str()).unwrap_or(if uri.ends_with(":collection"){"Liked Songs"}else{""}),
        "owner":"","description":"","year":0,"saved":saved.is_some(),"cover":""});
    if uri.starts_with("spotify:playlist:") {
        if let Some(owner)=saved.and_then(|v|v["subtitle"].as_str()).and_then(|s|s.split_once("\u{b7}")).map(|(_,s)|s.trim()).filter(|s|!s.is_empty()){
            info["owner"]=json!(owner);
        }
    }
    let mut picture=saved.and_then(|v|v["picture"].as_str()).unwrap_or("").to_owned();
    if uri.starts_with("spotify:album:"){
        let found=batch(session,ExtensionKind::ALBUM_V4,&[uri.to_owned()]).await?;
        let a=AlbumData::parse_from_bytes(found.get(uri).ok_or("No album metadata")?).map_err(|e|e.to_string())?;
        info["name"]=json!(a.name());info["owner"]=json!(a.artist.iter().map(|a|a.name()).collect::<Vec<_>>().join(", "));
        info["year"]=json!(a.date.as_ref().map_or(0,|d|d.year()));picture=a.cover_group.as_ref().and_then(|g|g.image.iter().min_by_key(|i|(i.width()-640).abs())).and_then(|i|image_address(i.file_id())).unwrap_or(picture);
    }else if uri.starts_with("spotify:playlist:"){
        let id=SpotifyId::from_base62(uri.rsplit(':').next().unwrap_or("")).map_err(|e|e.to_string())?;
        let bytes=session.spclient().get_playlist(&id).await.map_err(|e|e.to_string())?;
        let page=SelectedListContent::parse_from_bytes(&bytes).map_err(|e|e.to_string())?;
        let attr=page.attributes.get_or_default();info["name"]=json!(attr.name());info["description"]=json!(attr.description());
        if info["owner"].as_str().unwrap_or("").is_empty(){
            info["owner"]=json!(page.owner_username());
        }
        if let Some(url)=playlist_picture(&page){picture=url;}
    }
    info["cover"]=json!(crate::picture_file(session,&picture,"/user/data/spotifyps5/library-covers",1024).await.unwrap_or_default());Ok(info)
}
