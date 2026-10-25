//! The encrypted argument ffxivboot hands ffxivgame.exe (the Rust form of
//! runtime/portable/sqex_args.c):
//!
//!   sqex0002<base64url(Blowfish(" T =<tick> /LANG =en-us /REGION =2 /SERVER_UTC =1356916742
//!            /SESSION_ID =<56 chars>" + NUL))>!////
//!
//! Blowfish on little-endian 32-bit halves, keyed with the eight lowercase hex
//! digits of (tick & !0xFFFF); only whole 8-byte blocks are encrypted. The game
//! derives the key from its own GetTickCount, so `tick` must come from it too.

use anyhow::{Result, bail};
use blowfish::BlowfishLE;
use blowfish::cipher::{BlockEncrypt, KeyInit, generic_array::GenericArray};

pub fn launch_arg(session_id: &str, tick: u32) -> Result<String> {
    if session_id.len() != 56 || !session_id.bytes().all(|b| b.is_ascii_alphanumeric()) {
        bail!("the server sent a session id the game can't use ({} characters)", session_id.len());
    }
    let mut buf =
        format!(" T ={tick} /LANG =en-us /REGION =2 /SERVER_UTC =1356916742 /SESSION_ID ={session_id}").into_bytes();
    buf.push(0); // the NUL is encrypted too

    let key = format!("{:08x}", tick & !0xFFFF);
    let bf = BlowfishLE::new_from_slice(key.as_bytes()).expect("8-byte key");
    for block in buf.chunks_exact_mut(8) {
        bf.encrypt_block(GenericArray::from_mut_slice(block));
    }

    const B64: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut out = String::from("sqex0002");
    for c in buf.chunks(3) {
        let v = (c[0] as u32) << 16 | (*c.get(1).unwrap_or(&0) as u32) << 8 | *c.get(2).unwrap_or(&0) as u32;
        out.push(B64[(v >> 18) as usize] as char);
        out.push(B64[(v >> 12 & 63) as usize] as char);
        out.push(if c.len() > 1 { B64[(v >> 6 & 63) as usize] as char } else { '=' });
        out.push(if c.len() > 2 { B64[(v & 63) as usize] as char } else { '=' });
    }
    out.push_str("!////");
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Produced by runtime/portable/sqex_args.c for the same inputs.
    #[test]
    fn matches_the_c_version() {
        let sid = "0123456789abcdef0123456789abcdef0123456789abcdef01234567";
        assert_eq!(launch_arg(sid, 0x1234_5678).unwrap(), include_str!("../tests/sqex_vector.txt").trim());
    }

    #[test]
    fn rejects_bad_session_ids() {
        assert!(launch_arg("short", 1).is_err());
    }
}
