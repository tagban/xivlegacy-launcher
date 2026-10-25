//! Progress reporting shared by every phase of setup.
//!
//! Work is counted in bytes so one bar can move smoothly across downloads,
//! unpacking and patching. The launcher UI and the CLI read the same events.

use std::io::{self, Read};
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Phase {
    Download,
    UnpackIso,
    Disc,
    Patch,
    /// Checking an existing install (its own one-step job).
    Verify,
}

impl Phase {
    pub fn label(self) -> &'static str {
        match self {
            Phase::Download => "Downloading",
            Phase::UnpackIso => "Unpacking the disc image",
            Phase::Disc => "Installing from the disc",
            Phase::Patch => "Patching to 1.23b",
            Phase::Verify => "Checking the install",
        }
    }

    pub fn step(self) -> (u32, u32) {
        if self == Phase::Verify {
            return (1, 1);
        }
        let n = match self {
            Phase::Download => 1,
            Phase::UnpackIso => 2,
            Phase::Disc => 3,
            Phase::Patch => 4,
            Phase::Verify => unreachable!(),
        };
        (n, 4)
    }
}

/// Receives progress. `done`/`total` are bytes within the current phase;
/// `detail` names the file being worked on, if any.
pub trait Report: Send + Sync {
    fn report(&self, phase: Phase, done: u64, total: u64, detail: &str);
}

/// A byte counter several threads (e.g. parallel downloads) add to.
#[derive(Clone, Default)]
pub struct Counter(Arc<AtomicU64>);

impl Counter {
    pub fn add(&self, n: u64) {
        self.0.fetch_add(n, Ordering::Relaxed);
    }
    pub fn get(&self) -> u64 {
        self.0.load(Ordering::Relaxed)
    }
}

/// Counts bytes read through it.
pub struct Counting<R> {
    inner: R,
    counter: Counter,
}

impl<R> Counting<R> {
    pub fn new(inner: R, counter: Counter) -> Self {
        Counting { inner, counter }
    }
}

impl<R: Read> Read for Counting<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        self.counter.add(n as u64);
        Ok(n)
    }
}
