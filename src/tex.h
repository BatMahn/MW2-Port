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

enum { TEX_ATI, TEX_3DFX_555, TEX_3DFX_4444, TEX_PVR, TEX_DOS, TEX_MGA, TEX_TITANIUM };
/* TEX_MGA: the Matrox Mystique edition: u16 w, u16 h, RGB555 with bit 15 = opaque (0x0000 transparent - checked against
 * the ATi copy: identical colours, keyed texels exactly the ATi's transparent ones); the ground textures are 256 x 256. The
 * engine uploads every texture as a 16- or 256-colour palette (0x1005f560): up to 255 colours kept, else masked 0x7BDE,
 * else clustered by nearest colour as a running mean - reproduced here. */
/* TEX_TITANIUM: the Titanium edition's 16-bit build (1998, Install/16bit/MW2.PRJ; engine mercsw.dll, renderer plugins
 * such as plugin/dx5draw.dll): "1.2x", u16 type, u16 w, u16 h, u16 n, n x 4-byte palette entries (B, G, R, A in memory:
 * the dword 0xAARRGGBB), w x h 8-bit indices. Only these records are textures: the engine's loader (mercsw 0x10002867)
 * cuts the record to its first three bytes and skips every one that is not "1.2" - the archive's 670 plain records
 * (u16 w, u16 h, indices: the 8-bit build's art) never reach the 3D renderer, so they decode as -1 here. The engine
 * hands the renderer the indices and the palette, format 0 for type 0 and format 1 for any other type (whose polygons
 * are drawn with Render type 8, alpha-blended: 0x1000131e); dx5draw converts them (0x10006200): format 0 to the
 * screen's 16-bit RGB, alpha ignored; format 1 to ARGB4444 with the alpha INVERTED (0x10006713: A = 15 - (a >> 4) -
 * a palette alpha of 255 is a transparent texel, 0 an opaque one). Reproduced: type 0 as RGB565, others ARGB4444. */
/* TEX_DOS: the DOS archive's CELs (.XEL): u16 w, u16 h, w x h palette indices, 0xff transparent. Decoded with the INDEX
 * in red, green and blue (the renderer shades it through the LUMA table and the palette, as the DOS engine); alpha 0 for
 * 0xff. */

typedef struct {
    int       w, h;
    uint32_t *rgba;   /* 0xAABBGGRR, malloc'd */
    int       type;   /* TEX_TITANIUM: the record's u16 type (mercsw keeps it at +0x14 of its texture entry, 0x100029cc;
                         0x1000219e returns it); 0 for the other encodings */
    /* TEX_3DFX_555 / TEX_3DFX_4444: the same texels read the other way (malloc'd; NULL for the other encodings). The 3Dfx
     * and S3 engines store no format: a texture is read as ARGB4444 when the polygon that first draws it has the alpha
     * flag (types 8-11), else as ARGB1555 (3Dfx 0x10067240 -> 0x10066e70 -> 0x10066a30: Glide 0xc / 0xb; S3 0x100107a0
     * -> 0x10010170: S3DTK_TEXARGB4444 / 1555) - glr holds both readings (tex / tex_a) and draws a texture in the one its
     * first polygon chose (upfmt; glr_view.tex_cache) */
    uint32_t *alt;
    int       alt4444;   /* 1: alt is the ARGB4444 reading (rgba the RGB555 one); 0: alt the RGB555, rgba the ARGB4444 */
    int       a1555;     /* TEX_3DFX_*: the texels with bit 15 set (the ARGB1555 reading's alpha bit) */
    int       cel;       /* the archive's CEL record id (the engines' texture id; 0 unknown) - set by the loader */
    /* (3Dfx / S3, with alt) the reading the engine's texture cache holds for it: 0 none yet, 1 ARGB1555, 2 ARGB4444 - the
     * first polygon that draws it decides (glr: glr_view.tex_cache); the loader clears it, glview at an S3 palette reload */
    int       upfmt;
} texture;

int  tex_decode(const uint8_t *data, size_t len, int encoding, texture *out);
void tex_free(texture *t);

#endif
