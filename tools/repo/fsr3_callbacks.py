r"""Derives the engine's callbacks headers for FSR 3's frame generation (ADR 0165) from AMD's own.

Frame generation is two of AMD's techniques, each its shader headers (third_party/fidelityfx_sdk) written to
be compiled against a "callbacks" header the integrator supplies: optical flow, and frame interpolation.
AMD ships one of each for Direct3D's bare registers. The engine's are those files with the changes their
heads list, and this script is those changes -- so the vendored copy is never edited (R13), and a newer SDK
is this script run again:

    python tools/repo/fsr3_callbacks.py            # rewrite the headers
    python tools/repo/fsr3_callbacks.py --check    # exit 1 if a header is not what this would write

Every step asserts what it expects to find. A step that no longer matches a newer upstream stops the script
there, which is the point: the change has to be looked at again, not silently skipped.

`tools/repo/fsr2_callbacks.py` is the same thing for FSR 2, and says more about why.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UPSTREAM = 'third_party/fidelityfx_sdk/sdk/include/FidelityFX/gpu/'
INCLUDE = '../../' + UPSTREAM


class Source:
    """A header being derived: every change asserts it found what it changes."""

    def __init__(self, text):
        self.text = text

    def swap(self, a, b, count=1):
        assert self.text.count(a) == count, (a[:90], self.text.count(a))
        self.text = self.text.replace(a, b)

    def sub(self, pattern, replacement, least=1, flags=re.M):
        self.text, made = re.subn(pattern, replacement, self.text, flags=flags)
        assert made >= least, (pattern[:90], made)
        return made


def formats(source, table, widened):
    """Every image a pass writes says its format, and a two-channel one is declared with four."""
    seen = set()

    def with_format(match):
        indent, qualifier, kind, name = match.group(1), match.group(2) or '', match.group(3), match.group(4)
        if name not in table:
            return match.group(0)
        seen.add(name)
        if name in widened:
            kind = widened[name]
        return '%s[[vk::image_format("%s")]] %sRWTexture2D<%s> %s' % (indent, table[name], qualifier, kind, name)

    source.text = re.sub(r'^( *)(globallycoherent )?RWTexture2D<([^>]+)> +(rw_\w+)', with_format, source.text, flags=re.M)
    assert seen == set(table), sorted(set(table) ^ seen)


def atomics(source, least):
    """An atomic on an image, where the target has one; read, changed and written on Metal, which has not."""

    def three(match):
        indent, image, value, out = match.group(1), match.group(2), match.group(3), match.group(4)
        return (indent + '#if defined(ENG_SHADER_MSL)\n' + indent + out + ' = ' + image + ';\n' + indent + image + ' = ' +
                out + ' + ' + value + ';\n' + indent + '#else\n' + match.group(0) + '\n' + indent + '#endif')

    def two(match):
        indent, image, value = match.group(1), match.group(2), match.group(3)
        return (indent + '#if defined(ENG_SHADER_MSL)\n' + indent + image + ' = ' + image + ' + ' + value + ';\n' + indent +
                '#else\n' + match.group(0) + '\n' + indent + '#endif')

    def least_or_most(match):
        indent, which, image, value, out = match.group(1), match.group(2), match.group(3), match.group(4), match.group(5)
        fold = 'max' if which == 'Max' else 'min'
        if out is None:
            body = indent + image + ' = ' + fold + '(' + image + ', ' + value + ');\n'
        else:
            body = indent + out + ' = ' + image + ';\n' + indent + image + ' = ' + fold + '(' + out + ', ' + value + ');\n'
        return indent + '#if defined(ENG_SHADER_MSL)\n' + body + indent + '#else\n' + match.group(0) + '\n' + indent + '#endif'

    made = source.sub(r'^( *)InterlockedAdd\((rw_\w+\[[^\]]+\]), ([^,;]+), (\w+)\);', three, least=0)
    made += source.sub(r'^( *)InterlockedAdd\((rw_\w+\[[^\]]+\]), ([^,;]+)\);', two, least=0)
    made += source.sub(r'^( *)Interlocked(Min|Max)\((rw_\w+\[[^\]]+\]), ([^,;]+?)(?:, (\w+))?\);(?: *//.*)?$', least_or_most, least=0)
    assert made >= least, made


# --- optical flow -------------------------------------------------------------------------------------------

OF_SOURCE = UPSTREAM + 'opticalflow/ffx_opticalflow_callbacks_hlsl.h'
OF_OUTPUT = 'shaders/include/engine_fsr3_of_callbacks.hlsli'

OF_HEAD = '''// **The engine's callbacks for FSR 3's optical flow** (ADR 0165): how AMD's
// technique, which is its portable headers (third_party/fidelityfx_sdk, MIT),
// reads and writes its images through this engine's compute passes.
//
// **Generated** by `tools/repo/fsr3_callbacks.py` from AMD's own
// `ffx_opticalflow_callbacks_hlsl.h`, whose licence is above and whose every
// function is kept by name. Edit the script, not this file. What it changes:
//
//   - **The registers carry spaces**: what a compute shader reads is in space
//     0, what it writes in space 1, its constants in space 2 -- the RHI's
//     layout, where AMD's is Direct3D's bare registers.
//   - **The picture it finds motion in is read through a sampler**, at a
//     texel's middle: a texture of colours that a shader loads from is the
//     RHI's other kind of binding. The images of integers are loaded, as
//     AMD loads them -- nothing samples an integer -- and are that other
//     kind, bound after.
//   - **Every image a pass writes says its format**, and it is one of two:
//     an unsigned integer of thirty-two bits, or a float of thirty-two. They
//     are what every device stores to with no feature asked of it, and what
//     every device reads back through the binding it writes by, which the
//     search does to the motion. AMD's luminance is eight bits and its motion
//     two signed channels of sixteen -- formats a Vulkan device stores to
//     only as an extension, and Direct3D reads back only where it says so.
//   - **A motion is packed into one integer**, its x in the low sixteen bits
//     and its y in the high (`engPackFlow`, `engUnpackFlow`), for that reason.
//   - **An atomic on an image is one on Direct3D and Vulkan.** On Metal it is
//     read, changed and written (`ENG_SHADER_MSL`, which the build defines for
//     that target): the shading language has had atomics on an image only
//     since its version 3.1. The engine does not generate frames on Metal;
//     the passes are compiled for it because every pass is.

'''

OF_FORMATS = {
    'rw_optical_flow_input': 'r32ui',
    'rw_optical_flow_input_level_1': 'r32ui',
    'rw_optical_flow_input_level_2': 'r32ui',
    'rw_optical_flow_input_level_3': 'r32ui',
    'rw_optical_flow_input_level_4': 'r32ui',
    'rw_optical_flow_input_level_5': 'r32ui',
    'rw_optical_flow_input_level_6': 'r32ui',
    'rw_optical_flow': 'r32ui',
    'rw_optical_flow_next_level': 'r32ui',
    'rw_optical_flow_histogram': 'r32ui',
    'rw_optical_flow_global_motion_search': 'r32ui',
    'rw_optical_flow_scd_histogram': 'r32ui',
    'rw_optical_flow_scd_previous_histogram': 'r32f',
    'rw_optical_flow_scd_temp': 'r32ui',
    'rw_optical_flow_scd_output': 'r32ui',
}
# Not widened: retyped. A motion is one packed integer.
OF_WIDENED = {'rw_optical_flow': 'FfxUInt32', 'rw_optical_flow_next_level': 'FfxUInt32'}

FLOW_PACK = """
// **A motion in whole pixels, in one integer**: x in the low sixteen bits, y
// in the high, each signed.
FfxUInt32 engPackFlow(FfxInt32x2 motion)
{
    return (FfxUInt32(motion.x) & 0xffffu) | (FfxUInt32(motion.y) << 16u);
}

FfxInt32x2 engUnpackFlow(FfxUInt32 packed)
{
    return FfxInt32x2(FfxInt32(packed << 16u) >> 16, FfxInt32(packed) >> 16);
}
"""


def derive_optical_flow(upstream):
    source = Source(upstream)
    cut = source.text.index('#ifndef FFX_OPTICALFLOW_CALLBACKS_HLSL_H')
    source.text = source.text[:cut] + OF_HEAD + source.text[cut:]
    source.swap('#include "ffx_core.h"', '#include "' + INCLUDE + 'ffx_core.h"')
    source.swap('#include "opticalflow/ffx_opticalflow_common.h"', '#include "' + INCLUDE + 'opticalflow/ffx_opticalflow_common.h"')

    # --- The registers carry spaces ---------------------------------------------------------------------
    source.swap('''#define FFX_OPTICALFLOW_DECLARE_SRV(regIndex)  register(DECLARE_SRV_REGISTER(regIndex))
#define FFX_OPTICALFLOW_DECLARE_UAV(regIndex)  register(DECLARE_UAV_REGISTER(regIndex))
#define FFX_OPTICALFLOW_DECLARE_CB(regIndex)   register(DECLARE_CB_REGISTER(regIndex))''',
                '''#define DECLARE_SAMPLER_REGISTER(regIndex)  s##regIndex
#define FFX_OPTICALFLOW_DECLARE_SRV(regIndex)  register(DECLARE_SRV_REGISTER(regIndex), space0)
#define FFX_OPTICALFLOW_DECLARE_SAMPLER(regIndex)  register(DECLARE_SAMPLER_REGISTER(regIndex), space0)
#define FFX_OPTICALFLOW_DECLARE_UAV(regIndex)  register(DECLARE_UAV_REGISTER(regIndex), space1)
#define FFX_OPTICALFLOW_DECLARE_CB(regIndex)   register(DECLARE_CB_REGISTER(regIndex), space2)
''' + FLOW_PACK)

    # --- The picture, through a sampler --------------------------------------------------------------------
    source.swap('''        Texture2D<FfxFloat32x4>                   r_input_color                       : FFX_OPTICALFLOW_DECLARE_SRV(FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR);''',
                '''        Texture2D<FfxFloat32x4>                   r_input_color                       : FFX_OPTICALFLOW_DECLARE_SRV(FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR);
        SamplerState                              s_r_input_color                     : FFX_OPTICALFLOW_DECLARE_SAMPLER(FFX_OPTICALFLOW_BIND_SRV_INPUT_COLOR);''')
    source.swap('''    return r_input_color[iPxHistory];''',
                '''    // The picture is the size the motion is found at.
    return r_input_color.SampleLevel(s_r_input_color, (FfxFloat32x2(iPxHistory) + 0.5f) / FfxFloat32x2(DisplaySize()), 0);''')

    # --- Formats; a motion packed into one integer ---------------------------------------------------------------
    formats(source, OF_FORMATS, OF_WIDENED)
    for name in OF_WIDENED:
        source.sub(r'^( *)' + name + r'\[([^\]]+)\] = ([^;]+);', r'\1' + name + r'[\2] = engPackFlow(\3);')
    source.swap('''    return rw_optical_flow[iPxPos];''', '''    return engUnpackFlow(rw_optical_flow[iPxPos]);''')
    for name in ('r_optical_flow', 'r_optical_flow_previous'):
        source.sub(r'Texture2D<FfxInt32x2>( +)' + name + r'( +):', r'Texture2D<FfxUInt32> \1' + name + r'\2:')
        source.swap('    return ' + name + '[iPxPos];', '    return engUnpackFlow(' + name + '[iPxPos]);')

    # --- Atomics ------------------------------------------------------------------------------------------------
    atomics(source, least=5)
    return source.text


# --- frame interpolation ------------------------------------------------------------------------------------

FI_SOURCE = UPSTREAM + 'frameinterpolation/ffx_frameinterpolation_callbacks_hlsl.h'
FI_OUTPUT = 'shaders/include/engine_fsr3_fi_callbacks.hlsli'

FI_HEAD = """// **The engine's callbacks for FSR 3's frame interpolation** (ADR 0165): how
// AMD's technique, which is its portable headers (third_party/fidelityfx_sdk,
// MIT), reads and writes its images through this engine's compute passes.
//
// **Generated** by `tools/repo/fsr3_callbacks.py` from AMD's own
// `ffx_frameinterpolation_callbacks_hlsl.h`, whose licence is above and whose
// every function is kept by name. Edit the script, not this file. What it
// changes:
//
//   - **The registers carry spaces**: what a compute shader reads is in space
//     0, what it writes in space 1, its constants in space 2 -- the RHI's
//     layout, where AMD's is Direct3D's bare registers.
//   - **A sampler a texture**, at the texture's own index, for every image of
//     floats: the RHI binds a texture with its sampler, and AMD's has one
//     sampler for all of them. A texel of one is read through that sampler,
//     at the texel's middle, where AMD's indexes the texture: a texture of
//     floats a shader loads from is the RHI's other kind of binding. Where
//     the middle is comes from the frame's constants. The images of integers
//     are loaded, as AMD loads them -- nothing samples an integer -- and are
//     that other kind, bound after the sampled ones.
//   - **Every image a pass writes says its format**, each one every device
//     stores to with no feature asked of it; one of two channels is declared
//     with four, because the compiler asks for an extension on seeing two.
//   - **Optical flow's motion is read packed**, one integer a block
//     (`engUnpackFlow`), as the engine's optical flow writes it.
//   - **An atomic on an image is one on Direct3D and Vulkan.** On Metal it is
//     read, changed and written (`ENG_SHADER_MSL`): the engine does not
//     generate frames there, and the passes are compiled for it because
//     every pass is.
//   - **The inpainting pass reads the picture the interpolation wrote from
//     that picture, and writes another** (`RWLoadFrameinterpolationOutput`):
//     AMD's reads and writes the one image through its write binding, which
//     for a picture of eight bits is a thing not every device allows.

"""

FI_REGISTERS = """#define COUNTER_FRAME_INDEX_SINCE_LAST_RESET 1

// The RHI's layout, in place of the bare registers `ffx_core_hlsl.h` gives.
#undef FFX_DECLARE_SRV
#undef FFX_DECLARE_UAV
#undef FFX_DECLARE_CB
#define DECLARE_SAMPLER_REGISTER(regIndex) s##regIndex
#define FFX_DECLARE_SRV(regIndex)     register(DECLARE_SRV_REGISTER(regIndex), space0)
#define FFX_DECLARE_SAMPLER(regIndex) register(DECLARE_SAMPLER_REGISTER(regIndex), space0)
#define FFX_DECLARE_UAV(regIndex)     register(DECLARE_UAV_REGISTER(regIndex), space1)
#define FFX_DECLARE_CB(regIndex)      register(DECLARE_CB_REGISTER(regIndex), space2)

// **A texel read is a sample at the texel's middle**, through the texture's
// own sampler: at a texel's middle a sampler gives that texel, exactly.
#define ENG_FI_LOAD(texture, at) texture.SampleLevel(s_##texture, engFiUv_##texture(at), 0)
"""

# Which of the frame's sizes each image of floats is: where a texel's middle is. 'own' asks the texture.
FI_SIZES = {
    'DisplaySize()': ['r_previous_interpolation_source', 'r_current_interpolation_source', 'r_present_backbuffer',
                      'r_output', 'r_inpainting_mask'],
    'RenderSize()': ['r_dilated_motion_vectors', 'r_dilated_depth', 'r_disocclusion_mask', 'r_input_depth',
                     'r_input_motion_vectors'],
    'own': ['r_optical_flow_upsampled', 'r_optical_flow_debug', 'r_input_distortion_field', 'r_inpainting_pyramid'],
}

FI_FORMATS = {
    'rw_output': 'rgba8',
    'rw_dilated_motion_vectors': 'rgba16f',
    'rw_dilated_depth': 'r32f',
    'rw_reconstructed_depth_previous_frame': 'r32ui',
    'rw_reconstructed_depth_interpolated_frame': 'r32ui',
    'rw_disocclusion_mask': 'rgba8',
    'rw_game_motion_vector_field_x': 'r32ui',
    'rw_game_motion_vector_field_y': 'r32ui',
    'rw_optical_flow_motion_vector_field_x': 'r32ui',
    'rw_optical_flow_motion_vector_field_y': 'r32ui',
}
FI_WIDENED = {'rw_dilated_motion_vectors': 'FfxFloat32x4', 'rw_disocclusion_mask': 'FfxFloat32x4'}


def derive_frame_interpolation(upstream):
    source = Source(upstream)
    cut = source.text.index('#include "ffx_frameinterpolation_resources.h"')
    source.text = source.text[:cut] + FI_HEAD + source.text[cut:]
    source.swap('#include "ffx_frameinterpolation_resources.h"',
                '#include "' + INCLUDE + 'frameinterpolation/ffx_frameinterpolation_resources.h"')
    source.swap('#include "ffx_core.h"', '#include "' + INCLUDE + 'ffx_core.h"')

    # --- The registers carry spaces; one sampler is none -------------------------------------------------
    source.swap('#define COUNTER_FRAME_INDEX_SINCE_LAST_RESET 1\n', FI_REGISTERS)
    source.swap('SamplerState s_LinearClamp : register(s0);\n', '')

    # --- A sampler a float texture, and the middle of a texel of it -----------------------------------------
    size_of = {name: size for size, names in FI_SIZES.items() for name in names}
    declared = set()

    def with_sampler(match):
        indent, name, index = match.group(1), match.group(3), match.group(4)
        declared.add(name)
        size = size_of[name]
        if size == 'own':
            middle_of = ('FfxUInt32 w;\n' + indent + '    FfxUInt32 h;\n' + indent + '    ' + name +
                         '.GetDimensions(w, h);\n' + indent + '    return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(w, h);')
        else:
            middle_of = 'return (FfxFloat32x2(at) + 0.5f) / FfxFloat32x2(' + size + ');'
        return (match.group(0) + '\n' + indent + 'SamplerState s_' + name + ' : FFX_DECLARE_SAMPLER(' + index + ');\n' +
                indent + 'FfxFloat32x2 engFiUv_' + name + '(FfxInt32x2 at)\n' + indent + '{\n' + indent + '    ' + middle_of +
                '\n' + indent + '}')

    source.sub(r'^( *)Texture2D<(FfxFloat32(?:x[234])?)> +(r_\w+) *: FFX_DECLARE_SRV\((\w+)\);', with_sampler, least=10)
    assert declared == set(size_of), sorted(declared ^ set(size_of))
    source.sub(r'(r_\w+)\.SampleLevel\(s_LinearClamp,', lambda m: '%s.SampleLevel(s_%s,' % (m.group(1), m.group(1)), least=5)
    assert 's_LinearClamp' not in source.text

    # --- The pyramid: a level of it, a texel of that ------------------------------------------------------------
    source.swap("""        return r_inpainting_pyramid.mips[mipLevel][iPxInput];""",
                """        FfxUInt32 w;
        FfxUInt32 h;
        FfxUInt32 levels;
        r_inpainting_pyramid.GetDimensions(mipLevel, w, h, levels);
        return r_inpainting_pyramid.SampleLevel(s_r_inpainting_pyramid, (FfxFloat32x2(iPxInput) + 0.5f) / FfxFloat32x2(w, h), mipLevel);""")

    # --- Every texel read of a float texture, through its sampler ------------------------------------------------
    floats = '|'.join(sorted(size_of, key=len, reverse=True))
    source.sub(r'(?<![\w.])(' + floats + r')\[([^\]]+)\]', r'ENG_FI_LOAD(\1, \2)', least=10)

    # --- Formats; two channels declared with four ------------------------------------------------------------------
    formats(source, FI_FORMATS, FI_WIDENED)
    for name in FI_WIDENED:
        source.sub(r'^( *)' + name + r'\[([^\]]+)\] = ([^;]+);', r'\1' + name + r'[\2] = FfxFloat32x4(\3, 0.0f, 0.0f);')
        source.sub(r'^( *)return ' + name + r'\[([^\]]+)\];', r'\1return ' + name + r'[\2].xy;')

    # --- The inpainting pass reads the interpolated picture from the picture -------------------------------------------
    source.swap("""    FfxFloat32x4 RWLoadFrameinterpolationOutput(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
        return rw_output[iPxPos];
    }""", """    FfxFloat32x4 RWLoadFrameinterpolationOutput(FFX_PARAMETER_IN FfxInt32x2 iPxPos)
    {
    #if defined(FFX_FRAMEINTERPOLATION_BIND_SRV_OUTPUT)
        // The image the pass before wrote, bound to be read; this pass writes
        // another (the head of this file).
        return LoadFrameInterpolationOutput(iPxPos);
    #else
        return rw_output[iPxPos];
    #endif
    }""")

    # --- Optical flow's motion is packed (the optical flow callbacks say why) -----------------------------------------
    source.swap('    Texture2D<FfxInt32x2> r_optical_flow : FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW);',
                """    Texture2D<FfxUInt32> r_optical_flow : FFX_DECLARE_SRV(FFX_FRAMEINTERPOLATION_BIND_SRV_OPTICAL_FLOW);
    // A motion in whole pixels, in one integer: x in the low sixteen bits, y
    // in the high, each signed (`engine_fsr3_of_callbacks.hlsli`).
    FfxInt32x2 engUnpackFlow(FfxUInt32 packed)
    {
        return FfxInt32x2(FfxInt32(packed << 16u) >> 16, FfxInt32(packed) >> 16);
    }""")
    source.swap('            return r_optical_flow[iPxPos] * GetOpticalFlowScale();',
                '            return engUnpackFlow(r_optical_flow[iPxPos]) * GetOpticalFlowScale();')

    # --- No confidence: nothing makes one ---------------------------------------------------------------------------------
    source.swap("""        return r_optical_flow_confidence[iPxPos].y;""",
                """        // Optical flow makes none, and AMD's runtime binds none: a texture
        // not bound reads nothing, which is what is said here.
        return 0.0f;""")

    # --- Atomics ---------------------------------------------------------------------------------------------------------
    atomics(source, least=8)
    return source.text


TARGETS = [(OF_SOURCE, OF_OUTPUT, derive_optical_flow), (FI_SOURCE, FI_OUTPUT, derive_frame_interpolation)]


def main():
    os.chdir(ROOT)
    check = '--check' in sys.argv[1:]
    stale = False
    for upstream_path, output, derive in TARGETS:
        upstream = open(upstream_path, encoding='utf-8').read().replace('\r\n', '\n')
        derived = derive(upstream)
        if check:
            held = open(output, encoding='utf-8').read() if os.path.exists(output) else ''
            if held != derived:
                print('fsr3_callbacks: ' + output + ' is not what the script derives; run it without --check')
                stale = True
            else:
                print('fsr3_callbacks: ' + output + ' is current')
            continue
        open(output, 'w', encoding='utf-8', newline='\n').write(derived)
        print('fsr3_callbacks: wrote ' + output + ', ' + str(len(derived.split('\n'))) + ' lines')
    return 1 if stale else 0


if __name__ == '__main__':
    sys.exit(main())
