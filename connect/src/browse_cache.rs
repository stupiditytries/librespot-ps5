//! Persistent, account-scoped browse data. Contains metadata only, never tokens.
use std::{fs, path::{Path, PathBuf}, sync::atomic::{AtomicU64, Ordering}, time::{SystemTime, UNIX_EPOCH}};
use serde_json::{json, Value};
use sha1::{Digest, Sha1};

const DIRECTORY: &str = "/user/data/spotifyps5/cache/browse-v1";
const MAX_BYTES: u64 = 4 * 1024 * 1024;
static SEQUENCE: AtomicU64 = AtomicU64::new(0);

pub fn now() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default().as_secs()
}

pub struct Entry { pub at: u64, pub value: Value }
impl Entry {
    pub fn fresh(&self, at: u64, seconds: u64) -> bool {
        self.at <= at.saturating_add(60) && at.saturating_sub(self.at) < seconds
    }
}

pub struct DiskCache { directory: PathBuf, account: String }
impl DiskCache {
    pub fn for_account(account: &str) -> Self { Self::new(DIRECTORY, account) }
    fn new(directory: impl AsRef<Path>, account: &str) -> Self {
        Self { directory: directory.as_ref().into(), account: account.into() }
    }
    fn path(&self, key: &str) -> PathBuf {
        let digest = Sha1::digest(format!("{}\n{key}", self.account).as_bytes());
        self.directory.join(format!("{}.json", hex::encode(digest)))
    }
    pub fn load(&self, key: &str) -> Option<Entry> {
        let path = self.path(key);
        let size = fs::metadata(&path).ok()?.len();
        if size == 0 || size > MAX_BYTES { return None; }
        let data: Value = serde_json::from_slice(&fs::read(path).ok()?).ok()?;
        if data["v"] != 1 { return None; }
        Some(Entry { at: data["at"].as_u64()?, value: data.get("value")?.clone() })
    }
    pub fn store(&self, key: &str, value: &Value) { self.store_at(key, value, now()); }
    fn store_at(&self, key: &str, value: &Value, at: u64) {
        let data = json!({"v": 1, "at": at, "value": value}).to_string();
        if data.len() as u64 > MAX_BYTES || fs::create_dir_all(&self.directory).is_err() { return; }
        let path = self.path(key);
        let part = path.with_extension(format!("{}.{}.part", std::process::id(), SEQUENCE.fetch_add(1, Ordering::Relaxed)));
        if fs::write(&part, data).and_then(|()| fs::rename(&part, &path)).is_err() {
            let _ = fs::remove_file(part);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn reopening_the_cache_preserves_data_and_isolates_accounts() {
        let dir = std::env::temp_dir().join(format!("browse-cache-{}-{}", std::process::id(), SEQUENCE.fetch_add(1, Ordering::Relaxed)));
        let cache = DiskCache::new(&dir, "alice");
        cache.store_at("cover:playlist", &json!({"address": "https://i.scdn.co/image/abc"}), 100);
        let reopened = DiskCache::new(&dir, "alice");
        let hit = reopened.load("cover:playlist").unwrap();
        assert_eq!(hit.value["address"], "https://i.scdn.co/image/abc");
        assert!(hit.fresh(110, 300));
        assert!(!hit.fresh(401, 300));
        assert!(!hit.fresh(1, 300)); // a clock reset cannot make it fresh indefinitely
        assert!(DiskCache::new(&dir, "bob").load("cover:playlist").is_none());
        fs::write(cache.path("cover:playlist"), b"partial JSON").unwrap();
        assert!(reopened.load("cover:playlist").is_none());
        fs::remove_dir_all(dir).unwrap();
    }
}
