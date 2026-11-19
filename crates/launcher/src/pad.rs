//! Gamepads for the launcher's menus (a Steam Deck, an Xbox or PlayStation pad): the d-pad or
//! left stick moves, A (Cross) chooses, B (Circle) goes back, Start (Menu, Options) opens Settings.
//! A held direction repeats, so a volume can be swept without tapping.

use std::time::{Duration, Instant};

use gilrs::{Axis, Button, EventType, Gilrs};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Nav {
    Up,
    Down,
    Left,
    Right,
    Accept,
    Back,
    Menu,
}

const STICK: f32 = 0.6;
const REPEAT_AFTER: Duration = Duration::from_millis(400);
const REPEAT_EVERY: Duration = Duration::from_millis(110);

pub struct Pads {
    gilrs: Option<Gilrs>,
    /// The direction held (d-pad or stick), since when, and when it last repeated.
    held: Option<(Nav, Instant, Instant)>,
    stick: (f32, f32),
}

impl Pads {
    pub fn new() -> Pads {
        let gilrs = match Gilrs::new() {
            Ok(g) => Some(g),
            Err(e) => {
                crate::config::log(&format!("no gamepads: {e}"));
                None
            }
        };
        Pads { gilrs, held: None, stick: (0.0, 0.0) }
    }

    /// Whether a pad is connected now.
    pub fn connected(&self) -> bool {
        self.gilrs.as_ref().is_some_and(|g| g.gamepads().next().is_some())
    }

    /// Whether a direction is held (the caller keeps polling for the repeat).
    pub fn holding(&self) -> bool {
        self.held.is_some()
    }

    /// What the pads asked for since the last call.
    pub fn poll(&mut self) -> Vec<Nav> {
        let mut out = Vec::new();
        let Some(g) = self.gilrs.as_mut() else { return out };
        let now = Instant::now();
        let mut stick_moved = false;
        while let Some(ev) = g.next_event() {
            match ev.event {
                EventType::ButtonPressed(b, _) => {
                    let n = match b {
                        Button::DPadUp => Some(Nav::Up),
                        Button::DPadDown => Some(Nav::Down),
                        Button::DPadLeft => Some(Nav::Left),
                        Button::DPadRight => Some(Nav::Right),
                        Button::South => Some(Nav::Accept),
                        Button::East => Some(Nav::Back),
                        Button::Start => Some(Nav::Menu),
                        _ => None,
                    };
                    if let Some(n) = n {
                        out.push(n);
                        if dir(n) {
                            self.held = Some((n, now, now));
                        }
                    }
                }
                EventType::ButtonReleased(b, _) => {
                    let n = match b {
                        Button::DPadUp => Nav::Up,
                        Button::DPadDown => Nav::Down,
                        Button::DPadLeft => Nav::Left,
                        Button::DPadRight => Nav::Right,
                        _ => continue,
                    };
                    if self.held.is_some_and(|h| h.0 == n) {
                        self.held = None;
                    }
                }
                EventType::AxisChanged(Axis::LeftStickX, v, _) => {
                    self.stick.0 = v;
                    stick_moved = true;
                }
                EventType::AxisChanged(Axis::LeftStickY, v, _) => {
                    self.stick.1 = v;
                    stick_moved = true;
                }
                EventType::Disconnected => self.held = None,
                _ => {}
            }
        }
        if stick_moved {
            let (x, y) = self.stick;
            let n = if y > STICK {
                Some(Nav::Up)
            } else if y < -STICK {
                Some(Nav::Down)
            } else if x < -STICK {
                Some(Nav::Left)
            } else if x > STICK {
                Some(Nav::Right)
            } else {
                None
            };
            match (n, self.held) {
                (Some(n), Some(h)) if h.0 == n => {}
                (Some(n), _) => {
                    out.push(n);
                    self.held = Some((n, now, now));
                }
                (None, _) => self.held = None,
            }
        }
        if let Some((n, since, last)) = self.held
            && now.duration_since(since) >= REPEAT_AFTER
            && now.duration_since(last) >= REPEAT_EVERY
        {
            out.push(n);
            self.held = Some((n, since, now));
        }
        out
    }
}

fn dir(n: Nav) -> bool {
    matches!(n, Nav::Up | Nav::Down | Nav::Left | Nav::Right)
}
