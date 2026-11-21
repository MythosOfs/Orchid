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
