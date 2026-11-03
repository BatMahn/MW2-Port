#!/usr/bin/env python3
"""gen_shaders.py - compile the renderer's shaders (shaders/*.vert, *.frag) for every SDL_GPU backend and write them
into src/rb_shaders.c / src/rb_shaders.h, which are checked in: building the port needs no shader tools.

    make shaders                (or: python3 scripts/gen_shaders.py [--tools DIR])

Pipeline, per shader and variant:
    GLSL 450 --glslangValidator--> SPIR-V          (Vulkan)
    SPIR-V   --spirv-cross --msl--> MSL 2.1 source  (Metal; the storage buffers renumbered after the uniform buffers,
                                                     SDL_GPU's [[buffer]] order)
    SPIR-V   --spirv-cross --hlsl--> HLSL SM 6.0 --dxc--> DXIL (Direct3D 12)
The uniform blocks' member offsets are read back from SPIR-V (spirv-cross --reflect) and written as C structs with
the same layout (std140), checked identical between the stages that share a block.

Tools: glslangValidator, spirv-cross (1.4.3xx or newer), dxc (DirectXShaderCompiler 1.8+: its built-in validator
signs the DXIL). They are found on PATH or in --tools DIR (DIR/bin, DIR/lib for dxc's libdxcompiler.so). The checked-in
files were made with glslangValidator 15.1.0 (Ubuntu 24.04's glslang-tools), SPIRV-Cross git 15142bb (2026-09-28, C API
0.68.0) and DXC 1.10.2605 (the last two conda-forge linux-64 packages); the output is deterministic (the same tools give
byte-identical files)."""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SH = os.path.join(ROOT, 'shaders')

# (C name, source file, stage, defines). The world shader comes in two variants (DOS_LOOK 0 / 1).
SHADERS = [
    ('world_vs', 'world.vert', 'vert', {'DOS_LOOK': '0'}),
    ('world_fs', 'world.frag', 'frag', {'DOS_LOOK': '0'}),
    ('world_dos_vs', 'world.vert', 'vert', {'DOS_LOOK': '1'}),
    ('world_dos_fs', 'world.frag', 'frag', {'DOS_LOOK': '1'}),
    ('hud_vs', 'hud.vert', 'vert', {}),
    ('hud_fs', 'hud.frag', 'frag', {}),
    ('clear_vs', 'clear.vert', 'vert', {}),
    ('clear_fs', 'clear.frag', 'frag', {}),
]
# the uniform blocks written as C structs (block name -> C struct name)
BLOCKS = {'WorldFrame': 'rb_world_frame', 'WorldDraw': 'rb_world_draw', 'HudDraw': 'rb_hud_draw', 'ClearDraw': 'rb_clear_draw'}


def tool(args, name, env_lib=False):
    for d in ([os.path.join(args.tools, 'bin'), args.tools] if args.tools else []):
        p = os.path.join(d, name)
        if os.path.exists(p):
            return p
    p = shutil.which(name)
    if not p:
        sys.exit('gen_shaders: %s not found (PATH or --tools)' % name)
    return p


def run(cmd, env=None):
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    out = r.stdout.decode('utf-8', 'replace')
    if r.returncode != 0:
        sys.exit('gen_shaders: %s failed:\n%s' % (' '.join(cmd), out))
    return out


def spv_words(b):
    return list(struct.unpack('<%dI' % (len(b) // 4), b))


def spv_rebind(b, remap):
    """SPIR-V with the Binding decorations of the resources at remap's {(set, binding): new binding} changed"""
    w = spv_words(b)
    dset, at = {}, {}
    i = 5
    while i < len(w):   # OpDecorate (71) id DescriptorSet (34) s / Binding (33) b
        n, op = w[i] >> 16, w[i] & 0xffff
        if op == 71 and n >= 4 and w[i + 2] == 34:
            dset[w[i + 1]] = w[i + 3]
        if op == 71 and n >= 4 and w[i + 2] == 33:
            at[w[i + 1]] = i + 3
        i += max(n, 1)
    for vid, k in at.items():
        key = (dset.get(vid), w[k])
        if key in remap:
            w[k] = remap[key]
    return struct.pack('<%dI' % len(w), *w)


CTYPE = {'float': ('float', 1), 'int': ('int32_t', 1), 'uint': ('uint32_t', 1), 'vec2': ('float', 2), 'vec3': ('float', 3),
         'vec4': ('float', 4), 'ivec2': ('int32_t', 2), 'ivec3': ('int32_t', 3), 'ivec4': ('int32_t', 4), 'mat4': ('float', 16)}


def block_struct(refl, ubo):
    """the C struct of uniform block `ubo` (reflection entry) - members at their std140 offsets"""
    t = refl['types'][ubo['type']]
    fields, at, pad = [], 0, 0
    for m in t['members']:
        off = m['offset']
        if off > at:
            fields.append('    uint8_t _pad%d[%d];' % (pad, off - at))
            pad += 1
            at = off
        ct, n = CTYPE[m['type']]
        arr = m.get('array', [])
        stride = m.get('array_stride', 0)
        if arr:
            if stride != n * 4:
                sys.exit('gen_shaders: %s.%s: array stride %d (use vec4 / ivec4 arrays)' % (t['name'], m['name'], stride))
            fields.append('    %s %s[%d]%s;' % (ct, m['name'], arr[0], '[%d]' % n if n > 1 else ''))
            at += arr[0] * stride
        else:
            fields.append('    %s %s%s;' % (ct, m['name'], '[%d]' % n if n > 1 else ''))
            at += n * 4
    size = ubo['block_size']
    if size > at:
        fields.append('    uint8_t _pad%d[%d];' % (pad, size - at))
    return size, fields


def c_bytes(b):
    lines = []
    for i in range(0, len(b), 24):
        lines.append('    ' + ', '.join('0x%02x' % x for x in b[i:i + 24]) + ',')
    return '\n'.join(lines)


def c_string(s):
    out = []
    for ln in s.split('\n'):
        out.append('    "' + ln.replace('\\', '\\\\').replace('"', '\\"') + '\\n"')
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--tools', help='a folder holding bin/glslangValidator, bin/spirv-cross, bin/dxc (and lib/)')
    args = ap.parse_args()
    glslang, spvx, dxc = tool(args, 'glslangValidator'), tool(args, 'spirv-cross'), None
    for n in ('dxc', 'dxc-3.7'):
        try:
            dxc = tool(args, n)
            break
        except SystemExit:
            pass
    if not dxc:
        sys.exit('gen_shaders: dxc not found (PATH or --tools)')
    dxc_env = dict(os.environ)
    lib = os.path.join(os.path.dirname(os.path.dirname(dxc)), 'lib')
    if os.path.isdir(lib):
        dxc_env['LD_LIBRARY_PATH'] = lib + (':' + dxc_env['LD_LIBRARY_PATH'] if dxc_env.get('LD_LIBRARY_PATH') else '')
    versions = {
        'glslang': run([glslang, '--version']).split('\n')[0].strip(),
        'spirv-cross': run([spvx, '--revision']).strip() if True else '',
        'dxc': run([dxc, '--version'], env=dxc_env).strip().split('\n')[0],
    }
    tmp = tempfile.mkdtemp(prefix='mw2shaders')
    shaders, structs = [], {}
    try:
        for name, src, stage, defs in SHADERS:
            spv = os.path.join(tmp, name + '.spv')
            run([glslang, '-V', '--target-env', 'vulkan1.0', '-S', stage] + ['-D%s=%s' % kv for kv in defs.items()]
                + [os.path.join(SH, src), '-o', spv])
            run(['spirv-val', spv]) if shutil.which('spirv-val') else None
            b = open(spv, 'rb').read()
            refl = json.loads(run([spvx, spv, '--reflect']))
            ubos, ssbos = refl.get('ubos', []), refl.get('ssbos', [])
            samplers = refl.get('textures', [])
            for u in ubos:
                bn = refl['types'][u['type']]['name']
                size, fields = block_struct(refl, u)
                if bn in structs and structs[bn] != (size, fields):
                    sys.exit('gen_shaders: block %s differs between stages' % bn)
                structs[bn] = (size, fields)
            # Metal: [[buffer]] = the uniform buffers, then the storage buffers
            remap = {(s['set'], s['binding']): len(ubos) + k for k, s in enumerate(sorted(ssbos, key=lambda s: s['binding']))}
            mspv = os.path.join(tmp, name + '.msl.spv')
            open(mspv, 'wb').write(spv_rebind(b, remap))
            msl = run([spvx, mspv, '--msl', '--msl-version', '20100', '--msl-decoration-binding', '--output', '/dev/stdout'])
            hlsl = os.path.join(tmp, name + '.hlsl')
            run([spvx, spv, '--hlsl', '--shader-model', '60', '--output', hlsl])
            dxil = os.path.join(tmp, name + '.dxil')
            run([dxc, '-T', ('vs' if stage == 'vert' else 'ps') + '_6_0', '-E', 'main', hlsl, '-Fo', dxil], env=dxc_env)
            d = open(dxil, 'rb').read()
            if d[:4] != b'DXBC' or d[4:20] == b'\0' * 16:
                sys.exit('gen_shaders: %s: DXIL not signed (dxc 1.8 or newer signs with its built-in validator)' % name)
            shaders.append({'name': name, 'stage': stage, 'spirv': b, 'msl': msl, 'dxil': d, 'samplers': len(samplers),
                            'ubos': len(ubos), 'ssbos': len(ssbos), 'src': src, 'defs': defs})
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    regen = ('GENERATED by scripts/gen_shaders.py from the GLSL in shaders/ - do not edit. Regenerate with\n'
             ' * "make shaders" (glslangValidator, spirv-cross and dxc on PATH, or SHADER_TOOLS=<folder>) after changing a\n'
             ' * shader; the port builds from these files without any shader tools.\n'
             ' * Tools: %s; SPIRV-Cross %s; %s' % (versions['glslang'], versions['spirv-cross'], versions['dxc']))
    h = ['/* rb_shaders.h - ' + regen + ' */', '#ifndef MW2_RB_SHADERS_H', '#define MW2_RB_SHADERS_H', '',
         '#include <stddef.h>', '#include <stdint.h>', '']
    h.append('/* the uniform blocks (std140), as the shaders have them */')
    for bn, cn in BLOCKS.items():
        if bn not in structs:
            continue
        size, fields = structs[bn]
        h.append('typedef struct {')
        h.extend(fields)
        h.append('} %s;   /* %d bytes */' % (cn, size))
        h.append('typedef char %s_size_check[sizeof(%s) == %d ? 1 : -1];' % (cn, cn, size))
        h.append('')
    h.append('typedef struct {')
    h.append('    const char *name;')
    h.append('    int vertex;                          /* 1 vertex, 0 fragment */')
    h.append('    int samplers, uniform_buffers, storage_buffers;')
    h.append('    const unsigned char *spirv; size_t spirv_size;   /* Vulkan, entry "main" */')
    h.append('    const unsigned char *dxil; size_t dxil_size;     /* Direct3D 12, entry "main" */')
    h.append('    const char *msl; size_t msl_size;                /* Metal (MSL 2.1 source), entry "main0" */')
    h.append('} rb_shader_blob;')
    h.append('enum { ' + ', '.join('RB_SH_' + s['name'].upper() for s in shaders) + ', RB_SH_COUNT };')
    h.append('extern const rb_shader_blob rb_shader_blobs[RB_SH_COUNT];')
    h.append('')
    h.append('#endif')
    c = ['/* rb_shaders.c - ' + regen + ' */', '#include "rb_shaders.h"', '']
    for s in shaders:
        c.append('/* %s: shaders/%s%s */' % (s['name'], s['src'], ''.join(' -D%s=%s' % kv for kv in s['defs'].items())))
        c.append('static const unsigned char %s_spirv[%d] = {\n%s\n};' % (s['name'], len(s['spirv']), c_bytes(s['spirv'])))
        c.append('static const unsigned char %s_dxil[%d] = {\n%s\n};' % (s['name'], len(s['dxil']), c_bytes(s['dxil'])))
        c.append('static const char %s_msl[] =\n%s;' % (s['name'], c_string(s['msl'])))
        c.append('')
    c.append('const rb_shader_blob rb_shader_blobs[RB_SH_COUNT] = {')
    for s in shaders:
        c.append('    {"%s", %d, %d, %d, %d, %s_spirv, sizeof %s_spirv, %s_dxil, sizeof %s_dxil, %s_msl, sizeof %s_msl - 1},'
                 % (s['name'], s['stage'] == 'vert', s['samplers'], s['ubos'], s['ssbos'], s['name'], s['name'], s['name'],
                    s['name'], s['name'], s['name']))
    c.append('};')
    open(os.path.join(ROOT, 'src', 'rb_shaders.h'), 'w').write('\n'.join(h) + '\n')
    open(os.path.join(ROOT, 'src', 'rb_shaders.c'), 'w').write('\n'.join(c) + '\n')
    print('gen_shaders: %d shaders -> src/rb_shaders.c, src/rb_shaders.h' % len(shaders))
    for s in shaders:
        print('  %-14s spirv %6d  dxil %6d  msl %6d  samplers %d ubos %d ssbos %d' % (s['name'], len(s['spirv']), len(s['dxil']),
              len(s['msl']), s['samplers'], s['ubos'], s['ssbos']))


if __name__ == '__main__':
    main()
