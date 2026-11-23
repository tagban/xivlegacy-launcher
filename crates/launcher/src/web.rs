//! A Garlemald server's web front: sign-up, sign-in and the health check, over
//! HTTPS (port 443) when the server offers it, else plain HTTP (port 54993).
//! Sign-in answers with a redirect to `ffxiv://login_success?sessionId=…`;
//! failures redirect back with `err=…`. Functions take a base URL from [`base`].

use std::time::Duration;

use anyhow::{Result, bail};

pub const WEB_PORT: u16 = 54993;

/// The base URL of `host`'s web front: `https://host` or `http://host:54993`.
pub fn base(host: &str, tls: bool) -> String {
    if tls { format!("https://{host}") } else { format!("http://{host}:{WEB_PORT}") }
}

/// How `host` answers: `Some(true)` over HTTPS with a valid certificate,
/// `Some(false)` only over plain HTTP, `None` not at all.
pub fn probe(host: &str) -> Option<bool> {
    if alive(&base(host, true)) {
        Some(true)
    } else if alive(&base(host, false)) {
        Some(false)
    } else {
        None
    }
}

fn agent(timeout: Duration) -> ureq::Agent {
    ureq::AgentBuilder::new().redirects(0).timeout_connect(timeout).timeout(timeout).build()
}

/// Whether the server's web front answers.
pub fn alive(base: &str) -> bool {
    agent(Duration::from_secs(3))
        .get(&format!("{base}/healthz"))
        .call()
        .is_ok_and(|r| r.status() == 200)
}

/// POSTs a form and returns the redirect's Location.
fn post(base: &str, path: &str, fields: &[(&str, &str)]) -> Result<String> {
    let url = format!("{base}{path}");
    match agent(Duration::from_secs(10)).post(&url).send_form(fields) {
        Ok(r) => Ok(r.header("Location").unwrap_or_default().to_string()),
        Err(ureq::Error::Status(code, r)) => match r.header("Location") {
            Some(l) => Ok(l.to_string()),
            None => bail!("the server answered {code}"),
        },
        Err(_) => bail!("the server at {base} didn't answer"),
    }
}

fn query(location: &str, key: &str) -> Option<String> {
    let q = location.split_once('?')?.1;
    q.split('&').find_map(|kv| {
        let (k, v) = kv.split_once('=')?;
        (k == key).then(|| decode(v))
    })
}

fn decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        match b[i] {
            b'+' => out.push(b' '),
            b'%' if i + 2 < b.len() => {
                match std::str::from_utf8(&b[i + 1..i + 3]).ok().and_then(|h| u8::from_str_radix(h, 16).ok()) {
                    Some(v) => {
                        out.push(v);
                        i += 2;
                    }
                    None => out.push(b'%'),
                }
            }
            c => out.push(c),
        }
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Signs in; the session id the game is started with.
pub fn login(base: &str, user: &str, password: &str) -> Result<String> {
    let loc = post(base, "/login", &[("username", user), ("password", password)])?;
    if loc.starts_with("ffxiv://") {
        if let Some(sid) = query(&loc, "sessionId") {
            return Ok(sid);
        }
    }
    match query(&loc, "err").filter(|e| !e.is_empty()) {
        Some(e) => bail!("{e}"),
        None => bail!("Sign-in failed. Check your username and password."),
    }
}

/// Creates an account.
pub fn signup(base: &str, user: &str, password: &str) -> Result<()> {
    let loc = post(base, "/signup", &[("username", user), ("password", password), ("confirm", password)])?;
    match query(&loc, "err").filter(|e| !e.is_empty()) {
        Some(e) => bail!("{e}"),
        None => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Against a running server: XIV_TEST_HOST, XIV_TEST_USER, XIV_TEST_PASS.
    #[test]
    #[ignore]
    fn signs_in() {
        let v = |k| std::env::var(k).unwrap();
        let b = base(&v("XIV_TEST_HOST"), probe(&v("XIV_TEST_HOST")).unwrap());
        assert!(alive(&b));
        let sid = login(&b, &v("XIV_TEST_USER"), &v("XIV_TEST_PASS")).unwrap();
        assert_eq!(sid.len(), 56);
        crate::sqex::launch_arg(&sid, 1).unwrap();
        assert!(login(&b, &v("XIV_TEST_USER"), "wrong").is_err());
    }

    #[test]
    fn builds_bases() {
        assert_eq!(base("xiv.example.com", true), "https://xiv.example.com");
        assert_eq!(base("10.0.0.5", false), "http://10.0.0.5:54993");
    }

    #[test]
    fn reads_redirects() {
        assert_eq!(query("ffxiv://login_success?sessionId=abc", "sessionId").as_deref(), Some("abc"));
        assert_eq!(query("/login?err=Wrong+password%21", "err").as_deref(), Some("Wrong password!"));
        assert_eq!(query("/login", "err"), None);
    }
}
