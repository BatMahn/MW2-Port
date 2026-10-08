/*
 * tex.h - texture (CEL resource) decoding for the 3D-accelerated editions.
 *
 * Same artwork, four encodings (cross-checked texel by texel between editions):
 *   TEX_ATI     u16 w, u16 h, ARGB1555, rows. Alpha bit 1 = opaque. All textures.
 *               Best quality copy: keeps 5 bits per channel everywhere.
 *   TEX_3DFX    u16 w, u16 h, rows; ARGB4444 when the texture has transparency,
 *               RGB555 when opaque (688 vs 635 textures; format isn't stored, so
 *               the caller must say which). S3's textures are byte-identical.
 *   TEX_PVR     16-byte header "PT..", Morton-twiddled ARGB4444, alpha inverted
 *               (0 = opaque); some records hold full mipmap chains.
 * About 73 explosion/effect frames use an encoding not yet decoded.
 */
#ifndef MW2_TEX_H
#define MW2_TEX_H

#include <stddef.h>
#include <stdint.h>

enum { TEX_ATI, TEX_3DFX_555, TEX_3DFX_4444, TEX_PVR, TEX_DOS, TEX_MGA };
/* TEX_MGA: the Matrox Mystique edition: u16 w, u16 h, RGB555 with bit 15 = opaque (0x0000 transparent - checked against
 * the ATi copy: identical colours, keyed texels exactly the ATi's transparent ones); the ground textures are 256 x 256. The
 * engine uploads every texture as a 16- or 256-colour palette (0x1005f560): up to 255 colours kept, else masked 0x7BDE,
 * else clustered by nearest colour as a running mean - reproduced here. */
/* TEX_DOS: the DOS archive's CELs (.XEL): u16 w, u16 h, w x h palette indices, 0xff transparent. Decoded with the INDEX
 * in red, green and blue (the renderer shades it through the LUMA table and the palette, as the DOS engine); alpha 0 for
 * 0xff. */

typedef struct {
    int       w, h;
    uint32_t *rgba;   /* 0xAABBGGRR, malloc'd */
} texture;

int  tex_decode(const uint8_t *data, size_t len, int encoding, texture *out);
void tex_free(texture *t);

#endif
