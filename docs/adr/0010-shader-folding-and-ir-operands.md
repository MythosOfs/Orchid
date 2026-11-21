# 0010. Shader folding and IR operands

## Unsigned bitfield extracts

`BitFieldUExtract` with a count of zero yields zero, even at offset 32. The only valid
32-bit extract starts at offset zero and keeps every bit. The constant folder handles these
edges before evaluating a host shift, since shifting a 32-bit operand by 32 is undefined in C++.

## Constant buffer handles and phi operands

The two lanes of `GetCbufU32x2` read adjacent little-endian 32-bit words. Static texture
tracking applies the lane's byte offset before calculating the descriptor range. Previously the
static search walked past the extract and bound the first word for either lane. The per-draw
handle evaluator already applies this offset. Both OpenGL and Vulkan pipeline caches serialize
the constant buffer words that translation read, so they need fresh captures when translation
starts reading the second word.

An identity wrapping an immediate does not count as an instruction use. Resolve it when
storing a phi operand so dead-code elimination can remove the identity without invalidating
the operand. SPIR-V emission rejects a deferred phi operand whose definition is missing.
