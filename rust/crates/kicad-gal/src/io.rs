// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The `kgds_file_header` on-disk format.
//!
//! Streams recorded from repository fixtures are checked in and replayed by the
//! renderer's tests, so this is a test fixture format first and a wire format
//! second. It is written and read field by field in little-endian order rather
//! than by reinterpreting bytes, so that files stay portable and a big-endian
//! host would still produce byte-identical output.

use std::io::{Read, Write};

use crate::abi::{kgds_cmd, kgds_group, kgds_image, KGDS_MAGIC, KGDS_VERSION};
use crate::error::{DecodeError, Section};
use crate::stream::{Stream, StreamParts};

/// Size of `kgds_file_header` on disk.
const HEADER_BYTES: usize = 80;

/// Sections are padded to this boundary, as the header documents.
const SECTION_ALIGN: usize = 8;

fn pad_to_align(n: usize) -> usize {
    n.next_multiple_of(SECTION_ALIGN) - n
}

struct Cursor<'a> {
    bytes: &'a [u8],
    pos: usize,
}

impl<'a> Cursor<'a> {
    fn take(&mut self, n: usize, section: Section) -> Result<&'a [u8], DecodeError> {
        let end = self
            .pos
            .checked_add(n)
            .ok_or(DecodeError::SectionTooLarge {
                section,
                count: n as u64,
            })?;
        let slice = self
            .bytes
            .get(self.pos..end)
            .ok_or(DecodeError::Truncated {
                section,
                expected: end as u64,
                found: self.bytes.len() as u64,
            })?;
        self.pos = end;
        Ok(slice)
    }

    /// Skip the padding that follows a section, tolerating a file that stops
    /// exactly at the end of its last section without a trailing pad.
    fn align(&mut self) {
        self.pos = self
            .pos
            .next_multiple_of(SECTION_ALIGN)
            .min(self.bytes.len());
    }
}

fn rd_u32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

fn rd_u64(b: &[u8], off: usize) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[off..off + 8]);
    u64::from_le_bytes(a)
}

/// Convert a count declared in the header into a byte length, rejecting the
/// values that could only come from a corrupt file.
fn section_bytes(count: u64, elem: usize, section: Section) -> Result<usize, DecodeError> {
    let bytes = count
        .checked_mul(elem as u64)
        .ok_or(DecodeError::SectionTooLarge { section, count })?;
    usize::try_from(bytes).map_err(|_| DecodeError::SectionTooLarge { section, count })
}

fn read_cmds(
    cur: &mut Cursor<'_>,
    count: u64,
    section: Section,
) -> Result<Vec<kgds_cmd>, DecodeError> {
    let n = usize::try_from(count).map_err(|_| DecodeError::SectionTooLarge { section, count })?;
    let raw = cur.take(section_bytes(count, 24, section)?, section)?;
    let mut out = Vec::with_capacity(n);
    for i in 0..n {
        let b = &raw[i * 24..];
        out.push(kgds_cmd {
            op: u16::from_le_bytes([b[0], b[1]]),
            flags: u16::from_le_bytes([b[2], b[3]]),
            arg0: rd_u32(b, 4),
            arg1: rd_u32(b, 8),
            arg2: rd_u32(b, 12),
            arg3: rd_u32(b, 16),
            arg4: rd_u32(b, 20),
        });
    }
    cur.align();
    Ok(out)
}

fn read_coords(
    cur: &mut Cursor<'_>,
    count: u64,
    section: Section,
) -> Result<Vec<f64>, DecodeError> {
    let n = usize::try_from(count).map_err(|_| DecodeError::SectionTooLarge { section, count })?;
    let raw = cur.take(section_bytes(count, 8, section)?, section)?;
    let mut out = Vec::with_capacity(n);
    for i in 0..n {
        out.push(f64::from_bits(rd_u64(raw, i * 8)));
    }
    cur.align();
    Ok(out)
}

impl Stream {
    /// Read a serialised stream.
    ///
    /// The whole file is read before anything is parsed, but never more than it
    /// actually contains: a header claiming an absurd section size fails as a
    /// truncation rather than as an allocation, because nothing is sized from
    /// the header until the bytes behind it are known to be present.
    pub fn read(mut reader: impl Read) -> Result<Stream, DecodeError> {
        let mut bytes = Vec::new();
        reader.read_to_end(&mut bytes)?;
        Stream::from_bytes(&bytes)
    }

    /// Parse a serialised stream already in memory.
    pub fn from_bytes(bytes: &[u8]) -> Result<Stream, DecodeError> {
        if bytes.len() < HEADER_BYTES {
            return Err(DecodeError::Truncated {
                section: Section::Header,
                expected: HEADER_BYTES as u64,
                found: bytes.len() as u64,
            });
        }

        let magic = rd_u32(bytes, 0);
        if magic != KGDS_MAGIC {
            return Err(DecodeError::BadMagic { found: magic });
        }
        let version = rd_u32(bytes, 4);
        if version != KGDS_VERSION {
            return Err(DecodeError::UnsupportedVersion {
                found: version,
                expected: KGDS_VERSION,
            });
        }
        let flags = rd_u32(bytes, 8);
        // bytes[12..16] is `reserved`.

        let group_cmd_count = rd_u64(bytes, 16);
        let group_coord_count = rd_u64(bytes, 24);
        let group_count = rd_u64(bytes, 32);
        let frame_cmd_count = rd_u64(bytes, 40);
        let frame_coord_count = rd_u64(bytes, 48);
        let string_bytes = rd_u64(bytes, 56);
        let image_count = rd_u64(bytes, 64);
        let image_bytes = rd_u64(bytes, 72);

        let mut cur = Cursor {
            bytes,
            pos: HEADER_BYTES,
        };

        let group_cmds = read_cmds(&mut cur, group_cmd_count, Section::GroupCommands)?;
        let group_coords = read_coords(&mut cur, group_coord_count, Section::GroupCoords)?;

        let n_groups = usize::try_from(group_count).map_err(|_| DecodeError::SectionTooLarge {
            section: Section::Groups,
            count: group_count,
        })?;
        let raw = cur.take(
            section_bytes(group_count, 16, Section::Groups)?,
            Section::Groups,
        )?;
        let mut groups = Vec::with_capacity(n_groups);
        for i in 0..n_groups {
            let b = &raw[i * 16..];
            groups.push(kgds_group {
                id: rd_u32(b, 0),
                serial: rd_u32(b, 4),
                first_cmd: rd_u32(b, 8),
                cmd_count: rd_u32(b, 12),
            });
        }
        cur.align();

        let frame_cmds = read_cmds(&mut cur, frame_cmd_count, Section::FrameCommands)?;
        let frame_coords = read_coords(&mut cur, frame_coord_count, Section::FrameCoords)?;

        let n = usize::try_from(string_bytes).map_err(|_| DecodeError::SectionTooLarge {
            section: Section::Strings,
            count: string_bytes,
        })?;
        let strings = cur.take(n, Section::Strings)?.to_vec();
        cur.align();

        let n_images = usize::try_from(image_count).map_err(|_| DecodeError::SectionTooLarge {
            section: Section::Images,
            count: image_count,
        })?;
        let raw = cur.take(
            section_bytes(image_count, 32, Section::Images)?,
            Section::Images,
        )?;
        let mut images = Vec::with_capacity(n_images);
        for i in 0..n_images {
            let b = &raw[i * 32..];
            images.push(kgds_image {
                width: rd_u32(b, 0),
                height: rd_u32(b, 4),
                format: rd_u32(b, 8),
                reserved: rd_u32(b, 12),
                data_offset: rd_u64(b, 16),
                data_length: rd_u64(b, 24),
            });
        }
        cur.align();

        let n = usize::try_from(image_bytes).map_err(|_| DecodeError::SectionTooLarge {
            section: Section::ImageData,
            count: image_bytes,
        })?;
        let image_data = cur.take(n, Section::ImageData)?.to_vec();

        Stream::from_parts(StreamParts {
            version,
            flags,
            group_cmds: &group_cmds,
            group_coords: &group_coords,
            groups: &groups,
            frame_cmds: &frame_cmds,
            frame_coords: &frame_coords,
            strings: &strings,
            images: &images,
            image_data: &image_data,
        })
    }

    /// Write the stream in the `kgds_file_header` format.
    pub fn write(&self, mut writer: impl Write) -> Result<(), DecodeError> {
        writer.write_all(&self.to_bytes())?;
        Ok(())
    }

    /// Serialise to a byte vector.
    pub fn to_bytes(&self) -> Vec<u8> {
        fn put_cmds(out: &mut Vec<u8>, cmds: &[kgds_cmd]) {
            for c in cmds {
                out.extend_from_slice(&c.op.to_le_bytes());
                out.extend_from_slice(&c.flags.to_le_bytes());
                out.extend_from_slice(&c.arg0.to_le_bytes());
                out.extend_from_slice(&c.arg1.to_le_bytes());
                out.extend_from_slice(&c.arg2.to_le_bytes());
                out.extend_from_slice(&c.arg3.to_le_bytes());
                out.extend_from_slice(&c.arg4.to_le_bytes());
            }
        }
        fn put_coords(out: &mut Vec<u8>, coords: &[f64]) {
            for v in coords {
                out.extend_from_slice(&v.to_bits().to_le_bytes());
            }
        }
        fn pad(out: &mut Vec<u8>) {
            out.resize(out.len() + pad_to_align(out.len()), 0);
        }

        let mut out = Vec::with_capacity(
            HEADER_BYTES
                + (self.group_cmds.len() + self.frame_cmds.len()) * 24
                + (self.group_coords.len() + self.frame_coords.len()) * 8
                + self.groups.len() * 16
                + self.strings.len()
                + self.images.len() * 32
                + self.image_data.len()
                + 8 * SECTION_ALIGN,
        );

        out.extend_from_slice(&KGDS_MAGIC.to_le_bytes());
        out.extend_from_slice(&KGDS_VERSION.to_le_bytes());
        out.extend_from_slice(&self.flags.to_le_bytes());
        out.extend_from_slice(&0u32.to_le_bytes()); // reserved
        out.extend_from_slice(&(self.group_cmds.len() as u64).to_le_bytes());
        out.extend_from_slice(&(self.group_coords.len() as u64).to_le_bytes());
        out.extend_from_slice(&(self.groups.len() as u64).to_le_bytes());
        out.extend_from_slice(&(self.frame_cmds.len() as u64).to_le_bytes());
        out.extend_from_slice(&(self.frame_coords.len() as u64).to_le_bytes());
        out.extend_from_slice(&(self.strings.len() as u64).to_le_bytes());
        out.extend_from_slice(&(self.images.len() as u64).to_le_bytes());
        out.extend_from_slice(&(self.image_data.len() as u64).to_le_bytes());
        debug_assert_eq!(out.len(), HEADER_BYTES);

        put_cmds(&mut out, &self.group_cmds);
        pad(&mut out);
        put_coords(&mut out, &self.group_coords);
        pad(&mut out);

        for g in &self.groups {
            out.extend_from_slice(&g.id.to_le_bytes());
            out.extend_from_slice(&g.serial.to_le_bytes());
            out.extend_from_slice(&g.first_cmd.to_le_bytes());
            out.extend_from_slice(&g.cmd_count.to_le_bytes());
        }
        pad(&mut out);

        put_cmds(&mut out, &self.frame_cmds);
        pad(&mut out);
        put_coords(&mut out, &self.frame_coords);
        pad(&mut out);

        out.extend_from_slice(&self.strings);
        pad(&mut out);

        for im in &self.images {
            out.extend_from_slice(&im.width.to_le_bytes());
            out.extend_from_slice(&im.height.to_le_bytes());
            out.extend_from_slice(&im.format.to_le_bytes());
            out.extend_from_slice(&im.reserved.to_le_bytes());
            out.extend_from_slice(&im.data_offset.to_le_bytes());
            out.extend_from_slice(&im.data_length.to_le_bytes());
        }
        pad(&mut out);

        out.extend_from_slice(&self.image_data);
        pad(&mut out);

        out
    }
}
