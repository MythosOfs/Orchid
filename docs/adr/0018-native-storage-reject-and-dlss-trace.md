# 0018. Native storage reject policy rides the binding, and the DLSS trace is lazy

Status: accepted, 2026-11-22.

## Context

Native compute storage buffers can describe spans the host buffer cache cannot represent (a
u32-indexed host range, or an overlap merge that grows past it). The first reject check lived in
`CreateBuffer` behind `kepler_compute->IsNativeMode()`, a channel-wide flag that cannot tell a
graphics allocation on a native channel from a compute one. The reject also ran a probe
`ResolveOverlaps` pass ahead of the real scan, doubling the page-table walk per miss. Separately,
the DLSS dispatch trace held only a bounded window and emitted once, so a longer valid chain
dropped its prepass and a second sequence could never be captured.

## Decision

The reject policy is data, not ambient state: `Binding` and `MultiRangeSegment` carry
`native_compute`, `ResolveStorageBufferBinding` fills it from the caller's flag, and every
`FindBuffer` on the compute storage path (initial resolve, post-delete re-resolve, multi-range
segment resolve and refresh) passes it through. `CreateBuffer` sees only the explicit
`native_reject` argument; every graphics caller passes false and keeps the silent wrap.
`ResolveOverlaps` now scans once, always picking overlaps and clearing stale page-table entries,
and a rejected merge undoes exactly that recorded state (unpick the picked ids, restore the stale
entries) instead of probing first. `overlap_pages_scanned` counts the walked slots as op-count
evidence that one miss is one scan.

The DLSS trace retains up to two complete sequences per process, each from a matching prepass key
through the output key and the next ordinary dispatch, flagged `truncated` past the chain bound.
Entries record the launch outcome (succeeded / rejected / keyless), the per-SSBO size rule
(declared / bare / cap), the logical range, and the actual host span filled in by the storage
update pass — the summed multi-range segments when the binding split, not the fallback prefix.
The trace object itself is a `std::optional` allocated on first opt-in, and per-SSBO diagnostic
records exist only while a sequence is actively capturing.

The GL pipeline trace line logs the actual runtime mode and effective limit before SSBO
resolution under `EDEN_NX2_TRACE_GL_SSBO`, plus a once-per-runtime independent
GL_MAX_SHADER_STORAGE_BLOCK_SIZE for the checker to hold SSBO-mode ranges against.

## Consequences

- Graphics resolution on a native channel can no longer throw through the reject path; a rejected
  compute merge leaves every pre-existing buffer usable (unpicked, stale entries restored).
- The second sequence distinguishes cold/warm program captures; a reject or keyless launch is no
  longer logged as plain success.
- No live GL run yet: the checker is proven on synthetic matching, stale and single-mode logs;
  the same-runtime mixed-workload recipe stays prepared-unrun in the gate plan.
