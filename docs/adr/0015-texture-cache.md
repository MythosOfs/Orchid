# 0008 Texture cache

## Host scratch allocation

Destructive scratch growth releases the old allocation first. Capacity and size
stay zero if the allocation throws, so the next request can try again.

Block-linear write-back reserves scratch for the largest split mip layer before
changing guest bytes. Contiguous layers write directly. If the collector runs
out of scratch, it keeps the image registered with its GPU contents. A failed
full upload restores the CPU-dirty flag and releases its page tracking so a
later use can upload it again.
