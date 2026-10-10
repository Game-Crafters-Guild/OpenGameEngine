#pragma once

#include "Types/Types.h"

#include <span>

namespace GameEngine {

/**
 * @brief Gives back alpha 0 to the BC7 endpoints that the encoder's p-bit vote lifts to 1/255.
 *
 * BC7 modes 6 and 7 store each endpoint's channels without their lowest bit and add one
 * shared p-bit per endpoint as the lowest bit of all four channels. The cook's encoder
 * (DirectXTex) does not choose that p-bit by error: it sets it when three or four of the
 * endpoint's channel LSBs are 1. A transparent endpoint under odd RGB, white (255, 255, 255, 0)
 * for example, has three odd channels, so the vote sets p = 1 and the endpoint decodes to
 * alpha 1/255 in mode 6 and 4/255 in mode 7. Under an HDR particle colour that alpha floor
 * draws the whole quad.
 *
 * For each 4x4 block whose source texels include alpha 0, this clears the p-bit of every
 * mode 6 or mode 7 endpoint whose stored alpha bits are all 0. The endpoint then decodes to
 * alpha 0 exactly, and its RGB moves down by one p-bit step: 1/255 in mode 6, 4/255 in
 * mode 7 (a 6-bit endpoint expands as (v << 2) | (v >> 4), so its lowest bit is worth 4).
 * Texels interpolated from that endpoint move by no more than the same amount.
 * Modes 0 to 3 carry no alpha and modes 4 and 5 store alpha without a p-bit, so their blocks
 * are not changed.
 *
 * In mode 7 the gate is the whole block, not the subset that holds the transparent texel. A
 * per-subset gate needs the 64-entry two-subset partition table, which only the encoder holds,
 * and a copy here would be a second BC7 table to keep in step. The wider gate is safe: it only
 * touches an endpoint whose alpha bits are already 0, it moves that endpoint toward 0 (alpha by
 * 4/255, RGB by 4 steps), and a subset without a transparent texel whose
 * endpoint decodes to 4/255 loses at most those 4 steps of alpha.
 *
 * Only the endpoint is repaired. A transparent texel that the encoder put on an interpolated
 * index keeps the alpha of that index; that is the shared-index alpha error that
 * TextureAlphaDecodeProbe.h describes, not the p-bit vote.
 *
 * Bit layouts (bit 0 is the lowest bit of byte 0):
 * - Mode 6: 7 mode bits, then R0 R1 G0 G1 B0 B1 A0 A1 at 7 bits each, then P0 (bit 63), P1 (bit 64).
 * - Mode 7: 8 mode bits, 6 partition bits, then R, G, B and A for the four endpoints at 5 bits
 *   each (alpha at bits 74 to 93), then P0 to P3 (bits 94 to 97), one per endpoint in the
 *   same order.
 *
 * @param rgba   Source texels of the encoded image: RGBA8, tightly packed, width x height.
 * @param width  Image width in texels.
 * @param height Image height in texels.
 * @param blocks The BC7 payload of that image: ceil(width / 4) x ceil(height / 4) blocks of
 *               16 bytes, row-major. A payload of any other size asserts and is not changed.
 */
void RepairBC7TransparentEndpoints(const uint8* rgba, uint32 width, uint32 height, std::span<uint8> blocks);

} // namespace GameEngine
