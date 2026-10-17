//! Optional client metadata. Never owns playback or changes the PCM path.
use librespot::core::{session::Session, SpotifyUri};
use serde_json::{json,Value};
use std::{collections::HashMap,sync::{Arc,OnceLock,atomic::{AtomicU64,Ordering}},time::{Duration,SystemTime,UNIX_EPOCH}};
use tokio::sync::{Mutex,Semaphore,broadcast,watch};
use http_body_util::BodyExt;
use sha1::{Digest,Sha1};
const MAX_REPLY:usize=256*1024;
const MAX_ASSET:usize=16*1024*1024;
const ASSET_DIR:&str="/user/data/spotifyps5/canvas";
fn now()->u64{SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs()}
struct Entry{until:u64,value:Value}
type Cache=Mutex<HashMap<String,Arc<Mutex<Option<Entry>>>>>;
fn cache()->&'static Cache{static C:OnceLock<Cache>=OnceLock::new();C.get_or_init(||Mutex::new(HashMap::new()))}
fn permits()->&'static Semaphore{static S:Semaphore=Semaphore::const_new(2);&S}
static COOLDOWN:AtomicU64=AtomicU64::new(0);
fn failure(status:&str,reason:&str)->Value{json!({"status":status,"reason":reason})}
async fn limited(mut body:hyper::body::Incoming,max:usize)->Result<bytes::Bytes,String>{
    let mut bytes=Vec::new();
    while let Some(frame)=body.frame().await{
        let frame=frame.map_err(|_|"Response read failed")?;
        if let Ok(data)=frame.into_data(){
            if bytes.len().saturating_add(data.len())>max{return Err("Response too large".into());}
            bytes.extend_from_slice(&data);
        }
    }
    Ok(bytes.into())
}
async fn raw(session:&Session,path:&str,body:Option<Vec<u8>>)->Result<(u16,bytes::Bytes),String>{
    if COOLDOWN.load(Ordering::Relaxed)>now(){return Err("rate_limited".into());}
    let _permit=permits().acquire().await.map_err(|_|"Feature worker unavailable")?;
    if COOLDOWN.load(Ordering::Relaxed)>now(){return Err("rate_limited".into());}
    let started=std::time::Instant::now();
    let result=tokio::time::timeout(Duration::from_secs(8),async{
        let base=session.spclient().base_url().await.map_err(|_|"Service resolution failed")?;
        let token=session.login5().auth_token().await.map_err(|_|"Session authorization failed")?;
        let mut req=http::Request::builder().method(if body.is_some(){"POST"}else{"GET"}).uri(format!("{base}{path}"))
            .header("authorization",format!("{} {}",token.token_type,token.access_token))
            .header("accept",if body.is_some(){"application/x-protobuf"}else{"application/json"})
            .header("app-platform","WebPlayer");
        if let Ok(client)=session.spclient().client_token().await{req=req.header("client-token",client);}
        if body.is_some(){req=req.header("content-type","application/x-protobuf");}
        let request=req.body(bytes::Bytes::from(body.unwrap_or_default())).map_err(|_|"Invalid feature request")?;
        let response=session.http_client().request_fut(request).map_err(|_|"Feature network unavailable")?.await.map_err(|_|"Feature network unavailable")?;
        let status=response.status().as_u16();
        if status==429{
            let retry=response.headers().get("retry-after").and_then(|v|v.to_str().ok()).and_then(|v|v.parse::<u64>().ok()).unwrap_or(60).max(1);
            COOLDOWN.fetch_max(now().saturating_add(retry),Ordering::Relaxed);
        }
        let body=limited(response.into_body(),MAX_REPLY).await?;
        log::info!("optional media status={} bytes={} elapsed_ms={}",status,body.len(),started.elapsed().as_millis());
        Ok((status,body))
    }).await.map_err(|_|"Feature request timed out".to_string())?;
    result
}
fn colors(v:&Value)->Value{
    let color=|name:&str,fallback:[f64;3]|{
        v[name].as_i64().map(|n|{let n=n as u32;json!([((n>>16)&255) as f64/255.,((n>>8)&255) as f64/255.,(n&255) as f64/255.])}).unwrap_or(json!(fallback))
    };
    json!({"background":color("background",[0.13,0.15,0.2]),"text":color("text",[0.55,0.58,0.65]),"highlight":color("highlightText",[1.,1.,1.])})
}
fn normalize_lyrics(v:&Value)->Result<Value,String>{
    let lyrics=&v["lyrics"];
    let Some(rows)=lyrics["lines"].as_array()else{return Err("Lyrics response format changed".into())};
    if rows.is_empty(){return Ok(failure("none","No lyrics are available for this track"));}
    if rows.len()>1000{return Err("Lyrics are too large".into());}
    let mut synced=lyrics["syncType"]=="LINE_SYNCED";let mut prior=0;let mut lines=Vec::new();
    for row in rows{
        let words=row["words"].as_str().ok_or("Invalid lyric line")?;
        if words.len()>8192{return Err("Lyric line too large".into());}
        let at=match row["startTimeMs"].as_str().unwrap_or("0").parse::<u32>(){Ok(n) if n<=i32::MAX as u32=>n,_=>return Err("Invalid lyric timestamp".into())};
        if at<prior{synced=false;}prior=at;
        lines.push(json!({"start_ms":at,"words":words}));
    }
    if !synced{return Ok(failure("none",""));}
    let result=json!({"status":"ready","sync":"line","lines":lines,"colors":colors(&v["colors"]),
        "provider":lyrics["provider"].as_str().unwrap_or("").chars().take(128).collect::<String>(),"language":lyrics["language"].as_str().unwrap_or(""),"rtl":lyrics["isRtlLanguage"].as_bool().unwrap_or(false)});
    if result.to_string().len()>224*1024{return Err("Lyrics are too large".into());}
    Ok(result)
}
fn put(out:&mut Vec<u8>,tag:u8,text:&str){
    out.push(tag<<3|2);let mut n=text.len();while n>=128{out.push((n as u8)|128);n>>=7;}out.push(n as u8);out.extend_from_slice(text.as_bytes());
}
fn canvas_request(uri:&str)->Vec<u8>{let mut entity=Vec::new();put(&mut entity,1,uri);let mut out=vec![10];let mut n=entity.len();while n>=128{out.push(n as u8|128);n>>=7;}out.push(n as u8);out.extend(entity);out}
struct Wire<'a>{b:&'a[u8],at:usize}
impl<'a> Wire<'a>{
    fn var(&mut self)->Result<u64,String>{let mut n=0;for shift in (0..70).step_by(7){let b=*self.b.get(self.at).ok_or("Truncated protobuf")?;self.at+=1;if shift==63&&b>1{return Err("Protobuf overflow".into());}n|=u64::from(b&127)<<shift;if b&128==0{return Ok(n);}}Err("Protobuf overflow".into())}
    fn field(&mut self)->Result<Option<(u32,u64,&'a[u8])>,String>{
        if self.at==self.b.len(){return Ok(None);}
        let key=self.var()?;let tag=(key>>3) as u32;if tag==0{return Err("Invalid protobuf field".into());}
        let kind=key&7;
        if kind==0{return Ok(Some((tag,self.var()?,&[])));}
        let count=match kind{1=>8,2=>usize::try_from(self.var()?).map_err(|_|"Length overflow")?,5=>4,_=>return Err("Invalid protobuf wire type".into())};
        let end=self.at.checked_add(count).filter(|&n|n<=self.b.len()).ok_or("Truncated protobuf field")?;
        let b=&self.b[self.at..end];self.at=end;Ok(Some((tag,0,b)))
    }
}
fn canvas_metadata(bytes:&[u8],uri:&str)->Result<Option<(String,u64,u64)>,String>{
    let mut outer=Wire{b:bytes,at:0};let mut found=None;let mut ttl=300;
    while let Some((tag,n,b))=outer.field()?{
        if tag==2{ttl=if n<=i64::MAX as u64{n.min(3600)}else{0};}
        if tag!=1{continue;}
        let mut entity=Wire{b,at:0};let(mut url,mut identity,mut kind)=(String::new(),String::new(),0);
        while let Some((tag,n,b))=entity.field()?{match tag{2=>url=std::str::from_utf8(b).map_err(|_|"Invalid Canvas URL")?.into(),4=>kind=n,5=>identity=std::str::from_utf8(b).map_err(|_|"Invalid Canvas identity")?.into(),_=>{}}}
        if identity==uri&&!url.is_empty(){found=Some((url,kind));}
    }
    Ok(found.map(|(url,kind)|(url,kind,ttl)))
}
fn allowed_asset(url:&str)->bool{
    let Ok(uri)=url.parse::<http::Uri>()else{return false};
    uri.scheme_str()==Some("https")&&uri.port_u16().map_or(true,|p|p==443)&&
        uri.host().is_some_and(|h|["scdn.co","spotifycdn.com","spotifycdn.net"].iter().any(|d|h==*d||h.ends_with(&format!(".{d}"))))
}
async fn asset(session:&Session,url:&str)->Result<(String,usize),String>{
    if !allowed_asset(url){return Err("Canvas host unsupported".into());}
    let request=http::Request::get(url).body(bytes::Bytes::new()).map_err(|_|"Invalid Canvas asset")?;
    // No authorization or client token is sent to a CDN.
    let response=session.http_client().request_fut(request).map_err(|_|"Canvas download unavailable")?.await.map_err(|_|"Canvas download unavailable")?;
    if response.status()!=200{return Err("Canvas download failed".into());}
    let data=limited(response.into_body(),MAX_ASSET).await?;
    if data.len()<12{return Err("Empty Canvas asset".into());}
    let id=hex::encode(Sha1::digest(&data));std::fs::create_dir_all(ASSET_DIR).map_err(|_|"Canvas cache unavailable")?;
    let file=format!("{ASSET_DIR}/{id}");
    if !std::path::Path::new(&file).exists(){
        let mut entries=std::fs::read_dir(ASSET_DIR).map_err(|_|"Canvas cache unavailable")?.filter_map(Result::ok).filter_map(|e|{
            let m=e.metadata().ok()?;Some((m.modified().ok()?,m.len(),e.path()))}).collect::<Vec<_>>();
        entries.sort_by_key(|e|e.0);let mut total=entries.iter().map(|e|e.1).sum::<u64>();
        for (_,size,path) in entries{if total+data.len() as u64<=128*1024*1024{break;}if std::fs::remove_file(path).is_ok(){total=total.saturating_sub(size);}}
        let partial=format!("{file}.partial");
        std::fs::write(&partial,&data).map_err(|_|"Canvas cache is full")?;
        std::fs::rename(&partial,&file).map_err(|_|"Canvas cache write failed")?;
    }
    Ok((format!("/media/canvas/{id}"),data.len()))
}
async fn fetch(session:&Session,feature:&str,uri:&str)->Value{
    let Ok(SpotifyUri::Track{id})=SpotifyUri::from_uri(uri)else{return failure("unsupported","This feature is available for music tracks")};
    let request=if feature=="lyrics"{raw(session,&format!("/color-lyrics/v2/track/{}",id.to_base62().unwrap_or_default()),None).await}
        else{raw(session,"/canvaz-cache/v0/canvases",Some(canvas_request(uri))).await};
    let (status,body)=match request{Ok(v)=>v,Err(e)=>return failure(if e=="rate_limited"{"rate_limited"}else{"error"},&e)};
    match status{
        404=>return failure("none",if feature=="lyrics"{"Lyrics are unavailable for this track"}else{"Canvas is unavailable for this track"}),
        401|403=>return failure("error","Spotify did not permit this feature for the current session"),
        429=>return failure("rate_limited","Spotify asked this feature to wait. Try again later."),
        200=>{},_=>return failure("error","Spotify could not load this feature"),
    }
    if feature=="lyrics"{
        return serde_json::from_slice(&body).map_err(|_|"Lyrics response format changed".into()).and_then(|v|normalize_lyrics(&v)).unwrap_or_else(|e:String|failure("error",&e));
    }
    match canvas_metadata(&body,uri){
        Ok(None)=>failure("none","No Canvas is available for this track"),
        Ok(Some((url,kind,ttl)))=>{
            if kind>3{return failure("unsupported","This Canvas format is unsupported");}
            match tokio::time::timeout(Duration::from_secs(15),asset(session,&url)).await{
                Ok(Ok((path,bytes)))=>json!({"status":"ready","asset_path":path,"kind":if kind==0{"image"}else{"video"},"bytes":bytes,"ttl":ttl}),
                _=>failure("error","Canvas could not be downloaded"),
            }
        }
        Err(_)=>failure("error","Canvas response format changed"),
    }
}
pub async fn request(session:Session,feature:String,uri:String,client:String,id:u64,epoch:u64,generation:u64,panel:Arc<watch::Sender<crate::control::State>>,lines:broadcast::Sender<String>){
    let key=format!("{}\n{}\n{}",session.username(),feature,uri);
    let entry={let mut c=cache().lock().await;if c.len()>=32&&!c.contains_key(&key){c.clear();}c.entry(key).or_insert_with(||Arc::new(Mutex::new(None))).clone()};
    let mut cached=entry.lock().await;
    let value=if let Some(hit)=cached.as_ref().filter(|e|e.until>now()){hit.value.clone()}else{
        let value=fetch(&session,&feature,&uri).await;
        let duration=match value["status"].as_str(){Some("ready")=>if feature=="canvas"{value["ttl"].as_u64().unwrap_or(300).max(1)}else{1800},Some("none")=>300,_=>30};
        *cached=Some(Entry{until:now().saturating_add(duration),value:value.clone()});value
    };drop(cached);
    let state=panel.borrow();if state.uri!=uri||state.media_session!=epoch||state.media_generation!=generation{return;}drop(state);
    let mut value=value;value["uri"]=json!(uri);
    let send=|v:Value|{let mut reply=json!({"v":2,"client":client,"id":id,"session":epoch,"generation":generation});reply[&feature]=v;let _=lines.send(reply.to_string());};
    if feature=="lyrics"&&value["status"]=="ready"{
        let rows=value["lines"].as_array().cloned().unwrap_or_default();let revision=hex::encode(Sha1::digest(value.to_string().as_bytes()));
        for (index,chunk) in rows.chunks(5).enumerate(){let mut v=value.clone();v["lines"]=json!(chunk);v["offset"]=json!(index*5);v["total"]=json!(rows.len());v["complete"]=json!((index+1)*5>=rows.len());v["revision"]=json!(revision);send(v);}
    }else{send(value);}
}
pub async fn serve_asset(stream:&mut tokio::net::TcpStream,method:&str,path:&str,headers:&str)->std::io::Result<()>{
    use std::io::{Read,Seek,SeekFrom};use tokio::io::AsyncWriteExt;
    let id=path.strip_prefix("/media/canvas/").unwrap_or("");
    if id.len()!=40||!id.bytes().all(|b|b.is_ascii_hexdigit())||!matches!(method,"GET"|"HEAD"){
        stream.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").await?;return Ok(());
    }
    let file=format!("{ASSET_DIR}/{id}");
    let Ok(meta)=std::fs::symlink_metadata(&file)else{stream.write_all(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").await?;return Ok(())};
    if !meta.is_file()||meta.len()>MAX_ASSET as u64{return Ok(());}
    let size=meta.len();let mut start=0;let mut end=size.saturating_sub(1);let mut partial=false;
    for line in headers.lines(){
        if let Some(range)=line.to_ascii_lowercase().strip_prefix("range: bytes="){
            let parts=range.split_once('-');
            let valid=parts.and_then(|(a,b)|{
                if a.is_empty(){let n=b.parse::<u64>().ok()?.min(size);if n==0{return None;}Some((size-n,size-1))}
                else{let a=a.parse::<u64>().ok()?;let b=if b.is_empty(){size-1}else{b.parse::<u64>().ok()?};Some((a,b.min(size-1)))}
            }).filter(|&(a,b)|a<=b&&b<size);
            let Some((a,b))=valid else{stream.write_all(format!("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */{size}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").as_bytes()).await?;return Ok(())};
            start=a;end=b;partial=true;
        }
    }
    let length=end-start+1;let extra=if partial{format!("Content-Range: bytes {start}-{end}/{size}\r\n")}else{String::new()};
    stream.write_all(format!("HTTP/1.1 {}\r\nContent-Length: {length}\r\nAccept-Ranges: bytes\r\n{extra}Connection: close\r\n\r\n",if partial{"206 Partial Content"}else{"200 OK"}).as_bytes()).await?;
    if method=="HEAD"{return Ok(());}
    let mut file=std::fs::File::open(file)?;file.seek(SeekFrom::Start(start))?;let mut remaining=length;let mut buffer=[0u8;32768];
    while remaining>0{let want=remaining.min(buffer.len() as u64) as usize;let n=file.read(&mut buffer[..want])?;if n==0{break;}stream.write_all(&buffer[..n]).await?;remaining-=n as u64;}
    Ok(())
}
#[cfg(test)]mod tests{
 use super::*;
 #[test]fn lyrics_are_bounded_and_black_is_valid(){
  let v=normalize_lyrics(&json!({"lyrics":{"syncType":"LINE_SYNCED","lines":[{"startTimeMs":"0","words":"Synthetic"},{"startTimeMs":"1000","words":"Example"}]},"colors":{"background":-16777216}})).unwrap();
  assert_eq!(v["sync"],"line");assert_eq!(v["colors"]["background"],json!([0.,0.,0.]));
  assert!(normalize_lyrics(&json!({"lyrics":{"lines":[{"startTimeMs":"-1","words":"x"}]}})).is_err());
 }
 #[test]fn unsynced_and_out_of_order_lyrics_are_unavailable(){
  let plain=normalize_lyrics(&json!({"lyrics":{"syncType":"UNSYNCED","lines":[{"words":"Synthetic","startTimeMs":"0"}]}})).unwrap();
  assert_eq!(plain["status"],"none");assert!(plain.get("lines").is_none());
  let backwards=normalize_lyrics(&json!({"lyrics":{"syncType":"LINE_SYNCED","lines":[{"words":"A","startTimeMs":"1000"},{"words":"B","startTimeMs":"0"}]}})).unwrap();
  assert_eq!(backwards["status"],"none");
 }
 #[test]fn canvas_envelope_keeps_matching_entity_and_ttl(){
  let uri="spotify:track:0000000000000000000001";let mut entry=Vec::new();put(&mut entry,2,"https://canvaz.scdn.co/a.mp4");entry.extend([32,2]);put(&mut entry,5,uri);
  let mut wire=vec![10,entry.len() as u8];wire.extend(entry);wire.extend([16,60]);
  let c=canvas_metadata(&wire,uri).unwrap().unwrap();assert_eq!(c.1,2);assert_eq!(c.2,60);
  assert!(canvas_metadata(&wire,"wrong").unwrap().is_none());assert!(canvas_metadata(&wire[..5],uri).is_err());
  assert!(!canvas_request(uri).is_empty());assert!(allowed_asset(&c.0));assert!(!allowed_asset("https://evil.test/a"));assert!(!allowed_asset("http://canvaz.scdn.co/a"));
 }
}
