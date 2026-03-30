PPB: PicoPB ? PseudoPB?
=======================

PPB is a zero-copy lexer for binary protobuf.  No message object or
mutable DOM, just a stream of type-value pairs, with optional prescan
to drive preallocation.

For simplicity, the interface assumes all the serialized data is
available in a contiguous span of read-only memory.
