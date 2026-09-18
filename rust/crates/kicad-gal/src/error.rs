// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Errors produced while validating or decoding a draw stream.
//!
//! Every failure mode is a distinct variant carrying the indices involved,
//! because the producer is C++ on the other side of an FFI boundary and "the
//! stream is bad" is not a useful thing to hand a bug report.

use std::fmt;

/// Which serialised section a truncation or overflow was detected in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[allow(missing_docs)] // the variant names are the section names
pub enum Section {
    Header,
    GroupCommands,
    GroupCoords,
    Groups,
    FrameCommands,
    FrameCoords,
    Strings,
    Images,
    ImageData,
}

impl fmt::Display for Section {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = match self {
            Section::Header => "header",
            Section::GroupCommands => "group command",
            Section::GroupCoords => "group coordinate",
            Section::Groups => "group table",
            Section::FrameCommands => "frame command",
            Section::FrameCoords => "frame coordinate",
            Section::Strings => "string",
            Section::Images => "image table",
            Section::ImageData => "image data",
        };
        f.write_str(s)
    }
}

/// A draw stream that could not be accepted.
///
/// Decoding never panics and never reads out of bounds: a malformed stream
/// crossing the FFI boundary produces one of these instead.
#[derive(Debug)]
#[non_exhaustive]
// Each variant carries the indices that went wrong; the variant's own doc
// comment explains them, and repeating that per field would only add noise.
#[allow(missing_docs)]
pub enum DecodeError {
    /// The producer was built against a different ABI version.
    UnsupportedVersion { found: u32, expected: u32 },
    /// The serialised file did not start with `KGDS_MAGIC`.
    BadMagic { found: u32 },
    /// A command carried an opcode this ABI version does not define. Unknown
    /// opcodes cannot be skipped, because their argument layout — and hence
    /// what they reference — is unknown.
    UnknownOpcode { index: usize, raw: u16 },
    /// A command set a flag bit this ABI version does not define.
    UnknownFlags { index: usize, raw: u16 },
    /// `SET_TARGET`/`CLEAR_TARGET` named a render target that does not exist.
    UnknownTarget { index: usize, raw: u32 },
    /// An image table entry used an unknown pixel format.
    UnknownImageFormat { image: usize, raw: u32 },
    /// `KGDS_OP_GRID` carried a style that is not a `KIGFX::GRID_STYLE`.
    UnknownGridStyle { index: usize, raw: f64 },
    /// A command referenced coordinates outside the coordinate arena.
    CoordOutOfRange {
        index: usize,
        first: u64,
        len: u64,
        arena_len: usize,
    },
    /// A point run declared more points than can be addressed at all.
    PointCountOverflow { index: usize, points: u32 },
    /// The coordinate arena contained a NaN or infinity. Rejected up front so
    /// that non-finite values cannot reach the tessellator or the GPU, where
    /// they turn into unbounded geometry rather than a diagnosable error.
    NonFiniteCoord { coord: usize, value: f64 },
    /// A group's body ran past the end of the command array.
    GroupOutOfRange {
        group: usize,
        first: u32,
        count: u32,
        cmd_count: usize,
    },
    /// Two entries in the group table share an id, so `DRAW_GROUP` would be
    /// ambiguous.
    DuplicateGroupId { id: u32 },
    /// `DRAW_GROUP` named an id that is not in the group table.
    UnknownGroupId { index: usize, id: u32 },
    /// The group reference graph contains a cycle, which would make replay
    /// non-terminating.
    GroupCycle { id: u32 },
    /// Groups nest deeper than [`crate::MAX_GROUP_DEPTH`].
    GroupTooDeep { id: u32, depth: usize },
    /// `KGDS_OP_BITMAP` referenced an image table entry that does not exist.
    ImageOutOfRange {
        index: usize,
        image: u32,
        image_count: usize,
    },
    /// An image's pixels lie outside the image arena.
    ImageDataOutOfRange {
        image: usize,
        offset: u64,
        len: u64,
        arena_len: usize,
    },
    /// An image's declared size does not match `width * height * bpp`.
    ImageSizeMismatch {
        image: usize,
        declared: u64,
        expected: u64,
    },
    /// A section was shorter than the header said it would be.
    Truncated {
        section: Section,
        expected: u64,
        found: u64,
    },
    /// A declared section size cannot be represented on this target, which
    /// means the file is corrupt rather than merely large.
    SectionTooLarge { section: Section, count: u64 },
    /// A pointer/length pair from the FFI boundary described a slice longer
    /// than `isize::MAX` bytes, which cannot be formed safely.
    SliceTooLarge { section: Section, count: usize },
    /// A non-empty section had a null pointer.
    NullSection { section: Section },
    /// A section pointer was not aligned for its element type. Forming a slice
    /// from it would be undefined behaviour, so it is rejected instead.
    MisalignedSection { section: Section, align: usize },
    /// Underlying I/O failure while reading or writing a serialised stream.
    Io(std::io::Error),
}

impl fmt::Display for DecodeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            DecodeError::UnsupportedVersion { found, expected } => write!(
                f,
                "draw stream ABI version {found} is not supported (this build speaks {expected})"
            ),
            DecodeError::BadMagic { found } => {
                write!(f, "not a KGDS stream: magic {found:#010x}")
            }
            DecodeError::UnknownOpcode { index, raw } => {
                write!(f, "command {index}: unknown opcode {raw:#06x}")
            }
            DecodeError::UnknownFlags { index, raw } => {
                write!(f, "command {index}: unknown flag bits in {raw:#06x}")
            }
            DecodeError::UnknownTarget { index, raw } => {
                write!(f, "command {index}: unknown render target {raw}")
            }
            DecodeError::UnknownImageFormat { image, raw } => {
                write!(f, "image {image}: unknown pixel format {raw}")
            }
            DecodeError::UnknownGridStyle { index, raw } => {
                write!(f, "command {index}: unknown grid style {raw}")
            }
            DecodeError::CoordOutOfRange {
                index,
                first,
                len,
                arena_len,
            } => write!(
                f,
                "command {index}: coords[{first}..{}] outside arena of {arena_len}",
                first.saturating_add(*len)
            ),
            DecodeError::PointCountOverflow { index, points } => {
                write!(f, "command {index}: point count {points} is not addressable")
            }
            DecodeError::NonFiniteCoord { coord, value } => {
                write!(f, "coords[{coord}] is not finite ({value})")
            }
            DecodeError::GroupOutOfRange {
                group,
                first,
                count,
                cmd_count,
            } => write!(
                f,
                "group {group}: body [{first}..{}) outside command array of {cmd_count}",
                (*first as u64) + (*count as u64)
            ),
            DecodeError::DuplicateGroupId { id } => {
                write!(f, "group id {id} appears more than once in the group table")
            }
            DecodeError::UnknownGroupId { index, id } => {
                write!(f, "command {index}: DRAW_GROUP of unknown group id {id}")
            }
            DecodeError::GroupCycle { id } => {
                write!(f, "group id {id} takes part in a cycle of DRAW_GROUP references")
            }
            DecodeError::GroupTooDeep { id, depth } => {
                write!(f, "group id {id} nests {depth} deep, past the supported limit")
            }
            DecodeError::ImageOutOfRange {
                index,
                image,
                image_count,
            } => write!(
                f,
                "command {index}: image {image} outside image table of {image_count}"
            ),
            DecodeError::ImageDataOutOfRange {
                image,
                offset,
                len,
                arena_len,
            } => write!(
                f,
                "image {image}: bytes [{offset}..{}) outside arena of {arena_len}",
                offset.saturating_add(*len)
            ),
            DecodeError::ImageSizeMismatch {
                image,
                declared,
                expected,
            } => write!(
                f,
                "image {image}: declares {declared} bytes but its dimensions need {expected}"
            ),
            DecodeError::Truncated {
                section,
                expected,
                found,
            } => write!(
                f,
                "{section} section truncated: expected {expected} bytes, found {found}"
            ),
            DecodeError::SectionTooLarge { section, count } => {
                write!(f, "{section} section declares {count} entries, which cannot be read")
            }
            DecodeError::SliceTooLarge { section, count } => {
                write!(f, "{section} section of {count} entries is too large to borrow")
            }
            DecodeError::NullSection { section } => {
                write!(f, "{section} section is non-empty but its pointer is null")
            }
            DecodeError::MisalignedSection { section, align } => {
                write!(f, "{section} section pointer is not {align}-byte aligned")
            }
            DecodeError::Io(e) => write!(f, "I/O error: {e}"),
        }
    }
}

impl std::error::Error for DecodeError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            DecodeError::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<std::io::Error> for DecodeError {
    fn from(e: std::io::Error) -> Self {
        DecodeError::Io(e)
    }
}
