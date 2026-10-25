//! Reaching a server hosted on the player's own network.
//!
//! A server at home is published under its public name, but many home routers
//! can't send their own network back to their public address (no "hairpin
//! NAT"), so from inside the house that name doesn't answer. When it doesn't,
//! and this computer is behind the same public address as the server, the
//! server must be on this network: look for it there (the web port on each
//! address of this computer's /24) and use its local address instead. The
//! server hands such players its local world address too (Garlemald's
//! `lan_address`).

use std::net::{IpAddr, Ipv4Addr, SocketAddr, TcpStream, ToSocketAddrs, UdpSocket};
use std::sync::Mutex;
use std::thread;
use std::time::Duration;

use crate::web::{self, WEB_PORT};

fn is_local(ip: Ipv4Addr) -> bool {
    ip.is_private() || ip.is_loopback() || ip.is_link_local()
}

fn resolve_v4(host: &str) -> Option<Ipv4Addr> {
    (host, WEB_PORT).to_socket_addrs().ok()?.find_map(|a| match a.ip() {
        IpAddr::V4(v4) => Some(v4),
        IpAddr::V6(_) => None,
    })
}

/// This computer's public IPv4 address, as the internet sees it.
fn public_ip() -> Option<Ipv4Addr> {
    let agent = ureq::AgentBuilder::new().timeout(Duration::from_secs(5)).build();
    let text = agent.get("https://1.1.1.1/cdn-cgi/trace").call().ok()?.into_string().ok()?;
    text.lines().find_map(|l| l.strip_prefix("ip=")).and_then(|ip| ip.trim().parse().ok())
}

/// The address of the network interface that would carry traffic to `to`.
fn interface_ip(to: Ipv4Addr) -> Option<Ipv4Addr> {
    let s = UdpSocket::bind("0.0.0.0:0").ok()?;
    s.connect((to, 9)).ok()?;
    match s.local_addr().ok()?.ip() {
        IpAddr::V4(v4) => Some(v4),
        IpAddr::V6(_) => None,
    }
}

/// The server's public address, if this computer is behind it (the same
/// router), so the server is somewhere on this network.
pub fn behind_same_router(host: &str) -> Option<Ipv4Addr> {
    let server = resolve_v4(host)?;
    (!is_local(server) && public_ip()? == server).then_some(server)
}

/// The local address of the server published as `host`: `known` if it still
/// answers, else a search of this network. Only when this computer is behind
/// the server's own router, so it never picks another network's server.
pub fn find(host: &str, known: Option<&str>) -> Option<String> {
    let server = behind_same_router(host)?;
    if let Some(k) = known.filter(|k| web::alive(k)) {
        return Some(k.to_string());
    }
    let me = interface_ip(server).filter(|ip| ip.is_private())?;
    let [a, b, c, _] = me.octets();
    let found = Mutex::new(None);
    let candidates: Vec<Ipv4Addr> = (1..=254u8).map(|d| Ipv4Addr::new(a, b, c, d)).collect();
    thread::scope(|s| {
        for chunk in candidates.chunks(8) {
            let found = &found;
            s.spawn(move || {
                for &ip in chunk {
                    if found.lock().unwrap().is_some() {
                        return;
                    }
                    let addr = SocketAddr::from((ip, WEB_PORT));
                    if TcpStream::connect_timeout(&addr, Duration::from_millis(400)).is_ok()
                        && web::alive(&ip.to_string())
                    {
                        *found.lock().unwrap() = Some(ip.to_string());
                    }
                }
            });
        }
    });
    found.into_inner().unwrap()
}

#[cfg(test)]
mod tests {
    /// On the server's own network: XIV_TEST_PUBLIC_HOST is its public name.
    #[test]
    #[ignore]
    fn finds_the_server_on_this_network() {
        let host = std::env::var("XIV_TEST_PUBLIC_HOST").unwrap();
        let found = super::find(&host, None);
        println!("found: {found:?}");
        assert!(found.is_some());
    }
}
