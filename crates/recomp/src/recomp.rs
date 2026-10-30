//! The recompiler driver: translate the image's functions and write funcs.h (prototypes),
//! funcs_NNN.c (translations) and table.c (address -> function table for indirect calls, and the
//! pinned retail build's constants).
//!
//! `functions` translates those entries plus everything they reach by direct call or tail jump
//! (the closure), which is what a differential test needs; `all` translates every function.
//!
//! `hooks` names instructions where the host may step in (--hooks name=0x...): before each,
//! the translation calls the host function pointer rt_hook_<name> (GuestFn, defined in table.c,
//! NULL until the host sets it) with the guest's registers stored, and reloads them after. A hook
//! named replace_<name>, at a function's entry, replaces the function while set: the translation
//! returns right after it.
use crate::disasm::Disasm;
use crate::pe::Pe;
use crate::program::Program;
use crate::x86c::{FlagEnd, FlagFacts, FunctionTranslator, Translated};
use crate::{write_if_changed, Error, Event};
use indexmap::IndexMap;
use rayon::prelude::*;
use std::collections::{HashMap, HashSet};
use std::path::PathBuf;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::mpsc;

/// The recompiler's command line.
#[derive(Clone, Debug)]
pub struct Options {
    pub meta: PathBuf,
    pub image: PathBuf,
    pub out: PathBuf,
    /// the retail image the runtime maps: table.c's build constants (base, timestamp, size, .text,
    /// entry point) come from it, and the loader checks them
    pub retail: PathBuf,
    pub functions: Vec<u64>,
    pub all: bool,
    pub stats: bool,
    pub chunk: usize,
    /// (name, address)
    pub hooks: Vec<(String, u64)>,
    /// worker threads; 0 = rayon's default (one per core)
    pub threads: usize,
    /// instruction patches applied to the image before translation (--patches): a JSON file of
    /// {"patches": [{"va": "0x...", "bytes": "hex", "expect": "hex", "why": "..."}]}
    pub patches: Option<PathBuf>,
}

impl Options {
    pub fn new(meta: PathBuf, image: PathBuf, retail: PathBuf, out: PathBuf) -> Options {
        Options { meta, image, out, retail, functions: Vec::new(), all: true, stats: false, chunk: 400, hooks: Vec::new(),
                  threads: 0, patches: None }
    }
}

/// What a run translated.
#[derive(Clone, Debug, Default)]
pub struct Report {
    pub functions: usize,
    pub with_unimpl: usize,
    /// unimplemented instruction counts, most common first (all of them; recomp.py prints 40)
    pub unimpl: Vec<(String, usize)>,
    /// entry patches (jmp, int3)
    pub patches: Option<(usize, usize)>,
}

/// Progress lines every this many functions (the launcher's progress bar).
const PROGRESS_STEP: usize = 250;

fn unhex(s: &str, what: &str) -> Result<Vec<u8>, Error> {
    let s: String = s.chars().filter(|c| !c.is_whitespace()).collect();
    if s.len() % 2 != 0 {
        return Err(Error::Meta(format!("patches: {} is not whole bytes", what)));
    }
    (0..s.len()).step_by(2)
        .map(|i| u8::from_str_radix(&s[i..i + 2], 16).map_err(|_| Error::Meta(format!("patches: {} is not hex", what))))
        .collect()
}

/// The patch file: each patch's bytes over the image (checked against "expect", the bytes it
/// replaces, when given), and any function whose range holds the patch's start grown to its end.
fn apply_patches(path: &std::path::Path, pe: &mut Pe, meta: &mut serde_json::Value, on: &mut dyn FnMut(Event))
    -> Result<(), Error> {
    let doc = crate::read_json(path)?;
    let list = doc["patches"].as_array().ok_or_else(|| Error::Meta("patches: no \"patches\" array".into()))?;
    let base = pe.image_base as u64;
    for p in list {
        let va_s = p["va"].as_str().ok_or_else(|| Error::Meta("patches: va".into()))?;
        let va = u64::from_str_radix(va_s.trim_start_matches("0x"), 16).map_err(|_| Error::Meta(format!("patches: va {}", va_s)))?;
        let bytes = unhex(p["bytes"].as_str().unwrap_or(""), va_s)?;
        if let Some(e) = p["expect"].as_str() {
            let want = unhex(e, va_s)?;
            let have = pe.get_data(va - base, Some(want.len() as i64))?.to_vec();
            if have != want {
                return Err(Error::Meta(format!("patch at {}: the image does not hold the expected bytes (another build?)", va_s)));
            }
        }
        pe.patch(va - base, &bytes)?;
        let end = va + bytes.len() as u64;
        if let Some(funcs) = meta["functions"].as_array_mut() {
            for f in funcs {
                if let Some(ranges) = f["ranges"].as_array_mut() {
                    for r in ranges {
                        let (a, b) = (r[0].as_u64().unwrap_or(0), r[1].as_u64().unwrap_or(0));
                        if a <= va && va < b && b < end {
                            r[1] = serde_json::Value::from(end);
                        }
                    }
                }
            }
        }
        let msg = format!("patched {} bytes at {}: {}", bytes.len(), va_s, p["why"].as_str().unwrap_or(""));
        on(Event::Message(&msg));
    }
    Ok(())
}

/// Runs the recompiler; `on` gets its log lines as messages and its
/// `@progress translate main <done> <total>` points.
pub fn run(opts: &Options, on: &mut dyn FnMut(Event)) -> Result<Report, Error> {
    let mut meta = crate::read_json(&opts.meta)?;
    let mut pe = Pe::parse(crate::read(&opts.image)?)?;
    if let Some(path) = &opts.patches {
        apply_patches(path, &mut pe, &mut meta, on)?;
    }
    let hooks: HashMap<u64, String> = opts.hooks.iter().map(|(n, a)| (*a, n.clone())).collect();
    let mut prog = Program::new(&meta, &pe, "f_", hooks)?;
    drop(pe);
    let what = "main";

    let mut todo: Vec<u64> = if opts.all {
        let mut v: Vec<u64> = prog.entries.iter().copied().collect();
        v.sort();
        v
    } else {
        for &e in &opts.functions {
            if !prog.entries.contains(&e) {
                return Err(Error::Translate(format!("{:#x} is not a function entry in the metadata", e)));
            }
        }
        opts.functions.clone()
    };

    if opts.all {
        let (out, inn, msg) = flags_after_return(&prog, &todo, opts.threads)?;
        on(Event::Message(&msg));
        prog.flags_out = out;
        prog.flags_in = Some(inn);
    }

    // translated, in the order recomp.py's queue pops them (the unimplemented-instruction counts
    // keep that first-seen order for ties)
    let mut done: Vec<(u64, Translated)> = Vec::new();
    if opts.all {
        todo.reverse(); // queue.pop() takes the highest entry first
        done = translate_all(&prog, &todo, what, opts.threads, on)?;
    } else {
        // the closure, one at a time (references in sorted order: recomp.py extends its queue from
        // a set, so its order, and so the order of ties in the counts, may differ)
        let mut md = Disasm::new(true);
        let mut seen = HashSet::new();
        let mut queue = todo.clone();
        while let Some(e) = queue.pop() {
            if !seen.insert(e) {
                continue;
            }
            let t = FunctionTranslator::new(&prog, &mut md, e, &prog.functions[&e]).translate()?;
            if seen.len() % PROGRESS_STEP == 0 {
                on(Event::Progress { phase: "translate", what, done: seen.len(), total: todo.len().max(seen.len()) });
            }
            let mut refs = t.referenced.clone();
            refs.sort();
            refs.dedup();
            queue.extend(refs.into_iter().filter(|r| !seen.contains(r)));
            done.push((e, t));
        }
    }

    let mut unimpl: IndexMap<String, usize> = IndexMap::new();
    let mut unimpl_funcs = HashSet::new();
    let mut hooked = HashSet::new();
    for (e, t) in &done {
        for (_, mn, why) in &t.unimpl {
            let key = if why.starts_with("x87") { why } else { mn };
            *unimpl.entry(key.clone()).or_insert(0) += 1;
            unimpl_funcs.insert(*e);
        }
        hooked.extend(t.hooked.iter().copied());
    }
    let mut counts: Vec<(String, usize)> = unimpl.into_iter().collect();
    counts.sort_by(|a, b| b.1.cmp(&a.1)); // stable: Counter.most_common
    let mut by_entry: Vec<(u64, Vec<String>)> = done.into_iter().map(|(e, t)| (e, t.lines)).collect();
    by_entry.sort_by_key(|x| x.0);
    let entries: Vec<u64> = by_entry.iter().map(|x| x.0).collect();

    std::fs::create_dir_all(&opts.out).map_err(|e| Error::io(&opts.out, e))?;
    let mut hook_names: Vec<&String> = prog.hooks.values().collect();
    hook_names.sort();
    let mut h = String::from("/* generated by recomp.py - do not edit, do not commit */\n#pragma once\n#define RT_TRANSLATION 1 /* guest.h: the translated functions' own accessors */\n#include \"guest.h\"\n\n");
    for e in &entries {
        h += &format!("void {}{:08x}(Guest* g);\nRtRegs RT_FASTCC {}{:08x}_r(RT_FAST_PARAMS);\n", prog.prefix, e, prog.prefix, e);
    }
    for name in &hook_names {
        h += &format!("extern GuestFn rt_hook_{};\n", name);
    }
    write(&opts.out.join("funcs.h"), &h)?;

    // chunks are independent files: built and written in parallel (same bytes, any order)
    let chunk = opts.chunk.max(1);
    let parts: Vec<(usize, &[(u64, Vec<String>)])> = by_entry.chunks(chunk).enumerate().collect();
    let pool = rayon::ThreadPoolBuilder::new().num_threads(opts.threads).build()
        .map_err(|e| Error::Translate(format!("thread pool: {}", e)))?;
    let written: Result<Vec<String>, Error> = pool.install(|| {
        parts.par_iter().map(|(k, part)| {
            let name = format!("funcs_{:03}.c", k);
            let mut text = String::from("/* generated by recomp.py - do not edit, do not commit */\n#include \"funcs.h\"\n\n\
                #if defined(_MSC_VER)\n#pragma warning(disable: 4102 4189 4101 4702)\n\
                #pragma code_seg(\".xlat\") /* translated code in its own section: the profiler tells it from the runtime */\n\
                #endif\n\n");
            for (_, lines) in part.iter() {
                for l in lines {
                    text += l;
                    text.push('\n');
                }
                text.push('\n');
            }
            write_if_changed(&opts.out.join(&name), &text).map(|_| name)
        }).collect()
    });
    let chunks: HashSet<String> = written?.into_iter().collect();
    let listing = std::fs::read_dir(&opts.out).map_err(|e| Error::io(&opts.out, e))?;
    for f in listing {
        let f = f.map_err(|e| Error::io(&opts.out, e))?;
        let name = f.file_name().to_string_lossy().into_owned();
        if name.starts_with("funcs_") && name.ends_with(".c") && !chunks.contains(&name) {
            std::fs::remove_file(f.path()).map_err(|e| Error::io(&f.path(), e))?;
        }
    }
    let mut missed: Vec<u64> = prog.hooks.keys().filter(|a| !hooked.contains(*a)).copied().collect();
    missed.sort();
    if !missed.is_empty() && opts.all {
        let list: Vec<String> = missed.iter().map(|a| format!("{}={:#x}", prog.hooks[a], a)).collect();
        return Err(Error::Translate(format!("--hooks: {} is not an instruction in any translated function", list.join(", "))));
    }
    let kinds = patch_kinds(&prog, &entries);
    let img = image_constants(&prog, &opts.retail)?;

    let mut report = Report { functions: entries.len(), with_unimpl: unimpl_funcs.len(),
                              unimpl: counts.clone(), patches: None };
    let mut t = String::from("/* generated by recomp.py - do not edit, do not commit */\n#include \"runtime.h\"\n#include \"funcs.h\"\n\n");
    let top: Vec<String> = counts.iter().take(40).map(|(k, v)| format!("  {:7}  {}", v, k)).collect();
    for name in &hook_names {
        t += &format!("GuestFn rt_hook_{}; /* recomp.py --hooks */\n", name);
    }
    if !prog.hooks.is_empty() {
        t += "\n";
    }
    t += "const RtEntry rt_table[] = {\n";
    for e in &entries {
        t += &format!("    {{ 0x{:08X}u, f_{:08x}, f_{:08x}_r }},\n", e, e, e);
    }
    t += &format!("}};\nconst unsigned rt_table_count = {};\n\n", entries.len());
    t += "/* How each entry is redirected into its translation: 0 = 5-byte jmp, 1 = int3 (too close\n \
          * to the next entry, or a body shorter than 5 bytes with no padding after it). */\n";
    t += "const unsigned char rt_table_patch[] = {\n";
    for row in kinds.chunks(32) {
        let r: Vec<String> = row.iter().map(|k| k.to_string()).collect();
        t += &format!("    {},\n", r.join(","));
    }
    t += "};\n\n";
    t += "/* The pinned retail build this translation belongs to. */\n";
    for (name, value) in &img {
        t += &format!("const uint32_t rt_image_{} = 0x{:X}u;\n", name, value);
    }
    write(&opts.out.join("table.c"), &t)?;
    let jmp = kinds.iter().filter(|&&k| k == 0).count();
    report.patches = Some((jmp, kinds.len() - jmp));
    on(Event::Message(&format!("entry patches: {} jmp, {} int3", jmp, kinds.len() - jmp)));
    on(Event::Message(&format!("translated {} functions ({} with unimplemented instructions)", entries.len(), unimpl_funcs.len())));
    if opts.stats || !counts.is_empty() {
        let total: usize = counts.iter().map(|c| c.1).sum();
        on(Event::Message(&format!("unimplemented instructions: {}", total)));
        for l in &top {
            on(Event::Message(l));
        }
    }
    Ok(report)
}

fn write(path: &std::path::Path, text: &str) -> Result<(), Error> {
    std::fs::write(path, text).map_err(|e| Error::io(path, e))
}

/// Translates `order` in parallel (one capstone handle per worker); the results come back in
/// `order`. Progress is reported from this thread, in order, every PROGRESS_STEP functions.
/// Which functions' returns must store the arithmetic flags: those with a call site whose caller reads
/// flags after the return before writing all of them, directly or through its own return, a jump
/// into another function, or the entry of the next function it calls (x86c.rs FlagFacts); and the
/// functions those jump into, whose returns are theirs. If a call through a pointer needs them, every
/// function does (None).
fn flags_after_return(prog: &Program, order: &[u64], threads: usize) -> Result<(Option<HashSet<u64>>, HashSet<u64>, String), Error> {
    let pool = rayon::ThreadPoolBuilder::new().num_threads(threads).build()
        .map_err(|e| Error::Translate(format!("thread pool: {}", e)))?;
    let facts: HashMap<u64, FlagFacts> = pool.install(|| {
        order.par_iter().map_init(|| Disasm::new(true), |md, &e| (e, FunctionTranslator::new(prog, md, e, &prog.functions[&e]).flag_facts()))
            .collect()
    });
    // functions a pointer may hold: entries whose address is a 4-byte-aligned word outside the code
    // (vtables, callback tables) or an immediate in it (push offset f, mov r, offset f)
    let mut taken: HashSet<u64> = facts.values().flat_map(|ff| ff.taken.iter().copied()).collect();
    let (text_lo, text_hi) = prog.text_range();
    for (k, w) in prog.image.chunks_exact(4).enumerate() {
        let at = prog.base + 4 * k as u64;
        if at >= text_lo && at < text_hi {
            continue;
        }
        let v = u32::from_le_bytes([w[0], w[1], w[2], w[3]]) as u64;
        if prog.entries.contains(&v) {
            taken.insert(v);
        }
    }
    // an entry inside another function's body (a branch target the disassembly split off) is no
    // pointer's target: data that equals one is text or numbers (0x00530046 is "F\0S")
    taken.retain(|f| !prog.inner.contains(f));
    // which functions read the flags at their entry (least fixpoint); `ind`: one a pointer may hold
    let mut live_in: HashSet<u64> = HashSet::new();
    let mut ind = false;
    loop {
        let mut changed = false;
        for (&f, ff) in &facts {
            if live_in.contains(&f) {
                continue;
            }
            let l = ff.entry.iter().any(|e| match *e {
                FlagEnd::Live => true,
                FlagEnd::Call(None) => ind,
                FlagEnd::Dead | FlagEnd::Ret => false,
                FlagEnd::Call(Some(y)) | FlagEnd::Tail(y) => live_in.contains(&y),
            });
            if l {
                live_in.insert(f);
                ind |= taken.contains(&f);
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }
    // the callees whose returns must store them
    let mut need: HashSet<u64> = HashSet::new();
    let mut indirect = 0usize;
    let (mut sites, mut dead) = (0usize, 0usize);
    let mut work: Vec<u64> = Vec::new();
    let why = std::env::var("XIVLEGACY_FLAGS_WHY").is_ok();
    for (&f, ff) in &facts {
        for (k, (x, ends)) in ff.sites.iter().enumerate() {
            if why && x.is_none() && ends.contains(&FlagEnd::Live) {
                eprintln!("pointer call site in {:08x} live: {}", f, ff.why[k]);
            }
        }
    }
    let mark = |x: Option<u64>, need: &mut HashSet<u64>, work: &mut Vec<u64>, indirect: &mut usize| match x {
        Some(x) => {
            if need.insert(x) {
                work.push(x);
            }
        }
        None => *indirect += 1,
    };
    for ff in facts.values() {
        for (x, ends) in &ff.sites {
            let x = *x;
            sites += 1;
            let now = ends.iter().any(|e| match *e {
                FlagEnd::Live => true,
                FlagEnd::Call(None) => ind,
                FlagEnd::Call(Some(y)) => live_in.contains(&y),
                FlagEnd::Tail(g) => live_in.contains(&g),
                FlagEnd::Dead | FlagEnd::Ret => false,
            });
            if ends.iter().all(|e| *e == FlagEnd::Dead) {
                dead += 1;
            }
            if now {
                mark(x, &mut need, &mut work, &mut indirect);
            }
        }
    }
    // through returns and jumps into other functions
    while let Some(f) = work.pop() {
        let Some(ff) = facts.get(&f) else { continue };
        for (x, ends) in &ff.sites {
            let x = *x;
            if ends.iter().any(|e| matches!(e, FlagEnd::Ret | FlagEnd::Tail(_))) {
                if why && x.is_none() {
                    eprintln!("pointer call site in {:08x} needed through its return", f);
                }
                mark(x, &mut need, &mut work, &mut indirect);
            }
        }
        for &t in &ff.tails {
            if need.insert(t) {
                work.push(t);
            }
        }
    }
    if std::env::var("XIVLEGACY_FLAGS_WHY").is_ok() {
        // the functions a pointer may hold that read flags at entry, without counting pointers
        let mut li: HashSet<u64> = HashSet::new();
        loop {
            let mut changed = false;
            for (&f, ff) in &facts {
                if li.contains(&f) {
                    continue;
                }
                let l = ff.entry.iter().any(|e| match *e {
                    FlagEnd::Live => true,
                    FlagEnd::Call(Some(y)) | FlagEnd::Tail(y) => li.contains(&y),
                    _ => false,
                });
                if l {
                    li.insert(f);
                    changed = true;
                }
            }
            if !changed {
                break;
            }
        }
        let mut held: Vec<u64> = li.iter().copied().filter(|f| taken.contains(f)).collect();
        held.sort();
        eprintln!("held by pointers and reading flags at entry (pointers aside): {} of {}", held.len(), li.len());
        for f in held.iter().take(40) {
            let ff = &facts[f];
            eprintln!("  {:08x} {:?} {}", f, ff.entry, ff.why.last().cloned().unwrap_or_default());
        }

        let mut c: HashMap<String, usize> = HashMap::new();
        let mut ce: HashMap<String, usize> = HashMap::new();
        let mut ck: HashMap<String, usize> = HashMap::new();
        for ff in facts.values() {
            for (k, (x, ends)) in ff.sites.iter().enumerate() {
                let live = ends.contains(&FlagEnd::Live);
                if x.is_none() {
                    let key = if live { format!("live {}", ff.why[k]) } else { format!("{:?}", ends).chars().take(16).collect() };
                    *ck.entry(key).or_insert(0) += 1;
                }
                if live {
                    *c.entry(ff.why[k].clone()).or_insert(0) += 1;
                }
            }
            if ff.entry.contains(&FlagEnd::Live) {
                *ce.entry(ff.why.last().cloned().unwrap_or_default()).or_insert(0) += 1;
            }
        }
        for (name, m) in [("sites live because", c), ("entries live because", ce), ("pointer sites", ck)] {
            let mut v: Vec<_> = m.into_iter().collect();
            v.sort_by(|a, b| b.1.cmp(&a.1));
            eprintln!("{}: {}", name, v.iter().take(14).map(|(k, n)| format!("{} {}", k, n)).collect::<Vec<_>>().join(", "));
        }
    }
    // a call through a pointer that needs them: every function a pointer may hold stores them
    if indirect > 0 {
        let more: Vec<u64> = taken.iter().copied().filter(|t| !need.contains(t)).collect();
        for t in more {
            need.insert(t);
            work.push(t);
        }
        while let Some(f) = work.pop() {
            let Some(ff) = facts.get(&f) else { continue };
            for (x, ends) in &ff.sites {
                if let Some(x) = *x {
                    if ends.iter().any(|e| matches!(e, FlagEnd::Ret | FlagEnd::Tail(_))) && need.insert(x) {
                        work.push(x);
                    }
                }
            }
            for &t in &ff.tails {
                if need.insert(t) {
                    work.push(t);
                }
            }
        }
    }
    if why {
        let mut v: Vec<u64> = need.iter().copied().collect();
        v.sort();
        eprintln!("must store flags at return: {}", v.iter().take(20).map(|f| format!("{:08x}", f)).collect::<Vec<_>>().join(" "));
    }
    let msg = format!("flags after return: {} call sites, {} dead; {} functions read flags at entry (one a pointer may hold: {}); \
                       {} of {} functions must store them at return ({} held by pointers; {} needing call sites through one)",
                      sites, dead, live_in.len(), ind, need.len(), facts.len(), taken.len(), indirect);
    Ok((Some(need), live_in, msg))
}

fn translate_all(prog: &Program, order: &[u64], what: &str, threads: usize, on: &mut dyn FnMut(Event))
    -> Result<Vec<(u64, Translated)>, Error>
{
    let total = order.len();
    let counter = AtomicUsize::new(0);
    let (tx, rx) = mpsc::channel::<usize>();
    let pool = rayon::ThreadPoolBuilder::new().num_threads(threads).build()
        .map_err(|e| Error::Translate(format!("thread pool: {}", e)))?;
    let results = std::thread::scope(|s| {
        let worker = s.spawn(|| {
            let tx = tx; // dropped when the work is done, which ends the loop below
            pool.install(|| {
                order.par_iter().map_init(|| (Disasm::new(true), tx.clone()), |(md, tx), &e| {
                    let r = FunctionTranslator::new(prog, md, e, &prog.functions[&e]).translate();
                    let n = counter.fetch_add(1, Ordering::Relaxed) + 1;
                    if n.is_multiple_of(PROGRESS_STEP) {
                        let _ = tx.send(n);
                    }
                    r.map(|t| (e, t))
                }).collect::<Vec<_>>()
            })
        });
        let mut emitted = 0;
        for n in rx {
            while emitted + PROGRESS_STEP <= n {
                emitted += PROGRESS_STEP;
                on(Event::Progress { phase: "translate", what, done: emitted, total });
            }
        }
        worker.join()
    });
    let results = results.map_err(|_| Error::Translate("a translation thread panicked".into()))?;
    results.into_iter().collect()
}

/// 0 if a 5-byte jmp fits at the entry without touching another entry or live bytes, else 1.
fn patch_kinds(prog: &Program, entries: &[u64]) -> Vec<u8> {
    let mut all: Vec<u64> = prog.entries.iter().copied().collect();
    all.sort();
    let nxt: HashMap<u64, u64> = all.windows(2).map(|w| (w[0], w[1])).collect();
    entries
        .iter()
        .map(|&e| {
            let mut ok = !prog.inner.contains(&e) && nxt.get(&e).copied().unwrap_or(e + 5) as i64 - e as i64 >= 5;
            if ok {
                let ranges = &prog.functions[&e];
                let end = ranges.iter().filter(|&&(lo, hi)| lo <= e && e < hi).map(|r| r.1).max().unwrap_or(e);
                if end < e + 5 {
                    let tail = prog.read(end, e + 5 - end);
                    ok = tail.iter().all(|b| *b == 0xCC || *b == 0x90);
                }
            }
            if ok { 0 } else { 1 }
        })
        .collect()
}

/// What the loader checks the retail image against, and the translation's own view of it.
fn image_constants(prog: &Program, retail: &std::path::Path) -> Result<Vec<(&'static str, u64)>, Error> {
    let pe = Pe::parse(crate::read(retail)?)?;
    let text = pe.section(b".text")?;
    Ok(vec![
        ("base", prog.base),
        ("timestamp", pe.timestamp as u64),
        ("size", pe.size_of_image as u64),
        ("text_rva", text.virtual_address as u64),
        ("text_size", text.virtual_size as u64),
        ("oep", prog.base + pe.entry as u64),
    ])
}
