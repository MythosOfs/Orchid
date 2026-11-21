# 0010. CMIF output buffer contents

CMIF copies back the full temporary output span. A handler such as `IFile::Read` can write
only a prefix of that span and return the number of bytes it wrote. Prefill the temporary span
from the guest destination so a short read leaves the guest tail intact. Select the descriptor
using the output argument's mode. Auto-select uses B when the matching mapped descriptor has
a nonzero length and otherwise uses C.

Read through the coherent CPU memory path so the preserved bytes include any GPU writes already
visible to the caller. This preserves guest contents within each output span. Distinct outputs
continue to be serialized in their declared order.
