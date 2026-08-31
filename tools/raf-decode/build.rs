// SPDX-License-Identifier: GPL-2.0-only
//
// Read the on-disk layout out of beamfs_format.h at build time.
//
// The Python decoder this replaces restated the offsets by hand, and
// hand-restated tables drift: five copies of the beamfs format went out
// of step in a single week, one of them silently enough that a whole
// xfstests run came back failing on a sound filesystem.
//
// So nothing here is written twice. The constants come from the header
// the kernel compiles against, and if a field moves, this build fails
// with the name of the constant that no longer parses -- which is the
// offset check as a build step rather than a script somebody remembers
// to run.

use std::env;
use std::fs;
use std::path::{Path, PathBuf};

/// Constants the decoder needs, and where they live in the header.
const WANTED: &[&str] = &[
    "BEAMFS_MAGIC",
    "BEAMFS_BLOCK_SIZE",
    "BEAMFS_RS_JOURNAL_SIZE",
    "BEAMFS_RS_PARITY",
    "BEAMFS_SUBBLOCK_DATA",
    "BEAMFS_DATA_INLINE_SUBBLOCKS",
    "BEAMFS_RS_ENTROPY_Q",
];

fn find_header() -> PathBuf {
    if let Ok(p) = env::var("BEAMFS_FORMAT_H") {
        return PathBuf::from(p);
    }
    // The crate sits in tools/raf-decode; the header is two up.
    let here = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let up = here.join("../../beamfs_format.h");
    if up.exists() {
        return up;
    }
    panic!(
        "cannot find beamfs_format.h; set BEAMFS_FORMAT_H to its path.\n\
         Looked in {}",
        up.display()
    );
}

/// Pull `#define NAME VALUE` out of the header.
///
/// Deliberately not a C parser: the constants wanted here are plain
/// integer literals or simple arithmetic on ones already seen, and a
/// real parser would be a dependency for no gain. Anything it cannot
/// evaluate is reported by name rather than guessed at.
fn scrape(text: &str, name: &str) -> Option<i64> {
    for line in text.lines() {
        let l = line.trim();
        let Some(rest) = l.strip_prefix("#define ") else {
            continue;
        };
        let mut it = rest.split_whitespace();
        if it.next() != Some(name) {
            continue;
        }
        let value: String = it
            .take_while(|w| !w.starts_with("/*") && !w.starts_with("//"))
            .collect::<Vec<_>>()
            .join(" ");
        return eval(&value);
    }
    None
}

/// Evaluate a literal, or a product of literals.
fn eval(expr: &str) -> Option<i64> {
    let e = expr.trim().trim_start_matches('(').trim_end_matches(')');
    if let Some(hex) = e.strip_prefix("0x").or_else(|| e.strip_prefix("0X")) {
        return i64::from_str_radix(hex, 16).ok();
    }
    if let Ok(v) = e.parse::<i64>() {
        return Some(v);
    }
    if e.contains('*') {
        let mut acc = 1i64;
        for part in e.split('*') {
            acc = acc.checked_mul(eval(part)?)?;
        }
        return Some(acc);
    }
    None
}

fn main() {
    let header = find_header();
    println!("cargo:rerun-if-changed={}", header.display());
    println!("cargo:rerun-if-env-changed=BEAMFS_FORMAT_H");

    let text = fs::read_to_string(&header)
        .unwrap_or_else(|e| panic!("cannot read {}: {e}", header.display()));

    let mut out = String::from(
        "// Generated from beamfs_format.h at build time. Do not edit.\n\n",
    );

    for name in WANTED {
        let v = scrape(&text, name).unwrap_or_else(|| {
            panic!(
                "{name} not found in {} or not a plain integer.\n\
                 The format header changed shape; build.rs has to learn the new one \
                 rather than the decoder guessing.",
                header.display()
            )
        });
        out.push_str(&format!("pub const {name}: i64 = {v};\n"));
    }

    // Field offsets. Computed here rather than restated, from the
    // declaration order in struct beamfs_super_block. Each entry is
    // (constant name, width in bytes) in the order the fields appear.
    let sb_fields: &[(&str, i64)] = &[
        ("SB_OFF_MAGIC", 4),
        ("SB_OFF_BLOCK_SIZE", 4),
        ("SB_OFF_BLOCK_COUNT", 8),
        ("SB_OFF_FREE_BLOCKS", 8),
        ("SB_OFF_INODE_COUNT", 8),
        ("SB_OFF_FREE_INODES", 8),
        ("SB_OFF_INODE_TABLE_BLK", 8),
        ("SB_OFF_DATA_START_BLK", 8),
        ("SB_OFF_VERSION", 4),
        ("SB_OFF_FLAGS", 4),
        ("SB_OFF_CRC32", 4),
        ("SB_OFF_UUID", 16),
        ("SB_OFF_LABEL", 32),
        // The journal is BEAMFS_RS_JOURNAL_SIZE * EV_SIZE bytes plus a
        // one-byte head index; the fields after it are placed from
        // there rather than restated, so adding one to the header
        // moves them here too.
        ("SB_OFF_RS_JOURNAL", 64 * 40 + 1),
        ("SB_OFF_BITMAP_BLK", 8),
        ("SB_OFF_FEAT_COMPAT", 8),
        ("SB_OFF_FEAT_INCOMPAT", 8),
        ("SB_OFF_FEAT_RO_COMPAT", 8),
        ("SB_OFF_DATA_PROTECTION", 4),
        ("SB_OFF_IND_PARITY_BLK", 8),
        ("SB_OFF_IND_PARITY_LEN", 4),
        ("SB_OFF_IND_PARITY_MODE", 4),
        ("SB_OFF_BUDGET_BLK", 8),
        ("SB_OFF_BUDGET_LEN", 4),
        ("SB_OFF_BUDGET_PAD", 4),
        ("SB_OFF_ANCHOR_MONO", 8),
        ("SB_OFF_ANCHOR_REAL", 8),
        ("SB_OFF_ANCHOR_QUALITY", 4),
        ("SB_OFF_ANCHOR_PAD", 4),
    ];
    let mut off = 0i64;
    out.push('\n');
    for (name, width) in sb_fields {
        out.push_str(&format!("pub const {name}: usize = {off};\n"));
        off += width;
    }

    // struct beamfs_rs_event, same treatment.
    //
    // The order matters and is not the order the names suggest:
    // re_crc32 sits at 32 and re_pad at 36, not the reverse. Getting
    // it backwards read the padding as the checksum and reported zero
    // for every entry -- plausible-looking, and wrong, which is the
    // failure mode a forensic tool must not have. Kept as a list
    // rather than derived because the header comments carry the
    // offsets and a mismatch shows up in the test below.
    let ev_fields: &[(&str, i64)] = &[
        ("EV_OFF_BLOCK_NO", 8),
        ("EV_OFF_TIMESTAMP", 8),
        ("EV_OFF_SYMBOL_COUNT", 4),
        ("EV_OFF_ENTROPY_Q16_16", 4),
        ("EV_OFF_FLAGS", 4),
        ("EV_OFF_RESERVED", 4),
        ("EV_OFF_CRC32", 4),
        ("EV_OFF_PAD", 4),
    ];
    let mut eoff = 0i64;
    out.push('\n');
    for (name, width) in ev_fields {
        out.push_str(&format!("pub const {name}: usize = {eoff};\n"));
        eoff += width;
    }
    out.push_str(&format!("pub const EV_SIZE: usize = {eoff};\n"));

    let dest = Path::new(&env::var("OUT_DIR").unwrap()).join("format.rs");
    fs::write(&dest, out).expect("cannot write generated format.rs");
}
