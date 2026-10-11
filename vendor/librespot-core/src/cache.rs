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
// Addition for the PS5 receiver: the saved login is not kept as readable text.
//
// The saved login lets whoever holds it use the account. On the console the file sits where
// anything on the home network can fetch it (the jailbreak's file server asks for no
// password), so it is encrypted with a key made from a secret the caller provides (there,
// an identifier of the console itself) and carries a check value. A copy of the file is
// then of no use on another machine. It does not protect against code running on the same
// console, which can ask for the same identifier.
// ---------------------------------------------------------------------------------------

const SEAL_MAGIC: &[u8] = b"LSPS1";
/// Used when the caller provides no secret: the file is then only obscured.
const SEAL_FALLBACK: &[u8] = b"spotifyps5 saved login, not tied to a device";

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
pub fn seal(secret: &[u8], plain: &[u8]) -> Vec<u8> {
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
pub fn unseal(secret: &[u8], sealed: &[u8]) -> Option<Vec<u8>> {
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
        let location = self.credentials_location.as_ref()?;

        // This closure is just convencience to enable the question mark operator
        // PS5 receiver: the file is sealed (see the top of this file). A file in the old
        // readable form, or one sealed without a device secret (made on a PC), is still
        // accepted, and is written again sealed for this device.
        let secret = SEAL_SECRET.get().map(Vec::as_slice);
        let mut reseal = false;
        let mut read = || -> Result<Credentials, Error> {
            let mut file = File::open(location)?;
            let mut contents = Vec::new();
            file.read_to_end(&mut contents)?;
            if contents.starts_with(SEAL_MAGIC) {
                let plain = secret
                    .and_then(|secret| unseal(secret, &contents))
                    .or_else(|| {
                        reseal = secret.is_some();
                        unseal(SEAL_FALLBACK, &contents)
                    })
                    .ok_or_else(|| Error::unauthenticated("the saved login was sealed on another device"))?;
                return Ok(serde_json::from_slice(&plain)?);
            }
            reseal = true;
            Ok(serde_json::from_slice(&contents)?)
        };

        let result = read();
        match result {
            Ok(c) => {
                if reseal {
                    self.save_credentials(&c);
                }
                Some(c)
            }
            Err(e) => {
                // If the file did not exist, the file was probably not written
                // before. Otherwise, log the error.
                if e.kind != ErrorKind::NotFound {
                    warn!("Error reading credentials from cache: {e}");
                }
                None
            }
        }
    }

    pub fn save_credentials(&self, cred: &Credentials) {
        if let Some(location) = &self.credentials_location {
            // PS5 receiver: never written as readable text (see the top of this file).
            let secret = SEAL_SECRET.get().map_or(SEAL_FALLBACK, Vec::as_slice);
            let result = File::create(location).and_then(|mut file| {
                let data = serde_json::to_string(cred)?;
                file.write_all(&seal(secret, data.as_bytes()))
            });

            if let Err(e) = result {
                warn!("Cannot save credentials to cache: {e}")
            }
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
