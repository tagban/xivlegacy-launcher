//! Where the disc image and the patches are downloaded from, kept in a small
//! JSON file so the links can change without a new release:
//!
//! ```json
//! { "disc": ["https://…", "https://…"], "patches": ["https://…"] }
//! ```
//!
//! Each list is tried in order (the first is the main copy, the rest are
//! fallbacks). The file is read from, in order: `sources.json` next to the
//! program, then [`REMOTE`] (the copy in the repository), then the links built
//! into the program. Only links come from the file: the expected size and
//! SHA-1 of each download are built in, so an edited file can't make setup
//! accept a different download.

use std::path::Path;
use std::time::Duration;

use anyhow::{Context, Result, bail};
use serde::Deserialize;

use crate::download::Source;
use crate::setup::{ISO_7Z, PATCHES_ZIP};

/// The repository's copy; edit it there to change the links for everyone.
pub const REMOTE: &str = "https://raw.githubusercontent.com/tagban/xivlegacy-launcher/main/sources.json";
pub const FILE_NAME: &str = "sources.json";

#[derive(Debug, Clone, PartialEq, Deserialize)]
pub struct Sources {
    pub disc: Vec<String>,
    pub patches: Vec<String>,
    /// Where these links came from (for messages).
    #[serde(skip)]
    pub origin: String,
}

fn builtin_urls(src: &Source) -> Vec<String> {
    std::iter::once(src.url).chain(src.mirrors.iter().copied()).map(String::from).collect()
}

impl Sources {
    pub fn builtin() -> Sources {
        Sources { disc: builtin_urls(&ISO_7Z), patches: builtin_urls(&PATCHES_ZIP), origin: "built in".into() }
    }

    pub fn parse(json: &str, origin: &str) -> Result<Sources> {
        let mut s: Sources = serde_json::from_str(json).context("not a sources file")?;
        let ok = |u: &String| u.starts_with("https://") || u.starts_with("http://");
        s.disc.retain(ok);
        s.patches.retain(ok);
        if s.disc.is_empty() || s.patches.is_empty() {
            bail!("needs at least one http(s) link for both \"disc\" and \"patches\"");
        }
        s.origin = origin.to_string();
        Ok(s)
    }

    /// `sources.json` beside the program, else the repository's copy, else the built-in links.
    pub fn load() -> Sources {
        let beside = std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.join(FILE_NAME)));
        if let Some(p) = beside.filter(|p| p.is_file()) {
            if let Ok(s) = Sources::from_file(&p) {
                return s;
            }
        }
        Sources::fetch(REMOTE).unwrap_or_else(|_| Sources::builtin())
    }

    pub fn from_file(path: &Path) -> Result<Sources> {
        let text = std::fs::read_to_string(path)?;
        Sources::parse(&text, &path.display().to_string())
    }

    pub fn fetch(url: &str) -> Result<Sources> {
        let agent = ureq::AgentBuilder::new().timeout(Duration::from_secs(10)).build();
        let text = agent.get(url).call()?.into_string()?;
        Sources::parse(&text, url)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_repository_file_matches_the_built_in_links() {
        let repo = include_str!("../../../sources.json");
        let s = Sources::parse(repo, "repo").unwrap();
        assert_eq!((s.disc, s.patches), (Sources::builtin().disc, Sources::builtin().patches));
    }

    #[test]
    fn rejects_files_without_links() {
        assert!(Sources::parse(r#"{"disc": [], "patches": ["https://x"]}"#, "t").is_err());
        assert!(Sources::parse(r#"{"disc": ["file:///etc"], "patches": ["https://x"]}"#, "t").is_err());
        assert!(Sources::parse("not json", "t").is_err());
    }
}
