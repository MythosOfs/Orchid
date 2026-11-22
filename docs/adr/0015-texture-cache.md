# 0015 Texture cache

## Host scratch allocation

Destructive scratch growth releases the old allocation first. Capacity and size
stay zero if the allocation throws, so the next request can try again.

Block-linear write-back reserves scratch for the largest split mip layer before
changing guest bytes. Contiguous layers write directly. If the collector runs
out of scratch, it keeps the image registered with its GPU contents. A failed
full upload restores the CPU-dirty flag and releases its page tracking so a
later use can upload it again.

## Address spaces

An image records the GPU address space it was registered in. Unregistering
clears that space's page tables, whichever channel is bound. Downloads are
written back through the memory manager of that space: nvdrv owns the manager
through a shared pointer, the cache keeps a weak one, and a write-back holds the
manager while it runs. When the space is closed, another live space that maps
the image's GPU address to the same device address takes the write. Sparse
images are dropped instead, they are not shared between spaces.

## Clears and padded targets

A clear covers an image only inside the render area (the surface clip) and the
flipped scissor of a lower-left window origin. ClearCovers decides this for the
join-time check and for IsFullClear alike.

A Switch 2 render target is padded to whole GOBs and sampled at the width of the
surface clip. Targets and blit surfaces wider than the clip's right edge by
padding only are looked up at the clip's width, so target and texture are one
image. The clip has no dirty flag of its own, so targets are looked up again
whenever its right edge moves.
