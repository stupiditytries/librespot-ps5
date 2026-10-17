//! Kabuki's native provider and the playback permission shared with Connect.
//! UI transport never enters the PCM writer or the Service worker.
use std::sync::{Arc,atomic::{AtomicBool,Ordering}};
use std::time::{Duration,Instant};
use serde_json::{json,Value};
use tokio::{io::{AsyncBufReadExt,AsyncWriteExt,BufReader},net::TcpStream,sync::{broadcast,mpsc,watch}};
use crate::control::{State,Command};

static READY:AtomicBool=AtomicBool::new(false);
pub fn ready()->bool{READY.load(Ordering::Acquire)}
pub fn permit_play()->bool {
    if ready(){return true;}
    // Rate-limit notices, not the permission check. No shutdown timer.
    static LAST:std::sync::Mutex<Option<Instant>>=std::sync::Mutex::new(None);
    let mut last=LAST.lock().unwrap();
    if last.map_or(true,|at|at.elapsed()>=Duration::from_secs(3)) {
        *last=Some(Instant::now());
        log::warn!("Playback blocked: librespot needs Kabuki to run");
        #[cfg(target_os="freebsd")] unsafe {let payload=crate::toast::status_payload("librespot needs Kabuki to run","Launch Kabuki, then try casting again",std::time::SystemTime::now());
            if let Ok(payload)=std::ffi::CString::new(payload){librespot_kabuki_required(payload.as_ptr());}}
    }
    false
}
#[cfg(target_os="freebsd")]
extern "C"{fn librespot_kabuki_required(payload:*const std::ffi::c_char);}

fn text<'a>(v:&'a Value,key:&str)-> &'a str{v[key].as_str().unwrap_or("")}
fn content(tile:&str,data:Value)->Value{json!({"v":1,"content":{"tile":tile,"data":data}})}
async fn send(w:&mut tokio::net::tcp::OwnedWriteHalf,mut value:Value)->std::io::Result<()> {
    while value.to_string().len()>60*1024 {
        let mut removed=false;
        for level in ["top","sub"] {if let Some(rows)=value["content"]["data"][level]["rows"].as_array_mut(){if rows.pop().is_some(){removed=true;break;}}}
        if !removed{break;}
    }
    let line=value.to_string();
    if line.len()>64*1024{return Err(std::io::Error::new(std::io::ErrorKind::InvalidData,"Kabuki line exceeds limit"));}
    tokio::time::timeout(Duration::from_secs(2),async{w.write_all(line.as_bytes()).await?;w.write_all(b"\n").await}).await
        .map_err(|_|std::io::Error::new(std::io::ErrorKind::TimedOut,"Kabuki write timed out"))?
}
// A rejected optional image must not tear down playback permission. Kabuki has
// already removed that image from its card; suppress art for that card until the
// next real connection so repeat updates cannot accumulate broker strikes.
#[derive(Default)]
struct ArtworkPolicy { disabled: std::collections::HashSet<String> }
impl ArtworkPolicy {
    fn rejected(&mut self, error: &Value) -> bool {
        let tile = text(error, "tile");
        if text(error, "code") != "bad_art" || !matches!(tile, "LSPNOW" | "LSPQUE" | "LSPLIB") { return false; }
        if self.disabled.insert(tile.into()) {
            log::warn!("Kabuki rejected artwork for {tile}; continuing with placeholders");
        }
        true
    }
    fn prepare(&self, value: &mut Value) {
        if !self.disabled.contains(text(&value["content"], "tile")) { return; }
        fn clear(value: &mut Value) {
            match value {
                Value::Object(object) => {
                    if object.contains_key("art") { object.insert("art".into(), Value::Null); }
                    for child in object.values_mut() { clear(child); }
                }
                Value::Array(values) => { for child in values { clear(child); } }
                _ => {}
            }
        }
        clear(&mut value["content"]["data"]);
    }
}
async fn send_card(w: &mut tokio::net::tcp::OwnedWriteHalf, artwork: &ArtworkPolicy, mut value: Value) -> std::io::Result<()> {
    artwork.prepare(&mut value);
    send(w, value).await
}

#[derive(Default)]
struct Cards{library:Value,tracks:Value,open:String,busy:String,busy_tile:String,started:Option<Instant>,frozen:Option<Value>,desired:String,epoch:u64}
impl Cards {
    fn session(&mut self,epoch:u64)->bool{
        if self.epoch==epoch{return false;}
        *self=Self{epoch,..Self::default()};true
    }
    fn event(&mut self,j:&Value)->bool{
        // Old authenticated tasks may finish after a different account casts.
        if j["media_session"].as_u64().unwrap_or(0)!=self.epoch{return false;}
        if j["library"].is_object(){self.library=j["library"].clone();return true;}
        if j["tracks"].is_object()&&text(&j["tracks"],"uri")==self.open{self.tracks=j["tracks"].clone();return true;}
        if let Some(update)=j["cover_ready"].as_object(){if let Some(items)=self.library["items"].as_array_mut(){for item in items{if item["uri"]==update["uri"]{item["cover"]=update["cover"].clone();}}}return true;}
        false
    }
    fn queue(&self,s:&Value)->Value {
        let queue=self.frozen.as_ref().unwrap_or(&s["queue"]);
        let rows:Vec<Value>=queue.as_array().into_iter().flatten().take(100).enumerate().map(|(i,q)|json!({
            "key":format!("{}|{}",q["index"].as_u64().unwrap_or(i as u64),text(q,"uri")),
            "title":q["title"],"subtitle":q["artist"],"art":q["cover"]})).collect();
        content("LSPQUE",json!({"busy":if self.busy_tile=="LSPQUE"{&self.busy}else{""},"top":{
            "heading":if rows.is_empty(){""}else{"Up next"},"rows":rows,"loading":!queue.is_array(),
            "empty":{"title":"Nothing queued","detail":"Add songs from the Spotify app"}}}))
    }
    fn library(&self)->Value {
        let rows:Vec<Value>=self.library["items"].as_array().into_iter().flatten().take(150).map(|i|json!({
            "key":i["uri"],"title":i["name"],"subtitle":i["subtitle"],"art":i["cover"],"opens":true})).collect();
        let mut data=json!({"session":self.epoch.to_string(),"busy":if self.busy_tile=="LSPLIB"{&self.busy}else{""},"top":{"rows":rows,
            "loading":!self.library.is_object(),"empty":{"title":"Nothing saved yet","detail":text(&self.library,"error")}},"sub_key":null});
        if !self.open.is_empty(){
            let header=self.library["items"].as_array().into_iter().flatten().find(|i|text(i,"uri")==self.open);
            let known=text(&self.tracks,"uri")==self.open;
            let rows:Vec<Value>=self.tracks["items"].as_array().into_iter().flatten().take(150).map(|i|json!({
                "key":i["uri"],"title":i["title"],"subtitle":i["artist"]})).collect();
            data["sub_key"]=json!(self.open);data["sub"]=json!({"title":header.map(|i|text(i,"name")).unwrap_or(""),
                "art":header.map(|i|text(i,"cover")).unwrap_or(""),"covers":false,"loading":!known,"rows":if known{rows}else{vec![]},
                "empty":{"title":"No songs here","detail":text(&self.tracks,"error")}});
        }
        content("LSPLIB",data)
    }
    fn command(&mut self,a:&Value,s:&Value)->Option<Command> {
        let tile=text(a,"tile");let action=text(a,"action");let key=text(a,"key");let value=a["value"].as_i64().unwrap_or(0);
        let cmd=if tile=="LSPNOW" {
            match action {
                "play_pause"|"previous"|"next"=>json!({"v":1,"cmd":action}),
                "seek"=>json!({"v":1,"cmd":"seek","ms":value.max(0)}),
                "volume"=>json!({"v":1,"cmd":"volume","value":value.clamp(0,100)}),
                "shuffle"=>json!({"v":1,"cmd":"shuffle","on":value!=0}),
                "repeat"=>json!({"v":1,"cmd":"repeat","mode":if value==2{"one"}else if value==1{"all"}else{"off"}}),_=>return None
            }
        }else if tile=="LSPQUE"&&action=="select" {
            let (index,uri)=key.split_once('|')?;
            self.busy=key.into();self.busy_tile=tile.into();self.started=Some(Instant::now());self.desired=uri.into();self.frozen=Some(s["queue"].clone());
            json!({"v":1,"cmd":"play_queued","uri":uri,"index":index.parse::<u32>().ok()?})
        }else if tile=="LSPLIB" {
            match action {
                "open"=>{self.open=key.into();self.tracks=Value::Null;json!({"v":1,"cmd":"open_list","uri":key})},
                "close"=>{self.open.clear();return None;},
                "select"=>{self.busy=key.into();self.busy_tile=tile.into();self.started=Some(Instant::now());self.desired=key.into();json!({"v":1,"cmd":"play","context":self.open,"track":key})},
                _=>return None
            }
        }else{return None;};
        crate::control::parse(&cmd.to_string())
    }
    fn started(&mut self,s:&Value)->bool {
        let heard=self.started.is_some_and(|at|at.elapsed()>Duration::from_millis(300))&&text(s,"uri")==self.desired&&
            s["playing"]==true&&s["pending"]==false;
        if heard||self.started.is_some_and(|at|at.elapsed()>Duration::from_secs(15)) {
            self.busy.clear();self.busy_tile.clear();self.frozen=None;self.started=None;
        }
        heard
    }
}
pub fn start(state:Arc<watch::Sender<State>>,lines:broadcast::Sender<String>,commands:mpsc::UnboundedSender<Command>){
    tokio::spawn(async move {
        loop {
            READY.store(false,Ordering::Release);state.send_modify(|s|s.waiting_for="kabuki".into());
            match connection(state.clone(),lines.clone(),commands.clone()).await {Ok(())=>{},Err(e)=>log::warn!("Kabuki provider disconnected: {e}")}
            READY.store(false,Ordering::Release);state.send_modify(|s|s.waiting_for="kabuki".into());
            #[cfg(feature="usb-autostart")] crate::transport::intent(false);
            let _=commands.send(Command::Pause);
            tokio::time::sleep(Duration::from_secs(2)).await;
        }
    });
}
// Keep partial input across select cancellation and cap it before allocation grows.
async fn broker_line(read:&mut BufReader<tokio::net::tcp::OwnedReadHalf>,line:&mut Vec<u8>)->std::io::Result<usize>{
    loop {
        let bytes=read.fill_buf().await?;
        if bytes.is_empty(){return if line.is_empty(){Ok(0)}else{Err(std::io::Error::new(std::io::ErrorKind::UnexpectedEof,"Partial Kabuki line"))};}
        let end=bytes.iter().position(|b|*b==b'\n');let take=end.map_or(bytes.len(),|n|n+1);
        if line.len()+take>65536{return Err(std::io::Error::new(std::io::ErrorKind::InvalidData,"Kabuki message too large"));}
        line.extend_from_slice(&bytes[..take]);read.consume(take);
        if end.is_some(){return Ok(line.len());}
    }
}
async fn connection(state:Arc<watch::Sender<State>>,lines:broadcast::Sender<String>,commands:mpsc::UnboundedSender<Command>)->std::io::Result<()> {
    connection_at(state, lines, commands, std::net::SocketAddr::from(([127,0,0,1],38590))).await
}
async fn connection_at(state:Arc<watch::Sender<State>>,lines:broadcast::Sender<String>,commands:mpsc::UnboundedSender<Command>,address:std::net::SocketAddr)->std::io::Result<()> {
    let stream=TcpStream::connect(address).await?;
    let (read,mut write)=stream.into_split();let mut read=BufReader::new(read);let mut line=Vec::new();
    send(&mut write,json!({"v":1,"hello":{"provider":"librespot","name":"librespot-ps5","version":"1.0.0",
        "native":true,"art_dirs":["/user/data/spotifyps5/covers","/user/data/spotifyps5/queue-covers","/user/data/spotifyps5/library-covers"],
        "tiles":[{"id":"LSPNOW","card":"media","title":"Now Playing","icon":"icon:iconid_music","priority":100},
        {"id":"LSPQUE","card":"list","title":"Queue","icon":"icon:iconid_list","priority":101},
        {"id":"LSPLIB","card":"list","title":"Library","icon":"icon:iconid_app_library_media","priority":102}]}})).await?;
    let n=tokio::time::timeout(Duration::from_secs(3),broker_line(&mut read,&mut line)).await
        .map_err(|_|std::io::Error::new(std::io::ErrorKind::TimedOut,"Kabuki welcome timed out"))??;
    let welcome:Value=serde_json::from_slice(&line).unwrap_or_default();
    if n==0||welcome["v"]!=1||!welcome["welcome"]["templates"].as_array().is_some_and(|t|t.iter().any(|v|v=="media")&&t.iter().any(|v|v=="list")) {
        return Err(std::io::Error::new(std::io::ErrorKind::InvalidData,"Kabuki welcome incompatible"));
    }
    line.clear();
    let mut snapshots=state.subscribe();let mut events=lines.subscribe();let mut cards=Cards::default();
    READY.store(true,Ordering::Release);state.send_modify(|s|s.waiting_for.clear());
    log::info!("Kabuki native provider ready");let _=commands.send(Command::Library);
    let mut tick=tokio::time::interval(Duration::from_millis(100));tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    let mut previous=String::new();let mut library_dirty=true;let mut authenticated=false;let mut observed_epoch=0;let mut artwork=ArtworkPolicy::default();
    loop {
        tokio::select! {
            _=tick.tick()=>{
                let parsed:Value=serde_json::from_str(&snapshots.borrow().app_line()).unwrap_or_default();let s=&parsed["state"];
                let epoch=s["media_session"].as_u64().unwrap_or(0);
                if epoch!=observed_epoch {observed_epoch=epoch;authenticated=false;previous.clear();library_dirty=true;
                    cards.session(epoch);log::info!("Kabuki library session changed; cleared previous account views");}
                if s["connected"]==true&&!authenticated {let _=commands.send(Command::Library);}authenticated=s["connected"]==true;
                if cards.started(s){send(&mut write,json!({"v":1,"show":{"tile":"LSPNOW"}})).await?;library_dirty=true;}
                let mut media=s.clone();media["subtitle"]=s["artist"].clone();media["art"]=s["cover"].clone();
                if text(s,"title").is_empty(){media["title"]=json!("Nothing playing");media["subtitle"]=json!("Pick this console in the Spotify app");}
                let current=media.to_string();if current!=previous {send_card(&mut write,&artwork,content("LSPNOW",media)).await?;send_card(&mut write,&artwork,cards.queue(s)).await?;previous=current;}
                if library_dirty{send_card(&mut write,&artwork,cards.library()).await?;library_dirty=false;}
                let _=snapshots.borrow_and_update();
            }
            event=events.recv()=>{match event {
                Ok(line)=>{if let Ok(j)=serde_json::from_str::<Value>(&line){
                    let epoch=snapshots.borrow().media_session;
                    if cards.session(epoch){library_dirty=true;previous.clear();}
                    library_dirty|=cards.event(&j);
                }},Err(broadcast::error::RecvError::Lagged(_))=>{let _=commands.send(Command::Library);},Err(_)=>return Ok(())
            }}
            n=broker_line(&mut read,&mut line)=>{
                if n?==0{return Ok(());}if line.len()>65536{return Err(std::io::Error::new(std::io::ErrorKind::InvalidData,"Kabuki message too large"));}
                if let Ok(j)=serde_json::from_slice::<Value>(&line){
                    if j["error"].is_object(){
                        if artwork.rejected(&j["error"]) { previous.clear();library_dirty=true; }
                        else { return Err(std::io::Error::new(std::io::ErrorKind::InvalidData,j["error"].to_string())); }
                    }
                    if j["shown"]["tile"]=="LSPLIB"{let _=commands.send(Command::Library);}
                    if j["action"].is_object(){let s:Value=serde_json::from_str(&snapshots.borrow().app_line()).unwrap_or_default();
                        if let Some(cmd)=cards.command(&j["action"],&s["state"]){let _=commands.send(cmd);}library_dirty=true;previous.clear();}
                }
                line.clear();
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    static READY_TEST: std::sync::Mutex<()> = std::sync::Mutex::new(());
    #[test]
    fn account_switch_discards_nested_lists_and_late_replies(){
        let mut c=Cards::default();assert!(c.session(10));
        assert!(c.event(&json!({"media_session":10,"library":{"items":[{"uri":"old-list","name":"Old"}]}})));
        c.open="old-list".into();c.busy="old-song".into();c.frozen=Some(json!([]));
        assert!(c.session(11));assert!(c.open.is_empty()&&c.busy.is_empty()&&c.library.is_null()&&c.frozen.is_none());
        assert!(!c.event(&json!({"media_session":10,"library":{"items":[]}})));
        assert!(c.event(&json!({"media_session":11,"library":{"items":[{"uri":"new-list","name":"New"}]}})));
        c.open="new-list".into();assert!(!c.event(&json!({"media_session":11,"tracks":{"uri":"old-list","items":[]}})));
        assert_eq!(c.library()["content"]["data"]["session"],"11");
        assert!(c.session(12)); // Returning to the first account is a new session too.
        assert!(!c.event(&json!({"media_session":10,"library":{"items":[]}})));
    }
    #[test]
    fn missing_kabuki_always_blocks_playback(){
        let _guard=READY_TEST.lock().unwrap();
        READY.store(false,Ordering::Release);assert!(!permit_play());assert!(!permit_play());
    }
    #[test]
    fn artwork_fallback_is_scoped_and_does_not_hide_protocol_errors() {
        let mut policy=ArtworkPolicy::default();
        assert!(!policy.rejected(&json!({"code":"unsupported_version","tile":"LSPLIB"})));
        assert!(!policy.rejected(&json!({"code":"bad_art","tile":"OTHER1"})));
        assert!(policy.rejected(&json!({"code":"bad_art","tile":"LSPLIB"})));
        let mut library=content("LSPLIB",json!({"top":{"art":"cover.jpg","rows":[{"key":"track","title":"Song","art":"row.jpg"}]},
            "sub":{"art":"sub.jpg","rows":[{"art":"nested.jpg"}]}}));
        policy.prepare(&mut library);
        assert!(library["content"]["data"]["top"]["art"].is_null());
        assert!(library["content"]["data"]["top"]["rows"][0]["art"].is_null());
        assert!(library["content"]["data"]["sub"]["rows"][0]["art"].is_null());
        assert_eq!(library["content"]["data"]["top"]["rows"][0]["key"],"track");
        let mut now=content("LSPNOW",json!({"art":"good.jpg","playing":true}));
        policy.prepare(&mut now);assert_eq!(now["content"]["data"]["art"],"good.jpg");
    }
    #[tokio::test]
    async fn artwork_warning_keeps_provider_connected_and_permission_ready() {
        let _guard=READY_TEST.lock().unwrap();
        let listener=tokio::net::TcpListener::bind(("127.0.0.1",0)).await.unwrap();
        let address=listener.local_addr().unwrap();
        let state=Arc::new(watch::channel(State::default()).0);
        let (lines,_)=broadcast::channel(16);let (commands,mut received)=mpsc::unbounded_channel();
        let client=tokio::spawn(connection_at(state,lines,commands,address));
        let (socket,_)=listener.accept().await.unwrap();
        let (read,mut write)=socket.into_split();let mut read=BufReader::new(read);let mut line=Vec::new();
        broker_line(&mut read,&mut line).await.unwrap();
        assert!(serde_json::from_slice::<Value>(&line).unwrap()["hello"].is_object());line.clear();
        write.write_all(b"{\"v\":1,\"welcome\":{\"templates\":[\"media\",\"list\"]}}\n").await.unwrap();
        loop {
            broker_line(&mut read,&mut line).await.unwrap();
            let j:Value=serde_json::from_slice(&line).unwrap();line.clear();
            if j["content"]["tile"]=="LSPLIB" {break;}
        }
        write.write_all(b"{\"v\":1,\"error\":{\"code\":\"bad_art\",\"tile\":\"LSPLIB\"}}\n").await.unwrap();
        loop {
            broker_line(&mut read,&mut line).await.unwrap();
            let j:Value=serde_json::from_slice(&line).unwrap();line.clear();
            if j["content"]["tile"]=="LSPLIB" {
                assert!(j["content"]["data"]["top"]["art"].is_null());break;
            }
        }
        assert!(ready());assert!(!client.is_finished());
        while let Ok(command)=received.try_recv(){assert!(!matches!(command,Command::Pause));}
        write.write_all(b"{\"v\":1,\"error\":{\"code\":\"unsupported_version\"}}\n").await.unwrap();
        assert!(tokio::time::timeout(Duration::from_secs(2),client).await.unwrap().unwrap().is_err());
        READY.store(false,Ordering::Release);
    }
    #[tokio::test]
    async fn library_session_reaches_broker_and_rejects_old_account() {
        let _guard=READY_TEST.lock().unwrap();
        let listener=tokio::net::TcpListener::bind(("127.0.0.1",0)).await.unwrap();
        let address=listener.local_addr().unwrap();
        let mut initial=State::default();initial.media_session=1_792_206_880_925;initial.connected=true;
        let state=Arc::new(watch::channel(initial).0);
        let (lines,_)=broadcast::channel(16);let (commands,mut received)=mpsc::unbounded_channel();
        let client=tokio::spawn(connection_at(state.clone(),lines.clone(),commands,address));
        let (socket,_)=listener.accept().await.unwrap();
        let (read,mut write)=socket.into_split();let mut read=BufReader::new(read);let mut buffer=Vec::new();
        broker_line(&mut read,&mut buffer).await.unwrap();buffer.clear();
        write.write_all(b"{\"v\":1,\"welcome\":{\"templates\":[\"media\",\"list\"]}}\n").await.unwrap();
        received.recv().await.unwrap();
        async fn loaded(read:&mut BufReader<tokio::net::tcp::OwnedReadHalf>,buffer:&mut Vec<u8>)->Value {
            tokio::time::timeout(Duration::from_secs(3),async {
                loop {
                    broker_line(read,buffer).await.unwrap();let j:Value=serde_json::from_slice(buffer).unwrap();buffer.clear();
                    if j["content"]["tile"]=="LSPLIB"&&j["content"]["data"]["top"]["loading"]==false{return j;}
                }
            }).await.unwrap()
        }
        let first=state.borrow().media_session;
        lines.send(json!({"media_session":first,"library":{"items":[{"uri":"first","name":"First"}]}}).to_string()).unwrap();
        let a=loaded(&mut read,&mut buffer).await;
        assert_eq!(a["content"]["data"]["session"],first.to_string());
        assert_eq!(a["content"]["data"]["top"]["rows"][0]["key"],"first");
        let second=first+1;state.send_modify(|s|s.media_session=second);
        lines.send(json!({"media_session":first,"library":{"items":[{"uri":"stale","name":"Stale"}]}}).to_string()).unwrap();
        lines.send(json!({"media_session":second,"library":{"items":[{"uri":"second","name":"Second"}]}}).to_string()).unwrap();
        let b=loaded(&mut read,&mut buffer).await;
        assert_eq!(b["content"]["data"]["session"],second.to_string());
        assert_eq!(b["content"]["data"]["top"]["rows"][0]["key"],"second");
        assert!(b["content"]["data"]["sub_key"].is_null());
        client.abort();let _=client.await;READY.store(false,Ordering::Release);
    }
    #[tokio::test]
    async fn fragmented_line_survives_timer_cancellation_and_is_bounded(){
        let listener=tokio::net::TcpListener::bind(("127.0.0.1",0)).await.unwrap();
        let address=listener.local_addr().unwrap();
        let server=tokio::spawn(async move{
            let(mut socket,_)=listener.accept().await.unwrap();
            socket.write_all(b"{\"v\":").await.unwrap();
            tokio::time::sleep(Duration::from_millis(40)).await;
            socket.write_all(b"1}\n").await.unwrap();
            socket.write_all(&vec![b'x';65537]).await.unwrap();
        });
        let stream=TcpStream::connect(address).await.unwrap();
        let(read,_write)=stream.into_split();let mut read=BufReader::new(read);let mut line=Vec::new();
        assert!(tokio::time::timeout(Duration::from_millis(15),broker_line(&mut read,&mut line)).await.is_err());
        assert_eq!(&line,b"{\"v\":");
        broker_line(&mut read,&mut line).await.unwrap();
        assert_eq!(serde_json::from_slice::<Value>(&line).unwrap()["v"],1);
        line.clear();assert!(broker_line(&mut read,&mut line).await.is_err());assert!(line.len()<=65536);
        server.await.unwrap();
    }
}
