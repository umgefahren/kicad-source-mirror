# Golden draw streams

Serialised `DRAW_STREAM`s recorded from schematics already in this repository,
in the `kgds_file_header` format defined by
`include/gal/recording/draw_stream_abi.h`.

## Why these are checked in

They are derived data, and normally derived data does not belong in a source
tree. These earn their place because they decouple two halves of the migration
that would otherwise have to be built together:

* the Rust renderer and decoder (`rust/crates/kicad-gal`) can be developed and
  regression-tested against real schematic output **without linking any C++**,
  and therefore without a multi-hour eeschema build;
* a change in KiCad's drawing rules shows up as a diff in a specific opcode on a
  specific fixture, rather than as "the render looks wrong".

## What is in each file

One recorded frame plus the retained group geometry it refers to. The stream has
two arenas: group bodies, recorded once per cached item by
`KIGFX::VIEW::updateItemGeometry`, and the frame's own command list, which
mostly consists of `KGDS_OP_DRAW_GROUP` references into them. Coordinates are
`double`s in KiCad internal units (100 nm for eeschema), in world space.

Each `.kgds` has a `.txt` beside it with the statistics `kicad-sch-dump`
reported when it was recorded. Those reports carry the recording machine's paths
and timings, so they are not reproducible byte for byte and are documentation
rather than fixtures. One line in them is now stale on purpose: the
`viewport ... scale` they print is `KIGFX::VIEW`'s zoom factor, because
`SCH_HOST` used to report that where the ABI promised pixels per internal unit.
That was fixed for the live re-render work, and the fix changes nothing else —
every count and every byte in all four `.kgds` is unchanged, because a
zoom-to-fit frames the page and nothing on these sheets sits outside it.

## Regenerating

```sh
ninja -C <build> kicad_sch_dump
qa/data/draw_streams/generate.sh <build>/qa/tools/sch_dump/kicad-sch-dump
```

The viewport is pinned to 1920x1080 in that script. The recorded geometry is in
world coordinates and does not depend on it, but `KGDS_OP_BEGIN_FRAME` and the
grid command do, so changing it changes the bytes.

**Regenerate on the platform the files were recorded on** — Linux, for the ones
here — or expect a diff that is not a change in KiCad. Group ids are assigned in
the order `KIGFX::VIEW` visits items, and items with equal sort keys come out in
whatever order an unstable sort left them, which libstdc++ and libc++ decide
differently. On macOS these four files come back with the same group table, the
same commands and the same coordinates, with a handful of bodies swapped between
ids. Nothing renders differently; the bytes are simply not canonical. See
`docs/rust-migration/04-host-seam.md` §8.

## Keeping them small

These are fixtures, not a corpus. Add a file only when it covers something the
existing ones do not, and prefer the smallest schematic that does so. Anything
that would land as a multi-megabyte blob belongs in a generated corpus run, not
here — `kicad-sch-dump` can record any of the 466 `.kicad_sch` files in the tree
on demand.
