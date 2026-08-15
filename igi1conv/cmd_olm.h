#pragma once
#include <string>
#include <vector>
#include <cstdint>

#pragma pack(push, 1)
struct OlmMainHeader {
    float version1;
    float version2;
    uint32_t year;
    uint32_t month;
    uint32_t day;
    uint32_t hour;
    uint32_t minute;
    uint32_t second;
    uint32_t millisecond;
    uint32_t unknown_0;
    uint32_t count1;
    uint32_t layer_count;
    uint32_t reserved[4];
    uint16_t width;
    uint16_t height;
    uint16_t total_stride;
    uint16_t format;
    uint32_t pad;
    float uv_scale_u;
    float uv_scale_v;
    float zero;
};

struct OlmLayerDescriptor {
    uint32_t flags;
    uint32_t ptr1;
    uint32_t ptr2;
    uint16_t pixel_width;
    uint16_t pixel_height;
};

struct OlmPixel {
    uint8_t r, g, b, a;
};
#pragma pack(pop)

struct OLMFile {
    bool valid = false;
    std::string error;
    OlmMainHeader header;
    OlmLayerDescriptor layer;
    std::vector<OlmPixel> pixels;
};

OLMFile ParseOlm(const std::string& path);

// Write an OLMFile back to disk in the IGI1 single-layer binary layout
// (88-byte main header + 16-byte layer descriptor + RGBA pixels). The pixels
// in `olm` are expected in native OLM channel order (R,G,B,A as stored on
// disk) — callers converting from an image must swap R/B first. Returns true
// on success; on failure sets `err`.
bool WriteOlm(const std::string& path, const OLMFile& olm, std::string& err);

// Build an OLMFile carrying the given RGBA pixels (image order, R first) at
// width x height. If `templateOlm` is non-null its header/layer metadata
// (uv scale, version, etc.) are copied so a re-baked file keeps the original
// runtime fields; otherwise sensible IGI1 defaults are filled in. The pixels
// are stored in native OLM order (R/B swapped from the supplied image order).
OLMFile BuildOlmFromRGBA(const uint8_t* rgba, uint16_t width, uint16_t height,
                         const OLMFile* templateOlm);

int cmd_olm(int argc, char** argv);
