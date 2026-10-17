use std::{
    cmp::Reverse,
    collections::HashMap,
    fs::{self, File},
    io::{self, Read, Write},
    path::{Path, PathBuf},
    sync::{Arc, Mutex},
    time::SystemTime,
};

use priority_queue::PriorityQueue;
use thiserror::Error;

use crate::{Error, FileId, authentication::Credentials, error::ErrorKind};

const CACHE_LIMITER_POISON_MSG: &str = "cache limiter mutex should not be poisoned";

// ---------------------------------------------------------------------------------------
// Saved-login convenience storage. Device identifiers are not a hardware keystore.
// AEAD plus a random installation key protects a copied blob; privileged local
// code or a reader with both key material and the device identifier is outside
// this protection. No credentials are exposed by the app's loopback servers.
// ---------------------------------------------------------------------------------------

const SEAL_MAGIC: &[u8] = b"LSPS1";
const SEAL_V2: &[u8] = b"LSPS2";
const CREDENTIAL_LIMIT:u64=64*1024;
static STORE_STATUS:std::sync::atomic::AtomicU8=std::sync::atomic::AtomicU8::new(0);
/// unpaired, remembered, or storage_error; contains no account identifiers.
pub fn credentials_storage_status()->&'static str{match STORE_STATUS.load(std::sync::atomic::Ordering::Relaxed){1=>"remembered",2=>"storage_error",_=>"unpaired"}}
fn storage_result(ok:bool){STORE_STATUS.store(if ok{1}else{2},std::sync::atomic::Ordering::Relaxed);}

static SEAL_SECRET: std::sync::OnceLock<Vec<u8>> = std::sync::OnceLock::new();

/// Sets the secret the saved login is encrypted with. The first call decides.
pub fn set_credentials_secret(secret: &[u8]) {
    let _ = SEAL_SECRET.set(secret.to_vec());
}

fn seal_mac(key: &[u8], parts: &[&[u8]]) -> [u8; 20] {
    use hmac::{Hmac, Mac};
    let mut mac = <Hmac<sha1::Sha1> as Mac>::new_from_slice(key).expect("any key length");
    for part in parts {
        mac.update(part);
    }
    mac.finalize().into_bytes().into()
}

/// AES-128 in counter mode: the same call encrypts and decrypts.
fn seal_stream(secret: &[u8], nonce: &[u8; 16], data: &mut [u8]) {
    use aes::cipher::{BlockEncrypt, KeyInit, generic_array::GenericArray};
    let key = seal_mac(secret, &[b"encrypt"]);
    let cipher = aes::Aes128::new(GenericArray::from_slice(&key[..16]));
    for (index, chunk) in data.chunks_mut(16).enumerate() {
        let mut block = *nonce;
        for (byte, add) in block[8..].iter_mut().zip((index as u64).to_be_bytes()) {
            *byte ^= add;
        }
        let mut block = GenericArray::from(block);
        cipher.encrypt_block(&mut block);
        for (byte, key) in chunk.iter_mut().zip(block) {
            *byte ^= key;
        }
    }
}

/// Encrypts `plain` with `secret`: a marker, a random value, the encrypted bytes, and a
/// check value over all of it.
#[cfg(test)]
fn seal_legacy(secret: &[u8], plain: &[u8]) -> Vec<u8> {
    use rand::RngCore;
    let mut nonce = [0u8; 16];
    rand::rng().fill_bytes(&mut nonce);
    let mut body = plain.to_vec();
    seal_stream(secret, &nonce, &mut body);
    let mac_key = seal_mac(secret, &[b"check"]);
    let mut out = SEAL_MAGIC.to_vec();
    out.extend(nonce);
    out.extend(&body);
    let check = seal_mac(&mac_key, &[&out]);
    out.extend(check);
    out
}

/// The reverse of [`seal`]. `None` if the bytes were not sealed with this secret or have
/// been changed.
fn unseal_legacy(secret: &[u8], sealed: &[u8]) -> Option<Vec<u8>> {
    let head = SEAL_MAGIC.len() + 16;
    if sealed.len() < head + 20 || !sealed.starts_with(SEAL_MAGIC) {
        return None;
    }
    let (signed, check) = sealed.split_at(sealed.len() - 20);
    let mac_key = seal_mac(secret, &[b"check"]);
    let expected = seal_mac(&mac_key, &[signed]);
    // Compared without stopping at the first difference.
    if expected.iter().zip(check).fold(0u8, |sum, (a, b)| sum | (a ^ b)) != 0 {
        return None;
    }
    let nonce: [u8; 16] = signed[SEAL_MAGIC.len()..head].try_into().ok()?;
    let mut body = signed[head..].to_vec();
    seal_stream(secret, &nonce, &mut body);
    Some(body)
}


fn aead_key(secret:&[u8])->[u8;32]{use sha2::{Digest,Sha256};let mut h=Sha256::new();h.update(b"librespot-ps5 credentials v2\0");h.update(secret);h.finalize().into()}
/// Versioned XChaCha20-Poly1305 envelope, header authenticated as associated data.
pub fn seal(secret:&[u8],plain:&[u8])->Vec<u8>{
    use chacha20poly1305::{aead::{Aead,Payload},KeyInit,XChaCha20Poly1305,XNonce};use rand::RngCore;
    let mut nonce=[0u8;24];rand::rng().fill_bytes(&mut nonce);
    let cipher=XChaCha20Poly1305::new_from_slice(&aead_key(secret)).expect("fixed key size");
    let encrypted=cipher.encrypt(XNonce::from_slice(&nonce),Payload{msg:plain,aad:SEAL_V2}).expect("bounded credential plaintext");
    [SEAL_V2,&nonce,&encrypted].concat()
}
pub fn unseal(secret:&[u8],sealed:&[u8])->Option<Vec<u8>>{
    use chacha20poly1305::{aead::{Aead,Payload},KeyInit,XChaCha20Poly1305,XNonce};
    if sealed.starts_with(SEAL_MAGIC){return unseal_legacy(secret,sealed);}
    if !sealed.starts_with(SEAL_V2)||sealed.len()<45||sealed.len()>CREDENTIAL_LIMIT as usize{return None;}
    XChaCha20Poly1305::new_from_slice(&aead_key(secret)).ok()?.decrypt(XNonce::from_slice(&sealed[5..29]),Payload{msg:&sealed[29..],aad:SEAL_V2}).ok()
}
fn private_read(path:&Path,limit:u64)->io::Result<Vec<u8>>{
    let mut options=fs::OpenOptions::new();options.read(true);
    #[cfg(unix)]{use std::os::unix::fs::OpenOptionsExt;options.custom_flags(libc::O_NOFOLLOW);}
    let mut file=options.open(path)?;let meta=file.metadata()?;
    if !meta.is_file()||meta.len()>limit{return Err(io::Error::other("invalid credential file"));}
    #[cfg(unix)]{use std::os::unix::fs::PermissionsExt;file.set_permissions(fs::Permissions::from_mode(0o600))?;}
    let mut bytes=Vec::new();Read::by_ref(&mut file).take(limit+1).read_to_end(&mut bytes)?;
    if bytes.len() as u64>limit{return Err(io::Error::other("credential file too large"));}Ok(bytes)
}
fn private_directory(path:&Path)->io::Result<()>{
    fs::create_dir_all(path)?;
    if !fs::symlink_metadata(path)?.is_dir(){return Err(io::Error::other("credential directory is not a directory"));}
    #[cfg(unix)]{use std::os::unix::fs::PermissionsExt;fs::set_permissions(path,fs::Permissions::from_mode(0o700))?;}
    Ok(())
}
fn private_write(path:&Path,bytes:&[u8])->io::Result<()>{
    use rand::RngCore;let parent=path.parent().ok_or_else(||io::Error::other("missing credential directory"))?;private_directory(parent)?;
    if fs::symlink_metadata(path).is_ok_and(|m|!m.is_file()){return Err(io::Error::other("unsafe credential target"));}
    let mut nonce=[0u8;8];rand::rng().fill_bytes(&mut nonce);let tmp=parent.join(format!(".credentials-{:016x}.tmp",u64::from_ne_bytes(nonce)));
    let result=(||{let mut options=fs::OpenOptions::new();options.write(true).create_new(true);
        #[cfg(unix)]{use std::os::unix::fs::OpenOptionsExt;options.mode(0o600).custom_flags(libc::O_NOFOLLOW);}
        let mut f=options.open(&tmp)?;f.write_all(bytes)?;f.sync_all()?;drop(f);fs::rename(&tmp,path)?;
        // Some PS5 filesystems do not support directory fsync. The file was synced before rename.
        if let Ok(dir)=File::open(parent){let _=dir.sync_all();}Ok(())})();
    if result.is_err(){let _=fs::remove_file(tmp);}result
}
fn store_key(location:&Path,create:bool)->io::Result<Vec<u8>>{
    use rand::RngCore;use hmac::{Hmac,Mac};
    let device=SEAL_SECRET.get().filter(|s|s.len()>=16).ok_or_else(||io::Error::other("device binding unavailable"))?;
    let parent=location.parent().ok_or_else(||io::Error::other("missing credential directory"))?;private_directory(parent)?;
    let path=parent.join(".credential-key");
    let random=match private_read(&path,32){Ok(key) if key.len()==32=>key,Ok(_)=>return Err(io::Error::other("invalid installation key")),
        Err(e) if create&&e.kind()==io::ErrorKind::NotFound=>{
            let mut key=vec![0u8;32];rand::rng().fill_bytes(&mut key);
            // Exclusive creation prevents one writer replacing another's key.
            let mut options=fs::OpenOptions::new();options.write(true).create_new(true);
            #[cfg(unix)]{use std::os::unix::fs::OpenOptionsExt;options.mode(0o600).custom_flags(libc::O_NOFOLLOW);}
            match options.open(&path){Ok(mut f)=>{f.write_all(&key)?;f.sync_all()?;key},Err(e) if e.kind()==io::ErrorKind::AlreadyExists=>private_read(&path,32)?,Err(e)=>return Err(e)}
        },Err(e)=>return Err(e)};
    if random.len()!=32{return Err(io::Error::other("invalid installation key"));}
    let mut h=<Hmac<sha2::Sha256> as Mac>::new_from_slice(device).expect("any key size");h.update(b"librespot-ps5 installation key v2\0");h.update(&random);Ok(h.finalize().into_bytes().to_vec())
}
#[derive(Debug, Error)]
pub enum CacheError {
    #[error("audio cache location is not configured")]
    Path,
}

impl From<CacheError> for Error {
    fn from(err: CacheError) -> Self {
        Error::failed_precondition(err)
    }
}

/// Some kind of data structure that holds some paths, the size of these files and a timestamp.
/// It keeps track of the file sizes and is able to pop the path with the oldest timestamp if
/// a given limit is exceeded.
struct SizeLimiter {
    queue: PriorityQueue<PathBuf, Reverse<SystemTime>>,
    sizes: HashMap<PathBuf, u64>,
    size_limit: u64,
    in_use: u64,
}

impl SizeLimiter {
    /// Creates a new instance with the given size limit.
    fn new(limit: u64) -> Self {
        Self {
            queue: PriorityQueue::new(),
            sizes: HashMap::new(),
            size_limit: limit,
            in_use: 0,
        }
    }

    /// Adds an entry to this data structure.
    ///
    /// If this file is already contained, it will be updated accordingly.
    fn add(&mut self, file: &Path, size: u64, accessed: SystemTime) {
        self.in_use += size;
        self.queue.push(file.to_owned(), Reverse(accessed));
        if let Some(old_size) = self.sizes.insert(file.to_owned(), size) {
            // It's important that decreasing happens after
            // increasing the size, to prevent an overflow.
            self.in_use -= old_size;
        }
    }

    /// Returns true if the limit is exceeded.
    fn exceeds_limit(&self) -> bool {
        self.in_use > self.size_limit
    }

    /// Returns the least recently accessed file if the size of the cache exceeds
    /// the limit.
    ///
    /// The entry is removed from the data structure, but the caller is responsible
    /// to delete the file in the file system.
    fn pop(&mut self) -> Option<PathBuf> {
        if self.exceeds_limit() {
            if let Some((next, _)) = self.queue.pop() {
                if let Some(size) = self.sizes.remove(&next) {
                    self.in_use -= size;
                } else {
                    error!("`queue` and `sizes` should have the same keys.");
                }
                Some(next)
            } else {
                error!("in_use was > 0, so the queue should have contained an item.");
                None
            }
        } else {
            None
        }
    }

    /// Updates the timestamp of an existing element. Returns `true` if the item did exist.
    fn update(&mut self, file: &Path, access_time: SystemTime) -> bool {
        self.queue
            .change_priority(file, Reverse(access_time))
            .is_some()
    }

    /// Removes an element with the specified path. Returns `true` if the item did exist.
    fn remove(&mut self, file: &Path) -> bool {
        if self.queue.remove(file).is_none() {
            return false;
        }

        if let Some(size) = self.sizes.remove(file) {
            self.in_use -= size;
        } else {
            error!("`queue` and `sizes` should have the same keys.");
        }

        true
    }
}

struct FsSizeLimiter {
    limiter: Mutex<SizeLimiter>,
}

impl FsSizeLimiter {
    /// Returns access time and file size of a given path.
    fn get_metadata(file: &Path) -> io::Result<(SystemTime, u64)> {
        let metadata = file.metadata()?;

        // The first of the following timestamps which is available will be chosen as access time:
        // 1. Access time
        // 2. Modification time
        // 3. Creation time
        // 4. Current time
        let access_time = metadata
            .accessed()
            .or_else(|_| metadata.modified())
            .or_else(|_| metadata.created())
            .unwrap_or_else(|_| SystemTime::now());

        let size = metadata.len();

        Ok((access_time, size))
    }

    /// Recursively search a directory for files and add them to the `limiter` struct.
    fn init_dir(limiter: &mut SizeLimiter, path: &Path) {
        let list_dir = match fs::read_dir(path) {
            Ok(list_dir) => list_dir,
            Err(e) => {
                warn!("Could not read directory {path:?} in cache dir: {e}");
                return;
            }
        };

        for entry in list_dir {
            let entry = match entry {
                Ok(entry) => entry,
                Err(e) => {
                    warn!("Could not directory {path:?} in cache dir: {e}");
                    return;
                }
            };

            match entry.file_type() {
                Ok(file_type) if file_type.is_dir() || file_type.is_symlink() => {
                    Self::init_dir(limiter, &entry.path())
                }
                Ok(file_type) if file_type.is_file() => {
                    let path = entry.path();
                    match Self::get_metadata(&path) {
                        Ok((access_time, size)) => {
                            limiter.add(&path, size, access_time);
                        }
                        Err(e) => {
                            warn!("Could not read file {path:?} in cache dir: {e}")
                        }
                    }
                }
                Ok(ft) => {
                    warn!(
                        "File {:?} in cache dir has unsupported type {:?}",
                        entry.path(),
                        ft
                    )
                }
                Err(e) => {
                    warn!(
                        "Could not get type of file {:?} in cache dir: {}",
                        entry.path(),
                        e
                    )
                }
            };
        }
    }

    fn add(&self, file: &Path, size: u64) {
        self.limiter
            .lock()
            .expect(CACHE_LIMITER_POISON_MSG)
            .add(file, size, SystemTime::now())
    }

    fn touch(&self, file: &Path) -> bool {
        self.limiter
            .lock()
            .expect(CACHE_LIMITER_POISON_MSG)
            .update(file, SystemTime::now())
    }

    fn remove(&self, file: &Path) -> bool {
        self.limiter
            .lock()
            .expect(CACHE_LIMITER_POISON_MSG)
            .remove(file)
    }

    fn prune_internal<F: FnMut() -> Option<PathBuf>>(mut pop: F) -> Result<(), Error> {
        let mut first = true;
        let mut count = 0;
        let mut last_error = None;

        while let Some(file) = pop() {
            if first {
                debug!("Cache dir exceeds limit, removing least recently used files.");
                first = false;
            }

            let res = fs::remove_file(&file);
            if let Err(e) = res {
                warn!("Could not remove file {file:?} from cache dir: {e}");
                last_error = Some(e);
            } else {
                count += 1;
            }
        }

        if count > 0 {
            info!("Removed {count} cache files.");
        }

        if let Some(err) = last_error {
            Err(err.into())
        } else {
            Ok(())
        }
    }

    fn prune(&self) -> Result<(), Error> {
        Self::prune_internal(|| self.limiter.lock().expect(CACHE_LIMITER_POISON_MSG).pop())
    }

    fn new(path: &Path, limit: u64) -> Result<Self, Error> {
        let mut limiter = SizeLimiter::new(limit);

        Self::init_dir(&mut limiter, path);
        Self::prune_internal(|| limiter.pop())?;

        Ok(Self {
            limiter: Mutex::new(limiter),
        })
    }
}

/// A cache for volume, credentials and audio files.
#[derive(Clone)]
pub struct Cache {
    credentials_location: Option<PathBuf>,
    volume_location: Option<PathBuf>,
    audio_location: Option<PathBuf>,
    size_limiter: Option<Arc<FsSizeLimiter>>,
}

impl Cache {
    pub fn new<P: AsRef<Path>>(
        credentials_path: Option<P>,
        volume_path: Option<P>,
        audio_path: Option<P>,
        size_limit: Option<u64>,
    ) -> Result<Self, Error> {
        let mut size_limiter = None;

        if let Some(location) = &credentials_path {
            fs::create_dir_all(location)?;
        }

        let credentials_location = credentials_path
            .as_ref()
            .map(|p| p.as_ref().join("credentials.json"));

        if let Some(location) = &volume_path {
            fs::create_dir_all(location)?;
        }

        let volume_location = volume_path.as_ref().map(|p| p.as_ref().join("volume"));

        if let Some(location) = &audio_path {
            fs::create_dir_all(location)?;

            if let Some(limit) = size_limit {
                let limiter = FsSizeLimiter::new(location.as_ref(), limit)?;
                size_limiter = Some(Arc::new(limiter));
            }
        }

        let audio_location = audio_path.map(|p| p.as_ref().to_owned());

        let cache = Cache {
            credentials_location,
            volume_location,
            audio_location,
            size_limiter,
        };

        Ok(cache)
    }

    pub fn credentials(&self) -> Option<Credentials> {
        let location=self.credentials_location.as_ref()?;
        let result=(||->Result<Credentials,Error>{
            let contents=private_read(location,CREDENTIAL_LIMIT)?;
            let plain=if contents.starts_with(SEAL_V2){unseal(&store_key(location,false)?,&contents)}
                else if contents.starts_with(SEAL_MAGIC){SEAL_SECRET.get().and_then(|secret|unseal_legacy(secret,&contents))}
                else {None}.ok_or_else(||Error::unauthenticated("saved login could not be unlocked"))?;
            let c:Credentials=serde_json::from_slice(&plain)?;
            if c.auth_data.is_empty()||c.username.as_ref().is_none_or(|s|s.is_empty()) {return Err(Error::unauthenticated("saved login is incomplete"));}Ok(c)
        })();
        match result{Ok(c)=>{storage_result(true);Some(c)},Err(e)=>{if e.kind!=ErrorKind::NotFound{storage_result(false);warn!("Saved login unavailable; pair again if needed (contents withheld)");}None}}
    }
    pub fn save_credentials(&self,cred:&Credentials){
        if let Some(location)=&self.credentials_location{
            let result=(||->io::Result<()>{
                if cred.auth_data.is_empty()||cred.username.as_ref().is_none_or(|s|s.is_empty()){return Err(io::Error::other("incomplete reusable login"));}
                let key=store_key(location,true)?;let data=serde_json::to_vec(cred)?;
                if data.len()>CREDENTIAL_LIMIT as usize-64{return Err(io::Error::other("credential record too large"));}
                private_write(location,&seal(&key,&data))
            })();
            storage_result(result.is_ok());if result.is_err(){warn!("Could not remember login; current session remains available (contents withheld)");}
        }
    }

    pub fn volume(&self) -> Option<u16> {
        let location = self.volume_location.as_ref()?;

        let read = || -> Result<u16, Error> {
            let mut file = File::open(location)?;
            let mut contents = String::new();
            file.read_to_string(&mut contents)?;
            Ok(contents.parse()?)
        };

        match read() {
            Ok(v) => Some(v),
            Err(e) => {
                if e.kind != ErrorKind::NotFound {
                    warn!("Error reading volume from cache: {e}");
                }
                None
            }
        }
    }

    pub fn save_volume(&self, volume: u16) {
        if let Some(ref location) = self.volume_location {
            let result = File::create(location).and_then(|mut file| write!(file, "{volume}"));
            if let Err(e) = result {
                warn!("Cannot save volume to cache: {e}");
            }
        }
    }

    pub fn file_path(&self, file: FileId) -> Option<PathBuf> {
        match file.to_base16() {
            Ok(name) => self.audio_location.as_ref().map(|location| {
                let mut path = location.join(&name[0..2]);
                path.push(&name[2..]);
                path
            }),
            Err(e) => {
                warn!("Invalid FileId: {e}");
                None
            }
        }
    }

    pub fn file(&self, file: FileId) -> Option<File> {
        let path = self.file_path(file)?;
        match File::open(&path) {
            Ok(file) => {
                if let Some(limiter) = self.size_limiter.as_deref() {
                    if !limiter.touch(&path) {
                        error!("limiter could not touch {path:?}");
                    }
                }
                Some(file)
            }
            Err(e) => {
                if e.kind() != io::ErrorKind::NotFound {
                    warn!("Error reading file from cache: {e}")
                }
                None
            }
        }
    }

    pub fn save_file<F: Read>(&self, file: FileId, contents: &mut F) -> Result<PathBuf, Error> {
        if let Some(path) = self.file_path(file) {
            if let Some(parent) = path.parent() {
                if let Ok(size) = fs::create_dir_all(parent)
                    .and_then(|_| File::create(&path))
                    .and_then(|mut file| io::copy(contents, &mut file))
                {
                    if let Some(limiter) = self.size_limiter.as_deref() {
                        limiter.add(&path, size);
                        limiter.prune()?;
                    }
                    return Ok(path);
                }
            }
        }
        Err(CacheError::Path.into())
    }

    pub fn remove_file(&self, file: FileId) -> Result<(), Error> {
        let path = self.file_path(file).ok_or(CacheError::Path)?;

        fs::remove_file(&path)?;
        if let Some(limiter) = self.size_limiter.as_deref() {
            limiter.remove(&path);
        }

        Ok(())
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use std::time::Duration;

    #[test]
    fn legacy_seals_migrate_without_plaintext_or_public_fallback(){
        let old=seal_legacy(b"a device binding",b"synthetic credential");
        assert_eq!(unseal(b"a device binding",&old).unwrap(),b"synthetic credential");
        assert!(unseal(b"wrong device",&old).is_none());
        let modern=seal(b"a device binding",b"synthetic credential");assert!(modern.starts_with(SEAL_V2));
        assert!(unseal(b"wrong device",&modern).is_none());
    }
    #[cfg(unix)]
    #[test]
    fn credential_writes_are_private_and_reject_symlinks(){
        use std::os::unix::fs::{PermissionsExt,symlink};
        let root=std::env::temp_dir().join(format!("credential-safety-{}",std::process::id()));let _=fs::remove_dir_all(&root);fs::create_dir(&root).unwrap();
        let path=root.join("credentials");private_write(&path,b"first").unwrap();private_write(&path,b"second").unwrap();
        assert_eq!(private_read(&path,32).unwrap(),b"second");assert_eq!(fs::metadata(&path).unwrap().permissions().mode()&0o777,0o600);
        assert_eq!(fs::metadata(&root).unwrap().permissions().mode()&0o777,0o700);
        let link=root.join("linked");symlink(&path,&link).unwrap();assert!(private_write(&link,b"overwrite").is_err());assert!(private_read(&link,32).is_err());
        assert_eq!(fs::read(&path).unwrap(),b"second");assert!(private_read(&path,3).is_err());fs::remove_dir_all(root).unwrap();
    }
    fn ordered_time(v: u64) -> SystemTime {
        SystemTime::UNIX_EPOCH + Duration::from_secs(v)
    }

    #[test]
    fn test_size_limiter() {
        let mut limiter = SizeLimiter::new(1000);

        limiter.add(Path::new("a"), 500, ordered_time(2));
        limiter.add(Path::new("b"), 500, ordered_time(1));

        // b (500) -> a (500)  => sum: 1000 <= 1000
        assert!(!limiter.exceeds_limit());
        assert_eq!(limiter.pop(), None);

        limiter.add(Path::new("c"), 1000, ordered_time(3));

        // b (500) -> a (500) -> c (1000)  => sum: 2000 > 1000
        assert!(limiter.exceeds_limit());
        assert_eq!(limiter.pop().as_deref(), Some(Path::new("b")));
        // a (500) -> c (1000)  => sum: 1500 > 1000
        assert_eq!(limiter.pop().as_deref(), Some(Path::new("a")));
        // c (1000)   => sum: 1000 <= 1000
        assert_eq!(limiter.pop().as_deref(), None);

        limiter.add(Path::new("d"), 5, ordered_time(2));
        // d (5) -> c (1000) => sum: 1005 > 1000
        assert_eq!(limiter.pop().as_deref(), Some(Path::new("d")));
        // c (1000)   => sum: 1000 <= 1000
        assert_eq!(limiter.pop().as_deref(), None);

        // Test updating

        limiter.add(Path::new("e"), 500, ordered_time(3));
        //  c (1000) -> e (500)  => sum: 1500 > 1000
        assert!(limiter.update(Path::new("c"), ordered_time(4)));
        // e (500) -> c (1000)  => sum: 1500 > 1000
        assert_eq!(limiter.pop().as_deref(), Some(Path::new("e")));
        // c (1000)  => sum: 1000 <= 1000

        // Test removing
        limiter.add(Path::new("f"), 500, ordered_time(2));
        assert!(limiter.remove(Path::new("c")));
        assert!(!limiter.exceeds_limit());
    }
}
