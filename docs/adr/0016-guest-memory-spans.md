# 0009 Guest memory spans

`Memory::GetSpan` exposes a guest range as a single host pointer. Each
covered page must have the same host-minus-guest offset. A mapping ID
can survive at both ends when a page in the middle is remapped. The
last covered page is the one containing the last byte of the range.
Debug and rasterizer markers leave the backing offset intact.
