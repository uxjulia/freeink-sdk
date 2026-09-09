#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
namespace freeink::font::detail {
// Borrowed, bounds-checked OpenType view. Only liga/rlig LigatureSubst and
// ExtensionSubst are needed by the existing codepoint-based shaping contract.
struct View {
  const uint8_t* data = nullptr;
  size_t size = 0;
  bool has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
  unsigned u16(size_t offset) const { return has(offset, 2) ? unsigned(data[offset]) * 256 + data[offset + 1] : 0; }
  uint32_t u32(size_t offset) const { return has(offset, 4) ? (uint32_t(u16(offset)) << 16) | u16(offset + 2) : 0; }
  View sub(size_t offset) const { return has(offset, 1) ? View{data + offset, size - offset} : View{}; }
};
inline View table(const uint8_t* data, size_t bytes, const char* tag) {
  View file{data, bytes};
  const unsigned count = file.u16(4);
  if (!file.has(12, size_t(count) * 16)) return {};
  for (unsigned i = 0; i < count; ++i) {
    const size_t pos = 12 + i * 16;
    if (std::memcmp(data + pos, tag, 4)) continue;
    const auto offset = file.u32(pos + 8), len = file.u32(pos + 12);
    return file.has(offset, len) ? View{data + offset, len} : View{};
  }
  return {};
}
inline int coverage(View view, unsigned glyph) {
  const unsigned format = view.u16(0), count = view.u16(2);
  if (count > 4096) return -1;
  if (format == 1 && view.has(4, size_t(count) * 2)) {
    unsigned lo = 0, hi = count;
    while (lo < hi) {
      unsigned m = (lo + hi) / 2;
      if (view.u16(4 + m * 2) < glyph)
        lo = m + 1;
      else
        hi = m;
    }
    return lo < count && view.u16(4 + lo * 2) == glyph ? int(lo) : -1;
  }
  if (format == 2 && view.has(4, size_t(count) * 6)) {
    for (unsigned i = 0; i < count; ++i) {
      size_t p = 4 + i * 6;
      unsigned start = view.u16(p), end = view.u16(p + 2);
      if (glyph >= start && glyph <= end) return int(view.u16(p + 4) + glyph - start);
    }
  }
  return -1;
}
inline unsigned ligatureGlyph(View gsub, const unsigned* sequence, unsigned length) {
  if (!gsub.has(0, 10) || length < 2 || length > 3) return 0;
  View features = gsub.sub(gsub.u16(6)), lookups = gsub.sub(gsub.u16(8));
  const unsigned featureCount = features.u16(0), lookupCount = lookups.u16(0);
  if (featureCount > 128 || lookupCount > 1024 || !features.has(2, size_t(featureCount) * 6) ||
      !lookups.has(2, size_t(lookupCount) * 2))
    return 0;
  unsigned work = 0;
  for (unsigned f = 0; f < featureCount; ++f) {
    const size_t record = 2 + f * 6;
    if (std::memcmp(features.data + record, "liga", 4) && std::memcmp(features.data + record, "rlig", 4)) continue;
    View feature = features.sub(features.u16(record + 4));
    unsigned count = feature.u16(2);
    if (count > 128 || !feature.has(4, size_t(count) * 2)) continue;
    for (unsigned i = 0; i < count; ++i) {
      unsigned index = feature.u16(4 + i * 2);
      if (index >= lookupCount) continue;
      View lookup = lookups.sub(lookups.u16(2 + index * 2));
      const unsigned type = lookup.u16(0), subCount = lookup.u16(4);
      if ((type != 4 && type != 7) || subCount > 128 || !lookup.has(6, size_t(subCount) * 2)) continue;
      for (unsigned j = 0; j < subCount; ++j) {
        if (++work > 512) return 0;
        View sub = lookup.sub(lookup.u16(6 + j * 2));
        if (type == 7) {
          if (sub.u16(0) != 1 || sub.u16(2) != 4) continue;
          sub = sub.sub(sub.u32(4));
        }
        if (sub.u16(0) != 1) continue;
        const int cov = coverage(sub.sub(sub.u16(2)), sequence[0]);
        const unsigned sets = sub.u16(4);
        if (cov < 0 || unsigned(cov) >= sets || !sub.has(6, size_t(sets) * 2)) continue;
        View set = sub.sub(sub.u16(6 + unsigned(cov) * 2));
        const unsigned n = set.u16(0);
        if (n > 512 || !set.has(2, size_t(n) * 2)) continue;
        for (unsigned k = 0; k < n; ++k) {
          View lig = set.sub(set.u16(2 + k * 2));
          if (lig.u16(2) != length || !lig.has(4, (length - 1) * 2)) continue;
          bool match = true;
          for (unsigned c = 1; c < length; ++c) match = match && lig.u16(4 + (c - 1) * 2) == sequence[c];
          if (match) return lig.u16(0);
        }
      }
    }
  }
  return 0;
}
}  // namespace freeink::font::detail
