# Vulkan pipeline build ownership

A compute build owns its progress notification from queue submission until the build returns or
the queued task is destroyed. The GPU keeps its shader notifier alive through renderer teardown.

Graphics and compute driver builds can stay in the worker queue after the scheduler has built
the pipeline for its first draw or dispatch. The queue and pipeline each hold the build state.
The thread that claims it runs the task and releases its captures before publishing completion.
The queue can then process an already completed build even after the cache has evicted the
pipeline. Vulkan devices with a parallel compilation workaround keep builds on their worker.

Boot-time shader translation still runs on the existing loader workers. Scheduler takeover
applies only to driver builds whose shader modules and descriptor layout already exist.

## Task and mesh declarations

Graphics translation checks the declared task and mesh resources against
Vulkan device properties before emitting SPIR-V or creating shader modules.
An over-limit declaration skips the draw. The pipeline cache warns once per
stage and shader key, which includes the mesh register and header state.

The task emitter declares a payload and no workgroup storage. The mesh emitter
declares the payload even without a task stage, along with workgroup arrays for
attributes and the map, and a primitive-count word. Each map entry occupies
one 32-bit word. Mesh output memory counts each distinct emitted variable at
its declared width. Vulkan pads the vertex and primitive counts independently
and excludes triangle indices from the storage calculation.
[Vulkan SPIR-V limits](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html#VUID-RuntimeSpirv-maxMeshSharedMemorySize-08754)

Sports Resort task shader `9616b762fe93bee8` declares 27,904 bytes from the
`218 × 32` words in `translate_program.cpp`. The reason the guest registers
request that many lines remains open. Mesh shader `6f7a7cdcacbeb979` declares
another 6,908 bytes of workgroup storage, for a combined 34,812 bytes.
