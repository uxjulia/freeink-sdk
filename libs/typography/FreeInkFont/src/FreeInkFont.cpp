#include "FreeInkFont.h"

#include <ft2build.h>

#include "Gsub.h"
#include FT_FREETYPE_H
#include FT_MODULE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace freeink::font {
namespace {
constexpr size_t Align = alignof(std::max_align_t);
size_t aligned(size_t n) { return (n + Align - 1) & ~(Align - 1); }
constexpr unsigned MetricSlots = 1024, PixelSlots = 512;
uint64_t keyFor(int face, uint32_t cp, uint8_t points) {
  return (uint64_t(face + 1) << 40) | (uint64_t(points) << 32) | cp;
}
}  // namespace
struct alignas(std::max_align_t) Service::Block {
  size_t bytes;
  Block* next;
  bool free;
};
struct Service::State {
  FT_MemoryRec_ memory{};
  FT_Library library = nullptr;
  Block* blocks = nullptr;
  FT_Face faces[MaxFaces]{};
  uint8_t sizes[MaxFaces]{};
  unsigned standardLigatures[MaxFaces][7]{};
  struct MetricEntry {
    uint64_t key = 0;
    Metrics value{};
  } metrics[MetricSlots];
  struct KernEntry {
    uint32_t left = 0, right = 0;
    int16_t value = 0;
    uint8_t face = 0, points = 0;
  } kern[512];
  struct PixelEntry {
    uint64_t key = 0;
    size_t offset = 0;
  } bitmaps[PixelSlots];
};
// FreeType owns allocations obtained through these callbacks. They come from
// one caller-supplied arena; free coalesces blocks instead of fragmenting the
// device heap. Even malformed-font scratch work cannot exceed the arena.
void* Service::allocate(void* m, long n) {
  if (n <= 0 || size_t(n) > SIZE_MAX - Align) return nullptr;
  auto* state = static_cast<State*>(static_cast<FT_Memory>(m)->user);
  const size_t bytes = aligned(size_t(n));
  for (Block* b = state->blocks; b; b = b->next) {
    if (!b->free || b->bytes < bytes) continue;
    if (b->bytes >= bytes + sizeof(Block) + Align) {
      auto* tail =
          new (reinterpret_cast<uint8_t*>(b + 1) + bytes) Block{b->bytes - bytes - sizeof(Block), b->next, true};
      b->next = tail;
      b->bytes = bytes;
    }
    b->free = false;
    return b + 1;
  }
  return nullptr;
}
void Service::deallocate(void* m, void* ptr) {
  if (!ptr) return;
  auto* state = static_cast<State*>(static_cast<FT_Memory>(m)->user);
  (static_cast<Block*>(ptr) - 1)->free = true;
  for (Block* b = state->blocks; b && b->next;) {
    if (b->free && b->next->free) {
      b->bytes += sizeof(Block) + b->next->bytes;
      b->next = b->next->next;
    } else
      b = b->next;
  }
}
void* Service::reallocate(void* m, long oldSize, long newSize, void* ptr) {
  if (newSize <= 0) {
    deallocate(m, ptr);
    return nullptr;
  }
  void* result = allocate(m, newSize);
  if (!result) return nullptr;
  if (ptr && oldSize > 0) std::memcpy(result, ptr, size_t(std::min(oldSize, newSize)));
  deallocate(m, ptr);
  return result;
}
Service::~Service() { end(); }
bool Service::begin(void* workspace, size_t bytes, void* pixels, size_t pixelBytes) {
  end();
  const size_t stateBytes = aligned(sizeof(State));
  if (!workspace || !pixels || reinterpret_cast<uintptr_t>(workspace) % Align ||
      bytes < stateBytes + sizeof(Block) + 65536 || pixelBytes < 16384)
    return false;
  state_ = new (workspace) State{};
  state_->blocks =
      new (static_cast<uint8_t*>(workspace) + stateBytes) Block{bytes - stateBytes - sizeof(Block), nullptr, true};
  state_->memory.user = state_;
  state_->memory.alloc = [](FT_Memory m, long n) { return allocate(m, n); };
  state_->memory.free = [](FT_Memory m, void* p) { deallocate(m, p); };
  state_->memory.realloc = [](FT_Memory m, long a, long b, void* p) { return reallocate(m, a, b, p); };
  if (FT_New_Library(&state_->memory, &state_->library)) {
    state_ = nullptr;
    return false;
  }
  FT_Add_Default_Modules(state_->library);
  // Module installation can fail silently, including the raster scratch pool.
  // Keep initialization fallible instead of accepting a partially usable engine.
  if (!FT_Get_Module(state_->library, "truetype") || !FT_Get_Module(state_->library, "sfnt") ||
      !FT_Get_Module(state_->library, "smooth")) {
    end();
    return false;
  }
  pixels_ = static_cast<uint8_t*>(pixels);
  pixelBytes_ = pixelBytes;
  pixelUsed_ = 0;
  rasterizations_ = 0;
  return true;
}
void Service::end() {
  if (state_) {
    for (unsigned i = 0; i < MaxFaces; ++i) close(i);
    FT_Done_Library(state_->library);
    state_->~State();
  }
  state_ = nullptr;
  pixels_ = nullptr;
  pixelUsed_ = pixelBytes_ = 0;
}
int Service::open(const uint8_t* bytes, size_t len, FaceInfo* info) {
  if (!state_ || !bytes || len < 12 || len > size_t(std::numeric_limits<FT_Long>::max())) return -1;
  // Static glyf-based TTF only. Explicitly reject TTC, CFF and variable fonts.
  if (bytes[0] != 0 || bytes[1] != 1 || bytes[2] != 0 || bytes[3] != 0) return -1;
  for (unsigned i = 0; i < MaxFaces; ++i) {
    if (state_->faces[i]) continue;
    FT_Face face = nullptr;
    const auto error = FT_New_Memory_Face(state_->library, bytes, FT_Long(len), 0, &face);
    if (error) return error == FT_Err_Out_Of_Memory ? -2 : -1;
    if (!FT_IS_SCALABLE(face) || FT_HAS_MULTIPLE_MASTERS(face) || FT_Select_Charmap(face, FT_ENCODING_UNICODE)) {
      FT_Done_Face(face);
      return -1;
    }
    // Reject fvar even when variation support is compiled out.
    const unsigned tables = unsigned(bytes[4]) * 256 + bytes[5];
    if (tables > (len - 12) / 16) {
      FT_Done_Face(face);
      return -1;
    }
    for (unsigned j = 0; j < tables; ++j) {
      if (std::memcmp(bytes + 12 + j * 16, "fvar", 4) == 0) {
        FT_Done_Face(face);
        return -1;
      }
    }
    if (!face->family_name || std::strlen(face->family_name) >= sizeof(FaceInfo::family)) {
      FT_Done_Face(face);
      return -1;
    }
    state_->faces[i] = face;
    state_->sizes[i] = 0;
    const auto gsub = detail::table(bytes, len, "GSUB");
    static constexpr uint32_t sequences[7][3] = {{'f', 'f', 0},   {'f', 'i', 0},   {'f', 'l', 0}, {'f', 'f', 'i'},
                                                 {'f', 'f', 'l'}, {0x17f, 't', 0}, {'s', 't', 0}};
    for (unsigned l = 0; l < 7; ++l) {
      unsigned glyphs[3] = {FT_Get_Char_Index(face, sequences[l][0]), FT_Get_Char_Index(face, sequences[l][1]),
                            FT_Get_Char_Index(face, sequences[l][2])};
      state_->standardLigatures[i][l] = detail::ligatureGlyph(gsub, glyphs, sequences[l][2] ? 3 : 2);
    }
    if (info) {
      *info = FaceInfo{};
      if (face->family_name) {
        std::strncpy(info->family, face->family_name, sizeof(info->family) - 1);
      }
      const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
      info->style = (((face->style_flags & FT_STYLE_FLAG_BOLD) || (os2 && os2->usWeightClass >= 600)) ? 1 : 0) |
                    ((face->style_flags & FT_STYLE_FLAG_ITALIC) ? 2 : 0);
    }
    return int(i);
  }
  return -1;
}
void Service::close(int face) {
  if (!state_ || face < 0 || face >= int(MaxFaces) || !state_->faces[face]) return;
  FT_Done_Face(state_->faces[face]);
  state_->faces[face] = nullptr;
  for (auto& m : state_->metrics) m.key = 0;
  for (auto& k : state_->kern) k.points = 0;
  clearPixels();
}
bool Service::select(int face, uint8_t points) {
  if (!state_ || face < 0 || face >= int(MaxFaces) || !state_->faces[face] || points < 1 || points > 48) return false;
  if (state_->sizes[face] == points) return true;
  if (FT_Set_Char_Size(state_->faces[face], points * 64, points * 64, 150, 150)) return false;
  state_->sizes[face] = points;
  return true;
}
unsigned Service::glyphIndex(int face, uint32_t cp) {
  if (cp >= 0xfb00 && cp <= 0xfb06 && state_->standardLigatures[face][cp - 0xfb00])
    return state_->standardLigatures[face][cp - 0xfb00];
  return FT_Get_Char_Index(state_->faces[face], cp);
}
bool Service::covers(int face, uint32_t cp) {
  return state_ && face >= 0 && face < int(MaxFaces) && state_->faces[face] && glyphIndex(face, cp) != 0;
}
bool Service::metrics(int face, uint32_t cp, uint8_t points, Metrics& out) {
  if (!select(face, points)) return false;
  const auto key = keyFor(face, cp, points);
  auto& entry = state_->metrics[(cp * 31u + points * 7u + unsigned(face)) % MetricSlots];
  if (entry.key == key) {
    out = entry.value;
    return true;
  }
  const FT_UInt glyph = glyphIndex(face, cp);
  if (!glyph || FT_Load_Glyph(state_->faces[face], glyph, FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING)) return false;
  const auto slot = state_->faces[face]->glyph;
  FT_BBox box;
  FT_Outline_Get_CBox(&slot->outline, &box);
  const long left = box.xMin >> 6, right = (box.xMax + 63) >> 6;
  const long bottom = box.yMin >> 6, top = (box.yMax + 63) >> 6;
  if (right < left || top < bottom || right - left > 255 || top - bottom > 255 || left < INT16_MIN ||
      left > INT16_MAX || top < INT16_MIN || top > INT16_MAX)
    return false;
  out = {uint16_t(std::clamp<long>((slot->linearHoriAdvance + 2048) >> 12, 0, 65535)), int16_t(left), int16_t(top),
         uint8_t(right - left), uint8_t(top - bottom)};
  entry.key = key;
  entry.value = out;
  return true;
}
int16_t Service::kerning16(int face, uint32_t left, uint32_t right, uint8_t points) {
  if (!select(face, points)) return 0;
  auto& cached = state_->kern[(left * 31u + right * 7u + unsigned(face) * 13u + points) % 512];
  if (cached.points == points && cached.face == face && cached.left == left && cached.right == right)
    return cached.value;
  auto f = state_->faces[face];
  FT_Vector v{};
  if (FT_Get_Kerning(f, glyphIndex(face, left), glyphIndex(face, right), FT_KERNING_UNFITTED, &v)) return 0;
  const auto value = int16_t(std::clamp<long>(v.x >= 0 ? (v.x + 2) / 4 : -((-v.x + 2) / 4), INT16_MIN, INT16_MAX));
  cached = {left, right, value, uint8_t(face), points};
  return value;
}

uint32_t Service::ligature(int face, uint32_t a, uint32_t b) {
  uint32_t cp = 0;
  if (a == 'f') {
    if (b == 'f')
      cp = 0xfb00;
    else if (b == 'i')
      cp = 0xfb01;
    else if (b == 'l')
      cp = 0xfb02;
  } else if (a == 0xfb00) {
    if (b == 'i')
      cp = 0xfb03;
    else if (b == 'l')
      cp = 0xfb04;
  }
  if (!state_ || face < 0 || face >= int(MaxFaces) || !state_->faces[face]) return 0;
  return cp && state_->standardLigatures[face][cp - 0xfb00] ? cp : 0;
}
bool Service::lineMetrics(int face, uint8_t points, int& asc, int& desc, int& height) {
  if (!select(face, points)) return false;
  const auto& m = state_->faces[face]->size->metrics;
  asc = int((m.ascender + 63) >> 6);
  desc = int(m.descender >> 6);
  height = int((m.height + 63) >> 6);
  return true;
}
void Service::clearPixels() {
  pixelUsed_ = 0;
  if (state_)
    for (auto& p : state_->bitmaps) p.key = 0;
}
const uint8_t* Service::rasterize(int face, uint32_t cp, uint8_t points, bool darken) {
  if (!select(face, points)) return nullptr;
  const auto key = keyFor(face, cp, points) | (uint64_t(darken) << 63);
  auto& entry = state_->bitmaps[(cp * 31u + points * 7u + unsigned(face)) % PixelSlots];
  if (entry.key == key) return pixels_ + entry.offset;
  Metrics m;
  if (!metrics(face, cp, points, m)) return nullptr;
  const size_t bytes = (size_t(m.width) * m.height + 3) / 4;
  if (!bytes || bytes > pixelBytes_) return nullptr;
  auto f = state_->faces[face];
  if (FT_Load_Glyph(f, glyphIndex(face, cp), FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING) ||
      FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL))
    return nullptr;
  const auto& b = f->glyph->bitmap;
  if (b.width != m.width || b.rows != m.height || b.pixel_mode != FT_PIXEL_MODE_GRAY || b.pitch < int(b.width))
    return nullptr;
  if (pixelUsed_ + bytes > pixelBytes_) clearPixels();
  auto* dest = pixels_ + pixelUsed_;
  std::memset(dest, 0, bytes);
  for (unsigned y = 0; y < b.rows; ++y)
    for (unsigned x = 0; x < b.width; ++x) {
      const unsigned i = y * b.width + x;
      const unsigned value = b.buffer[y * b.pitch + x] >> 4;
      const uint8_t coverage = darken ? (value < 3    ? 0
                                         : value < 6  ? 1
                                         : value < 10 ? 2
                                                      : 3)
                                      : (value < 4    ? 0
                                         : value < 8  ? 1
                                         : value < 12 ? 2
                                                      : 3);
      dest[i / 4] |= coverage << (6 - (i % 4) * 2);
    }
  entry = {key, pixelUsed_};
  pixelUsed_ += bytes;
  ++rasterizations_;
  return dest;
}
}  // namespace freeink::font
