// Generated from beamfs_format.h at build time. Do not edit.

pub const BEAMFS_MAGIC: i64 = 1111834957;
pub const BEAMFS_BLOCK_SIZE: i64 = 4096;
pub const BEAMFS_RS_JOURNAL_SIZE: i64 = 64;
pub const BEAMFS_RS_PARITY: i64 = 16;
pub const BEAMFS_SUBBLOCK_DATA: i64 = 239;
pub const BEAMFS_DATA_INLINE_SUBBLOCKS: i64 = 16;
pub const BEAMFS_RS_ENTROPY_Q: i64 = 16;

pub const SB_OFF_MAGIC: usize = 0;
pub const SB_OFF_BLOCK_SIZE: usize = 4;
pub const SB_OFF_BLOCK_COUNT: usize = 8;
pub const SB_OFF_FREE_BLOCKS: usize = 16;
pub const SB_OFF_INODE_COUNT: usize = 24;
pub const SB_OFF_FREE_INODES: usize = 32;
pub const SB_OFF_INODE_TABLE_BLK: usize = 40;
pub const SB_OFF_DATA_START_BLK: usize = 48;
pub const SB_OFF_VERSION: usize = 56;
pub const SB_OFF_FLAGS: usize = 60;
pub const SB_OFF_CRC32: usize = 64;
pub const SB_OFF_UUID: usize = 68;
pub const SB_OFF_LABEL: usize = 84;
pub const SB_OFF_RS_JOURNAL: usize = 116;

pub const EV_OFF_BLOCK_NO: usize = 0;
pub const EV_OFF_TIMESTAMP: usize = 8;
pub const EV_OFF_SYMBOL_COUNT: usize = 16;
pub const EV_OFF_ENTROPY_Q16_16: usize = 20;
pub const EV_OFF_FLAGS: usize = 24;
pub const EV_OFF_RESERVED: usize = 28;
pub const EV_OFF_CRC32: usize = 32;
pub const EV_OFF_PAD: usize = 36;
pub const EV_SIZE: usize = 40;
