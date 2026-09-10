# FreeInkFont

A static TrueType font service independent of FreeInkBook and storage. The caller
supplies aligned workspace and pixel arenas, retains the font bytes until close,
and serializes all calls. The service never allocates from the system heap:
FreeType uses a coalescing allocator in the supplied workspace. Its optional
standard initialization/filesystem functions are compiled upstream code, but are
not used by this service.

`metrics()` returns 1/16-pixel advances and integer bitmap bounds without
rasterizing. Sizes are points at 150 DPI. `rasterize()` returns packed row-contiguous
2-bit coverage (zero is white); its result must be consumed before the next
rasterization or any face close. `darken` selects the darker e-ink thresholds and
is part of the cache key. Glyph image eviction resets only the image cache;
metric entries remain available. Face close invalidates both caches.

There are 16 face slots, 1,024 metric slots, 512 kerning slots, and 512 image lookup slots. Image
storage is a bounded append arena, recycled on exhaustion. Cached entries are
indexed by face, codepoint, size, and coverage mode. No additional framebuffer is
allocated. Caller examples use 768 KiB workspace and 512 KiB glyph storage; these
are budgets, not minimum requirements for every font. The grayscale rasterizer's
16 KiB cell pool belongs to its raster object and is allocated once through the
same fallible workspace allocator; it never occupies the caller's task stack.
Separate Service instances own separate pools. Initialization fails cleanly if
any required FreeType module cannot be installed within the workspace.

`open()` returns a nonnegative face ID, -1 for unsupported/malformed input, and
-2 when FreeType cannot allocate. TTC, CFF/OTF, and variable TTF files are rejected.
Fonts with family names longer than 63 bytes are rejected to avoid ambiguous
truncated identities. Standard Unicode Latin ligatures are resolved from
`liga`/`rlig` GSUB rules, including unencoded glyphs. This is not a general shaping
engine; callers retain their bidi/Arabic/combining-mark handling. GPOS pair
kerning is enabled; advanced positioning needs a shaping engine.

FreeType 2.14.3 sources in `third_party/freetype` are from official tag
`VER-2-14-3`, commit `0a0221a1347e2f1e07c395263540026e9a0aa7c7` at
https://github.com/freetype/freetype. Only include/, base/, truetype/, sfnt/, and
smooth/ are vendored. The local `ftgrays.c` patch moves the unchanged-size cell
pool from `gray_convert_glyph`'s stack to the FT_Memory-owned raster object,
with a borrowed worker pointer. Preserve this patch when updating FreeType.
Configuration lives in include/.
The FreeType License (FTL.TXT) applies; retain its attribution in distributions.
This software uses the FreeType library, copyright The FreeType Project.
