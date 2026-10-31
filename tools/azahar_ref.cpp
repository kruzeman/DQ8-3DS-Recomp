// DQ8 sampler differential test — reference side.
//
// This bridge exposes azahar's REAL texture decoder (compiled verbatim from
// /home/hermes/DQ8/azahar-master: src/video_core/texture/texture_decode.cpp +
// etc1.cpp + common/color.h) to the C comparison driver:
//
//   az_lookup_texel()  = Pica::Texture::LookupTexture (texture_decode.cpp:58)
//   az_sample_uv()     = the per-texture-unit math of
//                        RasterizerSoftware::TextureColor
//                        (renderer_software/sw_rasterizer.cpp:476-566) plus
//                        SwRenderer::GetWrappedTexCoord
//                        (renderer_software/sw_texturing.cpp:16-54).
//                        GetWrappedTexCoord / the UV math are ported line for
//                        line here because sw_rasterizer.cpp cannot be linked
//                        standalone (TEV/framebuffer deps); the decoder they
//                        call is compiled azahar code, not a reimplementation.
//
// The texture-coordinate arithmetic uses f32 instead of azahar's f24; the
// test driver only feeds coordinates whose u*width products are exactly
// representable in both (k/16 on 16x16 textures), so the integer texel
// coordinates entering the decoder are bit-identical.
#include <cstdint>

#include "video_core/pica/regs_texturing.h"
#include "video_core/texture/texture_decode.h"

using TextureFormat = Pica::TexturingRegs::TextureFormat;
using WrapMode = Pica::TexturingRegs::TextureConfig::WrapMode;

extern "C" void az_lookup_texel(const uint8_t* data, uint32_t w, uint32_t h,
                                uint32_t fmt, uint32_t x, uint32_t y,
                                uint8_t out[4]) {
    Pica::Texture::TextureInfo info{};
    info.physical_address = 0;
    info.width = w;
    info.height = h;
    info.format = static_cast<TextureFormat>(fmt);
    info.SetDefaultStride();  // stride = CalculateTileSize * (width / 8)
    info.is_shadow_source = false;
    auto c = Pica::Texture::LookupTexture(data, x, y, info, false);
    out[0] = c.r();
    out[1] = c.g();
    out[2] = c.b();
    out[3] = c.a();
}

// Verbatim port of SwRenderer::GetWrappedTexCoord (sw_texturing.cpp:16-54).
static int get_wrapped_tex_coord(int mode, int val, unsigned size) {
    switch (mode) {
    case WrapMode::ClampToEdge2:  // 4
        if (val < 0) {
            return (int)((unsigned)val % size);
        }
        [[fallthrough]];
    case WrapMode::ClampToEdge:  // 0
        val = val > 0 ? val : 0;
        val = val < (int)size - 1 ? val : (int)size - 1;
        return val;
    case WrapMode::ClampToBorder:  // 1
        return val;
    case WrapMode::ClampToBorder2:  // 5
    case WrapMode::Repeat2:         // 6
    case WrapMode::Repeat3:         // 7
    case WrapMode::Repeat:          // 2
        return (int)((unsigned)val % size);
    case WrapMode::MirroredRepeat: {  // 3
        unsigned coord = (unsigned)val % (2 * size);
        if (coord >= size) {
            coord = 2 * size - 1 - coord;
        }
        return (int)coord;
    }
    default:
        return 0;
    }
}

// Verbatim port of the per-unit UV/border/wrap math in
// RasterizerSoftware::TextureColor (sw_rasterizer.cpp:530-566), minus the
// texture-type switch (the DQ8 pipeline only binds plain Texture2D on unit 0)
// and the shadow comparison.
extern "C" void az_sample_uv(const uint8_t* data, uint32_t w, uint32_t h,
                             uint32_t fmt, uint32_t wrap_s, uint32_t wrap_t,
                             uint32_t border_rgba, float uf, float vf,
                             uint8_t out[4]) {
    int s = (int)(uf * (float)w);
    int t = (int)(vf * (float)h);

    bool use_border_s = false;
    bool use_border_t = false;
    if (wrap_s == (uint32_t)WrapMode::ClampToBorder) {
        use_border_s = s < 0 || s >= (int)w;
    } else if (wrap_s == (uint32_t)WrapMode::ClampToBorder2) {
        use_border_s = s >= (int)w;
    }
    if (wrap_t == (uint32_t)WrapMode::ClampToBorder) {
        use_border_t = t < 0 || t >= (int)h;
    } else if (wrap_t == (uint32_t)WrapMode::ClampToBorder2) {
        use_border_t = t >= (int)h;
    }

    if (use_border_s || use_border_t) {
        out[0] = (uint8_t)(border_rgba & 0xFFu);         // r
        out[1] = (uint8_t)((border_rgba >> 8) & 0xFFu);  // g
        out[2] = (uint8_t)((border_rgba >> 16) & 0xFFu); // b
        out[3] = (uint8_t)((border_rgba >> 24) & 0xFFu); // a
    } else {
        s = get_wrapped_tex_coord((int)wrap_s, s, w);
        t = h - 1 - get_wrapped_tex_coord((int)wrap_t, t, h);
        az_lookup_texel(data, w, h, fmt, (uint32_t)s, (uint32_t)t, out);
    }
}
