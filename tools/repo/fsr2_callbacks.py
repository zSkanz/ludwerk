r"""Derives `shaders/include/engine_fsr2_callbacks.hlsli` from AMD's `ffx_fsr2_callbacks_hlsl.h` (ADR 0164).

FSR 2 is AMD's shader headers (third_party/fidelityfx_fsr2), written to be compiled against a
"callbacks" header the integrator supplies: how each image is read and written, and where the frame's
constants are. AMD ships one for Direct3D's bare registers. The engine's is that file with the changes
its head lists, and this script is those changes -- so the vendored copy is never edited (R13), and a
newer FSR 2 is this script run again:

    python tools/repo/fsr2_callbacks.py            # rewrite the header
    python tools/repo/fsr2_callbacks.py --check    # exit 1 if the header is not what this would write

Every step asserts what it expects to find. A step that no longer matches a newer upstream stops the
script there, which is the point: the change has to be looked at again, not silently skipped.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UPSTREAM = 'third_party/fidelityfx_fsr2/src/ffx-fsr2-api/shaders/'
SOURCE = UPSTREAM + 'ffx_fsr2_callbacks_hlsl.h'
OUTPUT = 'shaders/include/engine_fsr2_callbacks.hlsli'

HEAD = '''// **The engine's callbacks for FSR 2** (ADR 0164): how AMD's algorithm, which
// is its portable headers (third_party/fidelityfx_fsr2, MIT), reads and writes
// its images through this engine's compute passes.
//
// **Generated** by `tools/repo/fsr2_callbacks.py` from AMD's own
// `ffx_fsr2_callbacks_hlsl.h`, whose licence is above and whose every function
// is kept by name. Edit the script, not this file. What it changes:
//
//   - **The registers carry spaces**: what a compute shader samples is in
//     space 0, what it writes in space 1, its constants in space 2 -- the RHI's
//     layout, where AMD's is Direct3D's bare registers.
//   - **A sampler a texture**, at the texture's own index: the RHI binds a
//     texture with its sampler. AMD's has two samplers for every texture.
//   - **A texel is read through its texture's sampler** (`ENG_FSR2_LOAD`),
//     where AMD's indexes the texture: a texture a shader loads from and
//     never samples is another kind of binding to the RHI, in another range
//     of slots.
//   - **Every image a pass writes says its format**, and each is one of the
//     three every device stores to with no feature asked of it: sixteen-bit
//     float in four channels, thirty-two-bit float in one, eight bits in four.
//     AMD's are narrower where it has two channels or one to keep -- formats a
//     Vulkan device stores to only as an extension. An image of two channels
//     is declared with four: the compiler asks for that extension on seeing
//     two, whatever format it is then given.
//   - **The reconstructed previous depth is read through the binding it is
//     written by**, where AMD's samples it in the pass after: a uint image is
//     not sampled, and loading from it would be the RHI's other kind of
//     binding. It is written as AMD writes it -- an atomic minimum -- on
//     Direct3D and Vulkan. **On Metal it is read, compared and written**
//     (`ENG_SHADER_MSL`, which the build defines for that target): the
//     shading language has had atomics on an image only since its version
//     3.1. Two threads that reproject onto one pixel in one dispatch can
//     leave the further of their depths there, and a pixel that came out
//     from behind something is then missed for a frame.
//   - **The luminance is one image**, the size of AMD's fifth mip of half the
//     render size, made by a pass of the engine's own (`fsr2_luminance`):
//     AMD's chain is made with atomics, and only that level of it is read.
//   - **The exposure is the engine's metered luminance**, made a gain where
//     it is read (`Exposure`).
//   - The branch for AMD's internal builds is gone.

'''

LOAD = '''
// **A texel read is a sample at the texel's middle**, through the texture's
// own sampler. At a texel's middle a sampler gives that texel, exactly.
// **Where the middle is comes from the frame's constants** -- an image is the
// render size, the output's, or the luminance's -- and not from asking the
// texture at every read, which measured a quarter of what the passes cost. An
// image of one texel, which is what stands in for a mask nobody made, answers
// that texel wherever it is read.
#define ENG_FSR2_LOAD(texture, at) texture.SampleLevel(s_##texture, engFsr2Uv_##texture(at), 0)'''

# The format of every image a pass writes, and whether its declaration is widened to four channels.
FORMATS = {
    'rw_reconstructed_previous_nearest_depth': 'r32ui',
    'rw_dilated_motion_vectors': 'rgba16f',
    'rw_dilatedDepth': 'r32f',
    'rw_internal_upscaled_color': 'rgba16f',
    'rw_lock_status': 'rgba16f',
    'rw_lock_input_luma': 'r32f',
    'rw_new_locks': 'r32f',
    'rw_prepared_input_color': 'rgba16f',
    'rw_luma_history': 'rgba8',
    'rw_upscaled_output': 'rgba16f',
    'rw_img_mip_shading_change': 'rgba16f',
    'rw_img_mip_5': 'r32f',
    'rw_dilated_reactive_masks': 'rgba8',
    'rw_exposure': 'rgba16f',
    'rw_auto_exposure': 'rgba16f',
    'rw_spd_global_atomic': 'r32ui',
    'rw_output_autoreactive': 'r32f',
    'rw_output_autocomposition': 'r32f',
    'rw_output_prev_color_pre_alpha': 'rgba16f',
    'rw_output_prev_color_post_alpha': 'rgba16f',
}
# Declared with four channels: the two-channel ones, and the luminance, which a device must filter.
WIDENED = ['rw_dilated_motion_vectors', 'rw_lock_status', 'rw_dilated_reactive_masks', 'rw_exposure',
           'rw_auto_exposure', 'rw_img_mip_shading_change']

# Which of the frame's sizes each sampled image is: where a texel's middle is. `None` is one texel.
SIZES = {
    'RenderSize()': ['r_input_color_jittered', 'r_input_opaque_only', 'r_input_motion_vectors', 'r_input_depth',
                     'r_reactive_mask', 'r_transparency_and_composition_mask',
                     'r_reconstructed_previous_nearest_depth', 'r_dilated_motion_vectors',
                     'r_previous_dilated_motion_vectors', 'r_dilatedDepth', 'r_lock_input_luma',
                     'r_prepared_input_color', 'r_dilated_reactive_masks', 'r_input_prev_color_pre_alpha',
                     'r_input_prev_color_post_alpha'],
    'DisplaySize()': ['r_internal_upscaled_color', 'r_lock_status', 'r_new_locks', 'r_luma_history', 'r_rcas_input'],
    'LumaMipDimensions()': ['r_imgMips'],
    None: ['r_input_exposure', 'r_auto_exposure'],
    # The two tables are read at their own size, which only they know; nothing the engine builds reads them.
    'own': ['r_lanczos_lut', 'r_upsample_maximum_bias_lut'],
}

PREVIOUS_DEPTH = '''#if defined(FSR2_BIND_UAV_RECONSTRUCTED_PREV_NEAREST_DEPTH)
// **Read through the binding it is written by**: a uint image is not sampled,
// and one a shader loads from is another kind of binding to the RHI.
FfxFloat32 LoadReconstructedPrevDepth(FfxUInt32x2 iPxPos)
{
    return asfloat(rw_reconstructed_previous_nearest_depth[iPxPos]);
}

void StoreReconstructedDepth(FfxUInt32x2 iPxSample, FfxFloat32 fDepth)
{
    FfxUInt32 uDepth = asuint(fDepth);

#if defined(ENG_SHADER_MSL)
    // **Not atomic on Metal** (the head of this file): read, compared, written.
    const FfxUInt32 uHeld = rw_reconstructed_previous_nearest_depth[iPxSample];
    #if FFX_FSR2_OPTION_INVERTED_DEPTH
        rw_reconstructed_previous_nearest_depth[iPxSample] = max(uHeld, uDepth);
    #else
        rw_reconstructed_previous_nearest_depth[iPxSample] = min(uHeld, uDepth);
    #endif
#else
    #if FFX_FSR2_OPTION_INVERTED_DEPTH
        InterlockedMax(rw_reconstructed_previous_nearest_depth[iPxSample], uDepth);
    #else
        InterlockedMin(rw_reconstructed_previous_nearest_depth[iPxSample], uDepth); // min for standard, max for inverted depth
    #endif
#endif
}

void SetReconstructedDepth(FfxUInt32x2 iPxSample, const FfxUInt32 uValue)
{
    rw_reconstructed_previous_nearest_depth[iPxSample] = uValue;
}
#endif

'''

EXPOSURE_BEFORE = '''    FfxFloat32 exposure = ENG_FSR2_LOAD(r_input_exposure, FfxUInt32x2(0, 0)).x;

    if (exposure == 0.0f) {
        exposure = 1.0f;
    }

    return exposure;'''
EXPOSURE_AFTER = '''    // **The engine meters a luminance, not a gain** (`luminance_adapt.hlsl`):
    // the frame's average, which the tonemap brings to its key. The same
    // division here, so what the algorithm weighs is what the eye is shown.
    const FfxFloat32 measured = ENG_FSR2_LOAD(r_input_exposure, FfxUInt32x2(0, 0)).x;

    if (measured <= 0.0f) {
        return 1.0f;
    }

    return 0.45f / max(measured, 1e-4f);'''


def derive(src):
    def swap(a, b, count=1):
        nonlocal src
        assert src.count(a) == count, (a[:80], src.count(a))
        src = src.replace(a, b)

    # The licence block stays; the engine's head goes after it.
    cut = src.index('#include "ffx_fsr2_resources.h"')
    src = src[:cut] + HEAD + src[cut:]
    swap('#include "ffx_fsr2_resources.h"', '#include "../../' + UPSTREAM + 'ffx_fsr2_resources.h"')
    swap('#include "ffx_core.h"', '#include "../../' + UPSTREAM + 'ffx_core.h"')

    # --- The registers carry spaces, and a sampler has one of its own ---------------------------
    swap('''#define DECLARE_SRV_REGISTER(regIndex)  t##regIndex
#define DECLARE_UAV_REGISTER(regIndex)  u##regIndex
#define DECLARE_CB_REGISTER(regIndex)   b##regIndex
#define FFX_FSR2_DECLARE_SRV(regIndex)  register(DECLARE_SRV_REGISTER(regIndex))
#define FFX_FSR2_DECLARE_UAV(regIndex)  register(DECLARE_UAV_REGISTER(regIndex))
#define FFX_FSR2_DECLARE_CB(regIndex)   register(DECLARE_CB_REGISTER(regIndex))''',
         '''#define DECLARE_SRV_REGISTER(regIndex)  t##regIndex
#define DECLARE_SAMPLER_REGISTER(regIndex)  s##regIndex
#define DECLARE_UAV_REGISTER(regIndex)  u##regIndex
#define DECLARE_CB_REGISTER(regIndex)   b##regIndex
#define FFX_FSR2_DECLARE_SRV(regIndex)  register(DECLARE_SRV_REGISTER(regIndex), space0)
#define FFX_FSR2_DECLARE_SAMPLER(regIndex)  register(DECLARE_SAMPLER_REGISTER(regIndex), space0)
#define FFX_FSR2_DECLARE_UAV(regIndex)  register(DECLARE_UAV_REGISTER(regIndex), space1)
#define FFX_FSR2_DECLARE_CB(regIndex)   register(DECLARE_CB_REGISTER(regIndex), space2)
''' + LOAD)
    swap('''SamplerState s_PointClamp : register(s0);
SamplerState s_LinearClamp : register(s1);
''', '')

    # --- The branch for AMD's internal builds, out ------------------------------------------------
    start = src.index('#if defined(FFX_INTERNAL)\n    Texture2D<FfxFloat32x4>')
    middle = src.index('#else // #if defined(FFX_INTERNAL)\n', start)
    src = src[:start] + src[middle + len('#else // #if defined(FFX_INTERNAL)\n'):]
    swap('#endif // #if defined(FFX_INTERNAL)\n', '')
    src = src.replace(' || defined(FFX_INTERNAL)', '')
    start = src.index('#if defined(FFX_INTERNAL)\nFfxFloat32x4 SampleDebug')
    finish = src.index('#endif\n', start) + len('#endif\n')
    src = src[:start] + src[finish:]
    assert 'FFX_INTERNAL' not in src

    # --- A sampler a texture, and the middle of a texel of it ----------------------------------------
    size_of = {name: size for size, names in SIZES.items() for name in names}
    declared = []

    def with_sampler(match):
        indent, name, index = match.group(1), match.group(3), match.group(4)
        declared.append(name)
        size = size_of[name]
        if size is None:
            middle_of = 'return FfxFloat32x2(0.5f, 0.5f);'
        elif size == 'own':
            middle_of = ('FfxUInt32 w;\n' + indent + '    FfxUInt32 h;\n' + indent + '    ' + name +
                         '.GetDimensions(w, h);\n' + indent + '    return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(w, h);')
        else:
            middle_of = 'return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(' + size + ');'
        return (match.group(0) + '\n' + indent + 'SamplerState' + ' ' * 31 + 's_' + name + ' : FFX_FSR2_DECLARE_SAMPLER(' +
                index + ');\n' + indent + 'FfxFloat32x2 engFsr2Uv_' + name + '(FfxInt32x2 at)\n' + indent + '{\n' +
                indent + '    ' + middle_of + '\n' + indent + '}')

    src = re.sub(r'^( *)Texture2D<([^>]+)> +(r_\w+) +: FFX_FSR2_DECLARE_SRV\((\w+)\);', with_sampler, src, flags=re.M)
    assert sorted(declared) == sorted(size_of), sorted(set(declared) ^ set(size_of))
    sampled = re.findall(r'(r_\w+)\.SampleLevel\(s_LinearClamp,', src)
    assert sampled and all(name in declared for name in sampled), sampled
    src = re.sub(r'(r_\w+)\.SampleLevel\(s_LinearClamp,', lambda m: '%s.SampleLevel(s_%s,' % (m.group(1), m.group(1)), src)
    assert 's_LinearClamp' not in src and 's_PointClamp' not in src

    # --- The luminance: one image -----------------------------------------------------------------------
    swap('''    return r_imgMips.mips[mipLevel][iPxPos];''', '''    return r_imgMips[iPxPos];''')
    swap('''    return r_imgMips.SampleLevel(s_r_imgMips, fUV, mipLevel);''', '''    return r_imgMips.SampleLevel(s_r_imgMips, fUV, 0);''')

    # --- Every texel read, through the texture's sampler ------------------------------------------------
    src, loads = re.subn(r'(?<![\w.])(r_\w+)\[([^\]]+)\]', r'ENG_FSR2_LOAD(\1, \2)', src)
    assert loads >= 20, loads

    # --- Every written image says its format; two channels are declared as four ---------------------------
    seen = []

    def with_format(match):
        indent, qualifier, kind, name = match.group(1), match.group(2) or '', match.group(3), match.group(4)
        seen.append(name)
        fmt = FORMATS[name]
        # A float image is not `unorm`: what is stored is what was written.
        if fmt in ('rgba16f', 'r32f'):
            kind = kind.replace('unorm ', '')
        if name in WIDENED:
            kind = 'FfxFloat32x4'
        return '%s[[vk::image_format("%s")]] %sRWTexture2D<%s> %s ' % (indent, fmt, qualifier, kind, name)

    src = re.sub(r'^( *)(globallycoherent )?RWTexture2D<([^>]+)> +(rw_\w+) +', with_format, src, flags=re.M)
    assert sorted(seen) == sorted(FORMATS), sorted(set(FORMATS) ^ set(seen))
    for name in WIDENED:
        if name == 'rw_img_mip_shading_change':
            continue  # written by the engine's own pass, with four channels there
        src, stores = re.subn(r'^( *)' + name + r'\[([^\]]+)\] = ([^;]+);',
                              r'\1' + name + r'[\2] = FfxFloat32x4(\3, 0.0f, 0.0f);', src, flags=re.M)
        assert stores >= (0 if name in ('rw_exposure', 'rw_auto_exposure') else 1), (name, stores)
        assert not re.search(r'(?<![\w.])' + name + r'\[[^\]]+\](?! *=)', src), name

    # --- The previous depth: read through its write binding, atomic off Metal -------------------------------
    start = src.index('#if defined(FSR2_BIND_SRV_RECONSTRUCTED_PREV_NEAREST_DEPTH)\nFfxFloat32 LoadReconstructedPrevDepth')
    finish = src.index('#if defined(FSR2_BIND_UAV_DILATED_DEPTH)\nvoid StoreDilatedDepth')
    block = src[start:finish]
    assert 'InterlockedMin' in block and 'SetReconstructedDepth' in block and block.count('#endif') == 4, block
    src = src[:start] + PREVIOUS_DEPTH + src[finish:]

    # --- The engine's exposure --------------------------------------------------------------------------------
    swap(EXPOSURE_BEFORE, EXPOSURE_AFTER)
    return src


def main():
    os.chdir(ROOT)
    upstream = open(SOURCE, encoding='utf-8').read().replace('\r\n', '\n')
    derived = derive(upstream)
    if '--check' in sys.argv[1:]:
        held = open(OUTPUT, encoding='utf-8').read() if os.path.exists(OUTPUT) else ''
        if held != derived:
            print('fsr2_callbacks: ' + OUTPUT + ' is not what the script derives; run it without --check')
            return 1
        print('fsr2_callbacks: ' + OUTPUT + ' is current')
        return 0
    open(OUTPUT, 'w', encoding='utf-8', newline='\n').write(derived)
    print('fsr2_callbacks: wrote ' + OUTPUT + ', ' + str(len(derived.split('\n'))) + ' lines')
    return 0


if __name__ == '__main__':
    sys.exit(main())
