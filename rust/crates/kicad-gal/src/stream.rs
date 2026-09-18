// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Validated access to a draw stream, borrowed ([`StreamView`]) or owned
//! ([`Stream`]).
//!
//! Both go through the same validation pass, and nothing else in this crate can
//! construct either type. That is deliberate: once a `StreamView` exists, every
//! index in it is known to be in range, every opcode known, every group
//! reference resolvable and acyclic — so the renderer's hot loop can be written
//! without a bounds check of its own.
//!
//! # Two arenas
//!
//! Retained group geometry and the current frame have separate command arrays
//! and separate coordinate arenas, because `KIGFX::VIEW` records groups during
//! its update pass and the frame during its draw pass, interleaved over time.
//! A command's coordinate indices always refer to the arena that command lives
//! in, and validation checks each arena against its own length.

use std::borrow::Cow;

use crate::abi::{
    kgds_cmd, kgds_group, kgds_image, kgds_stream_view, ImageFormat, Op, KGDS_VERSION,
};
use crate::command::{decode_in, Command, CommandIter};
use crate::error::{DecodeError, Section};

/// How deep `DRAW_GROUP` references may nest.
///
/// `KIGFX::VIEW` does not nest groups at all today, so anything beyond a
/// handful is a corrupt stream rather than a legitimate one. The limit exists
/// to bound the renderer's replay recursion, which would otherwise be a stack
/// overflow waiting for a malformed input.
pub const MAX_GROUP_DEPTH: usize = 16;

/// Lookup from group id to group table index.
///
/// Producers overwhelmingly number groups `0..n`, so the common case is stored
/// as nothing at all; anything else gets a sorted side table.
#[derive(Clone, Debug, PartialEq, Eq)]
enum GroupIndex<'a> {
    /// `groups[i].id == i` for every `i`.
    Identity,
    /// `(id, table index)` pairs, sorted by id.
    Sorted(Cow<'a, [(u32, u32)]>),
}

impl GroupIndex<'_> {
    fn resolve(&self, id: u32, group_count: usize) -> Option<usize> {
        match self {
            GroupIndex::Identity => {
                let i = id as usize;
                (i < group_count).then_some(i)
            }
            GroupIndex::Sorted(pairs) => pairs
                .binary_search_by_key(&id, |(k, _)| *k)
                .ok()
                .map(|slot| pairs[slot].1 as usize),
        }
    }

    fn to_owned_index(&self) -> GroupIndex<'static> {
        match self {
            GroupIndex::Identity => GroupIndex::Identity,
            GroupIndex::Sorted(pairs) => GroupIndex::Sorted(Cow::Owned(pairs.to_vec())),
        }
    }

    fn borrowed(&self) -> GroupIndex<'_> {
        match self {
            GroupIndex::Identity => GroupIndex::Identity,
            GroupIndex::Sorted(pairs) => GroupIndex::Sorted(Cow::Borrowed(pairs.as_ref())),
        }
    }
}

/// Which of the two arenas a command came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Arena {
    /// Retained group geometry.
    Group,
    /// This frame's command list.
    Frame,
}

/// The raw sections of a stream, as handed to [`StreamView::new`].
///
/// Grouping them keeps the constructor from taking a dozen positional
/// arguments whose order would be easy to get wrong.
#[derive(Clone, Copy, Debug)]
pub struct StreamParts<'a> {
    /// ABI version the producer was built against.
    pub version: u32,
    /// Producer-defined stream flags. Not interpreted here.
    pub flags: u32,
    /// Retained geometry. Indices in these commands use `group_coords`.
    pub group_cmds: &'a [kgds_cmd],
    /// The coordinate arena group bodies index into.
    pub group_coords: &'a [f64],
    /// The group table; each entry's `first_cmd` indexes `group_cmds`.
    pub groups: &'a [kgds_group],
    /// This frame's commands. Indices in them use `frame_coords`.
    pub frame_cmds: &'a [kgds_cmd],
    /// The coordinate arena frame commands index into.
    pub frame_coords: &'a [f64],
    /// The string arena.
    pub strings: &'a [u8],
    /// The image table.
    pub images: &'a [kgds_image],
    /// The image pixel arena.
    pub image_data: &'a [u8],
}

impl Default for StreamParts<'_> {
    fn default() -> Self {
        StreamParts {
            version: KGDS_VERSION,
            flags: 0,
            group_cmds: &[],
            group_coords: &[],
            groups: &[],
            frame_cmds: &[],
            frame_coords: &[],
            strings: &[],
            images: &[],
            image_data: &[],
        }
    }
}

/// A validated, borrowed view of a complete draw stream.
///
/// Construct it from raw FFI pointers with [`StreamView::from_raw`] or from
/// Rust slices with [`StreamView::new`]. Either way the whole stream is checked
/// before the value exists.
#[derive(Clone, Debug)]
pub struct StreamView<'a> {
    version: u32,
    flags: u32,
    group_cmds: &'a [kgds_cmd],
    group_coords: &'a [f64],
    groups: &'a [kgds_group],
    frame_cmds: &'a [kgds_cmd],
    frame_coords: &'a [f64],
    strings: &'a [u8],
    images: &'a [kgds_image],
    image_data: &'a [u8],
    group_index: GroupIndex<'a>,
}

impl<'a> StreamView<'a> {
    /// Validate Rust slices into a view.
    pub fn new(parts: StreamParts<'a>) -> Result<StreamView<'a>, DecodeError> {
        let group_index = validate(&parts)?;
        Ok(StreamView {
            version: parts.version,
            flags: parts.flags,
            group_cmds: parts.group_cmds,
            group_coords: parts.group_coords,
            groups: parts.groups,
            frame_cmds: parts.frame_cmds,
            frame_coords: parts.frame_coords,
            strings: parts.strings,
            images: parts.images,
            image_data: parts.image_data,
            group_index,
        })
    }

    /// Borrow a stream recorded by the C++ producer.
    ///
    /// # Safety
    ///
    /// The caller guarantees that, for the whole of `'a`:
    ///
    /// * each non-empty section pointer in `raw` points at that many
    ///   initialised, correctly aligned elements of the matching type;
    /// * none of that memory is mutated; and
    /// * the memory outlives `'a`.
    ///
    /// That is exactly the contract `kgds_stream_view` documents: the producer
    /// owns the buffers and keeps them alive until the next recording pass, so
    /// `'a` must not outlive that pass.
    ///
    /// Everything else — ranges, opcodes, group references, image extents — is
    /// checked here, so a producer bug short of violating the above yields a
    /// [`DecodeError`] rather than undefined behaviour.
    pub unsafe fn from_raw(raw: &kgds_stream_view) -> Result<StreamView<'a>, DecodeError> {
        // SAFETY: every section goes through `slice_from_raw`, which rejects
        // null, misaligned and over-long sections before forming a slice. The
        // remaining preconditions — that the memory is initialised, is not
        // mutated while borrowed, and outlives `'a` — are the caller's, and are
        // stated in this function's Safety section.
        //
        // The sections are read in one block rather than eight so that the
        // justification is written once and cannot drift from a call it is
        // meant to cover.
        let parts = unsafe {
            StreamParts {
                version: raw.version,
                flags: raw.flags,
                group_cmds: slice_from_raw(
                    raw.group_cmds,
                    raw.group_cmd_count,
                    Section::GroupCommands,
                )?,
                group_coords: slice_from_raw(
                    raw.group_coords,
                    raw.group_coord_count,
                    Section::GroupCoords,
                )?,
                groups: slice_from_raw(raw.groups, raw.group_count, Section::Groups)?,
                frame_cmds: slice_from_raw(
                    raw.frame_cmds,
                    raw.frame_cmd_count,
                    Section::FrameCommands,
                )?,
                frame_coords: slice_from_raw(
                    raw.frame_coords,
                    raw.frame_coord_count,
                    Section::FrameCoords,
                )?,
                strings: slice_from_raw(raw.strings, raw.string_bytes, Section::Strings)?,
                images: slice_from_raw(raw.images, raw.image_count, Section::Images)?,
                image_data: slice_from_raw(raw.image_data, raw.image_bytes, Section::ImageData)?,
            }
        };

        StreamView::new(parts)
    }

    /// ABI version this stream was recorded with. Always [`KGDS_VERSION`],
    /// since anything else is rejected.
    pub fn version(&self) -> u32 {
        self.version
    }

    /// Producer-defined stream flags.
    pub fn flags(&self) -> u32 {
        self.flags
    }

    /// The retained-geometry command array.
    pub fn group_cmds(&self) -> &'a [kgds_cmd] {
        self.group_cmds
    }

    /// The coordinate arena group bodies index into.
    pub fn group_coords(&self) -> &'a [f64] {
        self.group_coords
    }

    /// This frame's command array.
    pub fn frame_cmds(&self) -> &'a [kgds_cmd] {
        self.frame_cmds
    }

    /// The coordinate arena frame commands index into.
    pub fn frame_coords(&self) -> &'a [f64] {
        self.frame_coords
    }

    /// The string arena. No opcode in ABI version 1 references it; it is
    /// carried through so that a future one can.
    pub fn strings(&self) -> &'a [u8] {
        self.strings
    }

    /// The group table, in producer order.
    pub fn groups(&self) -> &'a [kgds_group] {
        self.groups
    }

    /// The image table.
    pub fn images(&self) -> &'a [kgds_image] {
        self.images
    }

    /// Iterate this frame's commands.
    pub fn frame(&self) -> CommandIter<'a> {
        CommandIter {
            cmds: self.frame_cmds,
            coords: self.frame_coords,
            image_count: self.images.len(),
            base: 0,
            pos: 0,
        }
    }

    /// Find a group by the id `DRAW_GROUP` refers to it by.
    pub fn group(&self, id: u32) -> Option<&'a kgds_group> {
        self.group_slot(id).map(|i| &self.groups[i])
    }

    /// Group table index for an id.
    pub fn group_slot(&self, id: u32) -> Option<usize> {
        self.group_index.resolve(id, self.groups.len())
    }

    /// Iterate a group's body, or `None` if no group has that id.
    pub fn group_body(&self, id: u32) -> Option<CommandIter<'a>> {
        let g = self.group(id)?;
        Some(self.group_body_at(g))
    }

    /// Iterate a group table entry's body.
    pub fn group_body_at(&self, g: &kgds_group) -> CommandIter<'a> {
        // Validation proved this range in bounds; clamping anyway means a
        // future caller with a hand-made `kgds_group` gets an empty iterator
        // rather than a panic.
        let first = (g.first_cmd as usize).min(self.group_cmds.len());
        let end = first
            .saturating_add(g.cmd_count as usize)
            .min(self.group_cmds.len());
        CommandIter {
            cmds: &self.group_cmds[first..end],
            coords: self.group_coords,
            image_count: self.images.len(),
            base: first,
            pos: 0,
        }
    }

    /// The pixels of an image table entry.
    pub fn image_pixels(&self, image: usize) -> Option<&'a [u8]> {
        let img = self.images.get(image)?;
        let start = usize::try_from(img.data_offset).ok()?;
        let end = start.checked_add(usize::try_from(img.data_length).ok()?)?;
        self.image_data.get(start..end)
    }

    /// Copy the view into an owned [`Stream`]. Revalidation is skipped, since
    /// the view could not exist unvalidated.
    pub fn to_owned_stream(&self) -> Stream {
        Stream {
            version: self.version,
            flags: self.flags,
            group_cmds: self.group_cmds.to_vec(),
            group_coords: self.group_coords.to_vec(),
            groups: self.groups.to_vec(),
            frame_cmds: self.frame_cmds.to_vec(),
            frame_coords: self.frame_coords.to_vec(),
            strings: self.strings.to_vec(),
            images: self.images.to_vec(),
            image_data: self.image_data.to_vec(),
            group_index: self.group_index.to_owned_index(),
        }
    }
}

/// An owned draw stream.
///
/// This is what [`crate::StreamBuilder`] produces and what [`Stream::read`]
/// loads from disk. Decoding goes through [`Stream::view`].
#[derive(Clone, Debug)]
pub struct Stream {
    pub(crate) version: u32,
    pub(crate) flags: u32,
    pub(crate) group_cmds: Vec<kgds_cmd>,
    pub(crate) group_coords: Vec<f64>,
    pub(crate) groups: Vec<kgds_group>,
    pub(crate) frame_cmds: Vec<kgds_cmd>,
    pub(crate) frame_coords: Vec<f64>,
    pub(crate) strings: Vec<u8>,
    pub(crate) images: Vec<kgds_image>,
    pub(crate) image_data: Vec<u8>,
    group_index: GroupIndex<'static>,
}

impl Stream {
    /// Validate and take ownership of a set of sections.
    pub fn from_parts(parts: StreamParts<'_>) -> Result<Stream, DecodeError> {
        Ok(StreamView::new(parts)?.to_owned_stream())
    }

    /// Borrow the stream for decoding.
    pub fn view(&self) -> StreamView<'_> {
        StreamView {
            version: self.version,
            flags: self.flags,
            group_cmds: &self.group_cmds,
            group_coords: &self.group_coords,
            groups: &self.groups,
            frame_cmds: &self.frame_cmds,
            frame_coords: &self.frame_coords,
            strings: &self.strings,
            images: &self.images,
            image_data: &self.image_data,
            group_index: self.group_index.borrowed(),
        }
    }

    /// ABI version.
    pub fn version(&self) -> u32 {
        self.version
    }

    /// Producer-defined stream flags.
    pub fn flags(&self) -> u32 {
        self.flags
    }

    /// The retained-geometry command array.
    pub fn group_cmds(&self) -> &[kgds_cmd] {
        &self.group_cmds
    }

    /// The retained-geometry coordinate arena.
    pub fn group_coords(&self) -> &[f64] {
        &self.group_coords
    }

    /// This frame's command array.
    pub fn frame_cmds(&self) -> &[kgds_cmd] {
        &self.frame_cmds
    }

    /// This frame's coordinate arena.
    pub fn frame_coords(&self) -> &[f64] {
        &self.frame_coords
    }

    /// The group table.
    pub fn groups(&self) -> &[kgds_group] {
        &self.groups
    }

    /// The image table.
    pub fn images(&self) -> &[kgds_image] {
        &self.images
    }

    /// Total number of commands, frame body and group bodies together.
    pub fn len(&self) -> usize {
        self.group_cmds.len() + self.frame_cmds.len()
    }

    /// True when the stream holds no commands at all.
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

impl PartialEq for Stream {
    fn eq(&self, other: &Self) -> bool {
        // Coordinates are compared by bit pattern rather than by value: a round
        // trip through the file format must be exact, and `-0.0 == 0.0` would
        // hide a sign being lost.
        fn bits_eq(a: &[f64], b: &[f64]) -> bool {
            a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.to_bits() == y.to_bits())
        }
        self.version == other.version
            && self.flags == other.flags
            && self.group_cmds == other.group_cmds
            && bits_eq(&self.group_coords, &other.group_coords)
            && self.groups == other.groups
            && self.frame_cmds == other.frame_cmds
            && bits_eq(&self.frame_coords, &other.frame_coords)
            && self.strings == other.strings
            && self.images == other.images
            && self.image_data == other.image_data
    }
}

/// Form a slice from an FFI pointer/length pair, rejecting the shapes that
/// would make `from_raw_parts` undefined behaviour.
///
/// # Safety
///
/// If this returns `Ok`, the caller must still have upheld that `ptr` points at
/// `count` initialised elements that are not mutated for `'a`.
unsafe fn slice_from_raw<'a, T>(
    ptr: *const T,
    count: usize,
    section: Section,
) -> Result<&'a [T], DecodeError> {
    if count == 0 {
        return Ok(&[]);
    }
    if ptr.is_null() {
        return Err(DecodeError::NullSection { section });
    }
    let align = align_of::<T>();
    if (ptr as usize) % align != 0 {
        return Err(DecodeError::MisalignedSection { section, align });
    }
    // `from_raw_parts` requires the total size to fit in isize.
    let bytes = (count as u128) * (size_of::<T>() as u128);
    if bytes > isize::MAX as u128 {
        return Err(DecodeError::SliceTooLarge { section, count });
    }
    // SAFETY: non-null, aligned, and under the size limit; the caller's
    // contract covers initialisation, aliasing and lifetime.
    Ok(unsafe { std::slice::from_raw_parts(ptr, count) })
}

/// The whole validation pass. Returns the group id lookup as a by-product,
/// since building it needs the same walk.
fn validate<'a>(parts: &StreamParts<'a>) -> Result<GroupIndex<'a>, DecodeError> {
    if parts.version != KGDS_VERSION {
        return Err(DecodeError::UnsupportedVersion {
            found: parts.version,
            expected: KGDS_VERSION,
        });
    }

    // -- coordinate arenas --------------------------------------------------
    // A NaN or infinity here would propagate into tessellation and into vertex
    // buffers, where it becomes unbounded geometry rather than a diagnosable
    // fault. One linear scan per arena rules it out for every command at once.
    for arena in [parts.group_coords, parts.frame_coords] {
        if let Some((i, v)) = arena.iter().enumerate().find(|(_, v)| !v.is_finite()) {
            return Err(DecodeError::NonFiniteCoord {
                coord: i,
                value: *v,
            });
        }
    }

    // -- image table --------------------------------------------------------
    for (i, img) in parts.images.iter().enumerate() {
        let format = ImageFormat::from_raw(img.format).ok_or(DecodeError::UnknownImageFormat {
            image: i,
            raw: img.format,
        })?;
        let end = img.data_offset.checked_add(img.data_length).ok_or(
            DecodeError::ImageDataOutOfRange {
                image: i,
                offset: img.data_offset,
                len: img.data_length,
                arena_len: parts.image_data.len(),
            },
        )?;
        if end > parts.image_data.len() as u64 {
            return Err(DecodeError::ImageDataOutOfRange {
                image: i,
                offset: img.data_offset,
                len: img.data_length,
                arena_len: parts.image_data.len(),
            });
        }
        let expected = (img.width as u64)
            .checked_mul(img.height as u64)
            .and_then(|px| px.checked_mul(format.bytes_per_pixel()))
            .ok_or(DecodeError::ImageSizeMismatch {
                image: i,
                declared: img.data_length,
                expected: u64::MAX,
            })?;
        if expected != img.data_length {
            return Err(DecodeError::ImageSizeMismatch {
                image: i,
                declared: img.data_length,
                expected,
            });
        }
    }

    // -- group table --------------------------------------------------------
    let group_count = parts.groups.len();
    let mut identity = true;
    for (i, g) in parts.groups.iter().enumerate() {
        let end = (g.first_cmd as u64) + (g.cmd_count as u64);
        if end > parts.group_cmds.len() as u64 {
            return Err(DecodeError::GroupOutOfRange {
                group: i,
                first: g.first_cmd,
                count: g.cmd_count,
                cmd_count: parts.group_cmds.len(),
            });
        }
        if g.id as usize != i {
            identity = false;
        }
    }

    let group_index = if identity {
        GroupIndex::Identity
    } else {
        let mut pairs: Vec<(u32, u32)> = parts
            .groups
            .iter()
            .enumerate()
            .map(|(i, g)| (g.id, i as u32))
            .collect();
        pairs.sort_unstable_by_key(|(id, _)| *id);
        if let Some(w) = pairs.windows(2).find(|w| w[0].0 == w[1].0) {
            return Err(DecodeError::DuplicateGroupId { id: w[0].0 });
        }
        GroupIndex::Sorted(Cow::Owned(pairs))
    };

    // -- every command, against its own arena -------------------------------
    // Decoding each command is exactly the range check the renderer would
    // otherwise have to repeat, so do it once here and let the iterator be
    // infallible afterwards.
    for (cmds, coords) in [
        (parts.group_cmds, parts.group_coords),
        (parts.frame_cmds, parts.frame_coords),
    ] {
        for (i, cmd) in cmds.iter().enumerate() {
            let decoded = decode_in(i, cmd, coords, parts.images.len())?;
            if let Command::DrawGroup { id, .. } = decoded {
                if group_index.resolve(id, group_count).is_none() {
                    return Err(DecodeError::UnknownGroupId { index: i, id });
                }
            }
        }
    }

    // -- group reference graph ----------------------------------------------
    check_group_graph(parts, &group_index)?;

    Ok(group_index)
}

/// Prove the `DRAW_GROUP` reference graph is acyclic and shallow.
///
/// Without this, a group whose body draws itself would make replay
/// non-terminating: the renderer would recurse until the stack ran out, on
/// input that came from another process.
fn check_group_graph(parts: &StreamParts<'_>, index: &GroupIndex<'_>) -> Result<(), DecodeError> {
    #[derive(Clone, Copy, PartialEq)]
    enum Mark {
        Unvisited,
        InProgress,
        Done,
    }

    let n = parts.groups.len();
    let mut marks = vec![Mark::Unvisited; n];
    // Iterative DFS, so that a deep graph cannot overflow the validator's own
    // stack while it is busy proving the renderer's cannot.
    let mut stack: Vec<(usize, usize, usize)> = Vec::new(); // (slot, cursor, depth)

    for root in 0..n {
        if marks[root] != Mark::Unvisited {
            continue;
        }
        marks[root] = Mark::InProgress;
        stack.push((root, 0, 1));

        while let Some((slot, cursor, depth)) = stack.pop() {
            let g = parts.groups[slot];
            let first = g.first_cmd as usize;
            let body = &parts.group_cmds[first..first + g.cmd_count as usize];

            let mut next = None;
            for (off, cmd) in body.iter().enumerate().skip(cursor) {
                if cmd.op != Op::DrawGroup as u16 {
                    continue;
                }
                let child_id = cmd.arg0;
                let Some(child) = index.resolve(child_id, n) else {
                    // Already rejected by the per-command pass.
                    continue;
                };
                match marks[child] {
                    Mark::InProgress => return Err(DecodeError::GroupCycle { id: child_id }),
                    Mark::Done => continue,
                    Mark::Unvisited => {
                        if depth + 1 > MAX_GROUP_DEPTH {
                            return Err(DecodeError::GroupTooDeep {
                                id: child_id,
                                depth: depth + 1,
                            });
                        }
                        next = Some((off + 1, child));
                        break;
                    }
                }
            }

            match next {
                Some((resume, child)) => {
                    stack.push((slot, resume, depth));
                    marks[child] = Mark::InProgress;
                    stack.push((child, 0, depth + 1));
                }
                None => marks[slot] = Mark::Done,
            }
        }
    }

    Ok(())
}
