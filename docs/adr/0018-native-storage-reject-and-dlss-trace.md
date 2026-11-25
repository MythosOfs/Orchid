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
(declared / bare / cap), the logical range, and the actual host span — reported from inside
`BindHostComputeStorageBuffers`, at the branch that really bound: `path=multi` with the summed
segment spans when `BindMultiRangeStorage` succeeded, `path=single` with the fallback
buffer's offset and span when the runtime refused the multi-range bind, `path=null` when
nothing bound. Reporting the planned segments from the planning pass would claim a bind that
can still fall back. The trace object itself is a `std::optional` allocated on first opt-in,
per-SSBO diagnostic records exist only while a sequence is actively capturing, and the host
report itself is gated by `DlssTraceCapturing` so the disabled path does no trace work.

`overlap_pages_scanned` is test-only instrumentation: production `BufferCache` carries no
op-count state; `ResolveOverlaps` increments it only when the runtime type exposes the member
(a `requires` hook), so `FixtureRuntime` counts walked slots while the Vulkan and GL runtimes
pay nothing. The counts are op counts, not timings — no performance claim is made from them.

The GL pipeline trace emits a `begin runtime=R block_size=` marker on first Configure per
runtime (the independent GL_MAX_SHADER_STORAGE_BLOCK_SIZE), per-Configure lines carrying a
global serial and requested-vs-actual mode before SSBO resolution, and an
`end runtime=R configures=N` marker at static teardown. The checker requires the whole
envelope — missing begin/end proves truncation, a repeated begin proves replay, serials must
span 0..N-1 gapless, and the end count must equal the configure lines present.

## Consequences

- Graphics resolution on a native channel can no longer throw through the reject path; a rejected
  compute merge leaves every pre-existing buffer usable (unpicked, stale entries restored).
- The second sequence distinguishes cold/warm program captures; a reject or keyless launch is no
  longer logged as plain success.
- No live GL run yet: the checker is proven on pinned positive/negative logs through
  run-ssbo-checker-cases.sh; the orchid MCP transport is agent-session-only, so the launch
  and negative-control stay an agent-side recipe in the gate plan.
