#!/bin/bash
# Makes the toolchain's standard-library source compile for the console's FreeBSD 11
# layouts. With the libc crate set to FreeBSD 11, a directory entry's file number is 32
# bits wide, and one line of std assigns it to a 64-bit field without converting.
# Safe to run repeatedly; fails if the line it expects is no longer there.
set -euo pipefail
export RUSTUP_HOME=${RUSTUP_HOME:-$HOME/.rustup} CARGO_HOME=${CARGO_HOME:-$HOME/.cargo}
export PATH=$CARGO_HOME/bin:$PATH
file=$(rustc --print sysroot)/lib/rustlib/src/rust/library/std/src/sys/fs/unix.rs
if grep -q 'd_ino: (\*entry_ptr).d_fileno as u64,' "$file"; then
    exit 0
fi
grep -q 'd_ino: (\*entry_ptr).d_fileno,' "$file" || {
    echo "patch-rust-src: std's directory-entry code has changed; update this patch" >&2
    exit 1
}
sed -i 's/d_ino: (\*entry_ptr).d_fileno,/d_ino: (*entry_ptr).d_fileno as u64,/' "$file"
echo "patch-rust-src: patched $file"
