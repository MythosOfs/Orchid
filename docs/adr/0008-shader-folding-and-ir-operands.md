# 0008. Shader folding and IR operands

## Unsigned bitfield extracts

`BitFieldUExtract` with a count of zero yields zero, even at offset 32. The only valid
32-bit extract starts at offset zero and keeps every bit. The constant folder handles these
edges before evaluating a host shift, since shifting a 32-bit operand by 32 is undefined in C++.
