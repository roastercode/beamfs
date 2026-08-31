// SPDX-License-Identifier: GPL-2.0-only
//
// raf-decode -- forensic decoder for the beamfs radiation event journal
//
// Author: Aurelien Desbrieres <aurelien@hackers.camp>

//! Reads a beamfs volume and prints the 64-slot ring buffer of
//! radiation events the kernel keeps in the superblock.
//!
//! Where beamfsd signs and forwards events as they happen, this reads
//! them afterwards, off a device or an image, for audit and for
//! checking what a test campaign actually did to a volume.
//!
//! The layout comes from beamfs_format.h at build time rather than
//! being restated here. The Python version this replaces kept its own
//! copy of the offsets, and copies drift: five copies of the format
//! went out of step in one week, one of them quietly enough that an
//! entire xfstests run came back failing on a healthy filesystem. A
//! forensic tool that reads the wrong bytes is worse than none, since
//! it produces evidence rather than an error.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::PathBuf;
use std::process::ExitCode;

mod format {
    // build.rs emits the whole layout; this binary reads part of it.
    // The unused ones are kept because they are the layout, and a
    // decoder that carries only the fields it happens to print today
    // is a decoder somebody will extend by restating an offset.
    #![allow(dead_code)]
    include!(concat!(env!("OUT_DIR"), "/format.rs"));
}

/// UNCORRECTABLE: the subblock exceeded the correction radius.
const FLAG_UNCORRECTABLE: u32 = 1 << 1;
/// ENTROPY_VALID: the entropy field holds a measurement.
const FLAG_ENTROPY_VALID: u32 = 1 << 0;

/// Subblocks per data block: a data-block event encodes its subblock
/// arithmetically, `block_no = phys * SUBBLOCKS + subblock`.
///
/// re_block_no does not hold a block number on those entries, despite
/// the name. Worth knowing before reading a dump.
const SUBBLOCKS: u64 = 16;

#[derive(Debug)]
struct Superblock {
    magic: u32,
    version: u32,
    block_count: u64,
    free_blocks: u64,
    journal_head: u8,
}

#[derive(Debug)]
struct Event {
    slot: usize,
    block_no: u64,
    timestamp: u64,
    symbols: u32,
    entropy_q16: u32,
    flags: u32,
    crc_stored: u32,
    empty: bool,
}

impl Event {
    fn uncorrectable(&self) -> bool {
        self.flags & FLAG_UNCORRECTABLE != 0
    }

    fn entropy_valid(&self) -> bool {
        self.flags & FLAG_ENTROPY_VALID != 0
    }

    /// The (block, subblock) the entry refers to.
    fn coordinates(&self) -> (u64, u64) {
        (self.block_no / SUBBLOCKS, self.block_no % SUBBLOCKS)
    }

    /// Whether the entry keeps the invariants format-v4.md 6.5 states.
    ///
    /// An UNCORRECTABLE entry must carry a symbol count of zero and a
    /// cleared ENTROPY_VALID: there was nothing to count and nothing
    /// to measure. An entry that breaks this is either a decoder bug
    /// or a corrupted journal, and either way should not be read as
    /// evidence.
    fn invariants_hold(&self) -> bool {
        if !self.uncorrectable() {
            return true;
        }
        self.symbols == 0 && !self.entropy_valid()
    }
}

fn le32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

fn le64(b: &[u8], off: usize) -> u64 {
    let mut v = [0u8; 8];
    v.copy_from_slice(&b[off..off + 8]);
    u64::from_le_bytes(v)
}

fn read_superblock(buf: &[u8]) -> Superblock {
    let jbase = format::SB_OFF_RS_JOURNAL;
    #[allow(clippy::cast_sign_loss)]
    let jsize = (format::BEAMFS_RS_JOURNAL_SIZE as usize) * format::EV_SIZE;
    Superblock {
        magic: le32(buf, format::SB_OFF_MAGIC),
        version: le32(buf, format::SB_OFF_VERSION),
        block_count: le64(buf, format::SB_OFF_BLOCK_COUNT),
        free_blocks: le64(buf, format::SB_OFF_FREE_BLOCKS),
        journal_head: buf[jbase + jsize],
    }
}

fn read_event(buf: &[u8], slot: usize) -> Event {
    let base = format::SB_OFF_RS_JOURNAL + slot * format::EV_SIZE;
    let block_no = le64(buf, base + format::EV_OFF_BLOCK_NO);
    let timestamp = le64(buf, base + format::EV_OFF_TIMESTAMP);
    let symbols = le32(buf, base + format::EV_OFF_SYMBOL_COUNT);
    Event {
        slot,
        block_no,
        timestamp,
        symbols,
        entropy_q16: le32(buf, base + format::EV_OFF_ENTROPY_Q16_16),
        flags: le32(buf, base + format::EV_OFF_FLAGS),
        crc_stored: le32(buf, base + format::EV_OFF_CRC32),
        empty: block_no == 0 && timestamp == 0 && symbols == 0,
    }
}

/// The magic as an unsigned value.
///
/// The header spells it as a plain integer literal, so build.rs sees
/// it signed; the on-disk field is a __le32.
#[allow(clippy::cast_sign_loss)]
fn magic_expected() -> u32 {
    format::BEAMFS_MAGIC as u32
}

fn print_text(sb: &Superblock, events: &[Event]) {
    println!("superblock");
    println!(
        "  magic          0x{:08x} ({})",
        sb.magic,
        if sb.magic == magic_expected() { "ok" } else { "BAD" }
    );
    println!("  version        {}", sb.version);
    println!("  blocks         {}", sb.block_count);
    println!("  free           {}", sb.free_blocks);
    println!("  journal head   {}", sb.journal_head);
    println!();

    let live: Vec<&Event> = events.iter().filter(|e| !e.empty).collect();
    if live.is_empty() {
        println!("journal empty");
        return;
    }

    println!("journal: {} of {} slots used", live.len(), events.len());
    println!();
    println!("  slot         block   subblock  symbols     flags  state");
    for e in &live {
        let (blk, sub) = e.coordinates();
        let state = if e.uncorrectable() {
            if e.invariants_hold() {
                "uncorrectable"
            } else {
                "uncorrectable (INVARIANTS BROKEN)"
            }
        } else {
            "corrected"
        };
        println!(
            "  {:>4}  {:>12}  {:>9}  {:>7}  {:>08x}  {}",
            e.slot, blk, sub, e.symbols, e.flags, state
        );
    }

    println!();
    let unc = live.iter().filter(|e| e.uncorrectable()).count();
    let bad = live.iter().filter(|e| !e.invariants_hold()).count();
    println!("  corrected      {}", live.len() - unc);
    println!("  uncorrectable  {unc}");
    if bad > 0 {
        println!("  BROKEN         {bad} entries violate the flag invariants");
    }
}

fn print_json(sb: &Superblock, events: &[Event]) {
    println!("{{");
    println!("  \"superblock\": {{");
    println!("    \"magic\": {},", sb.magic);
    println!("    \"version\": {},", sb.version);
    println!("    \"block_count\": {},", sb.block_count);
    println!("    \"free_blocks\": {},", sb.free_blocks);
    println!("    \"rs_journal_head\": {}", sb.journal_head);
    println!("  }},");
    println!("  \"events\": [");
    let live: Vec<&Event> = events.iter().filter(|e| !e.empty).collect();
    for (i, e) in live.iter().enumerate() {
        let (blk, sub) = e.coordinates();
        let comma = if i + 1 == live.len() { "" } else { "," };
        println!(
            "    {{\"slot\": {}, \"block_no\": {}, \"block\": {}, \"subblock\": {}, \
             \"timestamp\": {}, \"symbols\": {}, \"entropy_q16\": {}, \"flags\": {}, \
             \"uncorrectable\": {}, \"entropy_valid\": {}, \"invariants_ok\": {}, \
             \"crc32\": {}}}{}",
            e.slot,
            e.block_no,
            blk,
            sub,
            e.timestamp,
            e.symbols,
            e.entropy_q16,
            e.flags,
            e.uncorrectable(),
            e.entropy_valid(),
            e.invariants_hold(),
            e.crc_stored,
            comma
        );
    }
    println!("  ]");
    println!("}}");
}

fn usage() -> ExitCode {
    eprintln!(
        "usage: raf-decode [--json] <device-or-image>\n\
         \n\
         Prints the radiation event journal from a beamfs superblock.\n\
         Layout is taken from beamfs_format.h at build time, so this\n\
         cannot disagree with the kernel about where the fields are."
    );
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let json = args.iter().any(|a| a == "--json");
    let Some(path) = args.iter().find(|a| !a.starts_with("--")) else {
        return usage();
    };
    let path = PathBuf::from(path);

    let mut f = match File::open(&path) {
        Ok(f) => f,
        Err(e) => {
            eprintln!("raf-decode: {}: {e}", path.display());
            return ExitCode::FAILURE;
        }
    };

    #[allow(clippy::cast_sign_loss)]
    let bs = format::BEAMFS_BLOCK_SIZE as usize;
    let mut buf = vec![0u8; bs];
    if let Err(e) = f.seek(SeekFrom::Start(0)).and_then(|_| f.read_exact(&mut buf)) {
        eprintln!("raf-decode: cannot read superblock: {e}");
        return ExitCode::FAILURE;
    }

    let sb = read_superblock(&buf);
    if sb.magic != magic_expected() {
        eprintln!(
            "raf-decode: not a beamfs volume (magic 0x{:08x})",
            sb.magic
        );
        return ExitCode::FAILURE;
    }

    #[allow(clippy::cast_sign_loss)]
    let slots = format::BEAMFS_RS_JOURNAL_SIZE as usize;
    let events: Vec<Event> = (0..slots).map(|i| read_event(&buf, i)).collect();

    if json {
        print_json(&sb, &events);
    } else {
        print_text(&sb, &events);
    }

    // A journal with entries that break their own invariants is not
    // evidence, and the exit status should say so to a script.
    if events.iter().any(|e| !e.empty && !e.invariants_hold()) {
        return ExitCode::FAILURE;
    }
    ExitCode::SUCCESS
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ev(block_no: u64, symbols: u32, flags: u32) -> Event {
        Event {
            slot: 0,
            block_no,
            timestamp: 1,
            symbols,
            entropy_q16: 0,
            flags,
            crc_stored: 0,
            empty: false,
        }
    }

    #[test]
    fn coordinates_split_block_and_subblock() {
        // 145 * 16 + 0, the saturation protocol's first target.
        let (b, s) = ev(2320, 0, FLAG_UNCORRECTABLE).coordinates();
        assert_eq!((b, s), (145, 0));
    }

    #[test]
    fn uncorrectable_needs_zero_symbols() {
        assert!(ev(2320, 0, FLAG_UNCORRECTABLE).invariants_hold());
        assert!(!ev(2320, 3, FLAG_UNCORRECTABLE).invariants_hold());
    }

    #[test]
    fn uncorrectable_needs_entropy_cleared() {
        let e = ev(2320, 0, FLAG_UNCORRECTABLE | FLAG_ENTROPY_VALID);
        assert!(!e.invariants_hold());
    }

    #[test]
    fn a_corrected_entry_carries_no_invariant() {
        // Corrections have symbols and may have entropy; only the
        // uncorrectable case is constrained.
        assert!(ev(2320, 6, FLAG_ENTROPY_VALID).invariants_hold());
    }

    #[test]
    fn generated_offsets_are_sane() {
        // The point of build.rs: if the header moves a field, these
        // change with it. Pinned so a silent shift fails here rather
        // than in a forensic report.
        assert_eq!(format::EV_OFF_BLOCK_NO, 0);
        assert_eq!(format::EV_OFF_TIMESTAMP, 8);
        assert_eq!(format::EV_SIZE, 40);
        // crc32 before pad, as the header has it. Reversed, the
        // decoder reads padding as the checksum and reports zero for
        // every entry.
        assert_eq!(format::EV_OFF_CRC32, 32);
        assert_eq!(format::EV_OFF_PAD, 36);
        assert_eq!(format::SB_OFF_MAGIC, 0);
        assert_eq!(format::SB_OFF_CRC32, 64);
    }
}
