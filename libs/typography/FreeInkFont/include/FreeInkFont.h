#pragma once
#include <cstddef>
#include <cstdint>

namespace freeink::font {
// Caller owns both arenas and serializes access. No global state, filesystem,
// implicit heap allocations, or book-layout dependency. Font bytes are borrowed
// until close(); glyph pixels survive until the next rasterize() call.
struct Metrics {
  uint16_t advance16 = 0;
  int16_t left = 0, top = 0;
  uint8_t width = 0, height = 0;
};
struct FaceInfo {
  char family[64] = {};
  uint8_t style = 0;  // bit 0 bold, bit 1 italic
};
class Service {
 public:
  static constexpr uint32_t Revision = 1;
  static constexpr unsigned MaxFaces = 16;
  Service() = default;
  ~Service();
  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;
  bool begin(void* workspace, size_t workspaceBytes, void* pixels, size_t pixelBytes);
  void end();
  int open(const uint8_t* bytes, size_t length, FaceInfo* info = nullptr);
  void close(int face);
  bool covers(int face, uint32_t cp);
  bool metrics(int face, uint32_t cp, uint8_t points, Metrics& out);
  int16_t kerning16(int face, uint32_t left, uint32_t right, uint8_t points);
  uint32_t ligature(int face, uint32_t left, uint32_t right);
  bool lineMetrics(int face, uint8_t points, int& ascent, int& descent, int& height);
  const uint8_t* rasterize(int face, uint32_t cp, uint8_t points, bool darken = false);
  void clearPixels();
  size_t pixelUsed() const { return pixelUsed_; }
  uint32_t rasterizations() const { return rasterizations_; }

 private:
  struct Block;
  struct State;
  State* state_ = nullptr;
  uint8_t* pixels_ = nullptr;
  size_t pixelBytes_ = 0, pixelUsed_ = 0;
  uint32_t rasterizations_ = 0;
  static void* allocate(void* memory, long size);
  static void deallocate(void* memory, void* ptr);
  static void* reallocate(void* memory, long oldSize, long newSize, void* ptr);
  unsigned glyphIndex(int face, uint32_t cp);
  bool select(int face, uint8_t points);
};
}  // namespace freeink::font
