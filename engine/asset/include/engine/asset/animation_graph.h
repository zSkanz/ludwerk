// **An animation graph as its file says it** (ADR 0197): parameters, layers of
// states and transitions, blends along one parameter or across two, and the
// named moments of a clip. This is the reader and the parsed form, and nothing
// that plays one -- `render::GraphPlayer` steps it, and whoever owns the clips
// binds them by the names kept here.
//
// **Reading is strict.** A graph is written by hand, and a key the reader does
// not know is far more often a slip than an extension: it is an error that
// names the key, like every other check here. What is validated is everything
// a later stage would otherwise trip on, so a graph that reads is one the
// evaluator can step without looking again.
//
// **No message is worded here.** An error is a code, where in the file it is
// and the name or value at fault; the caller words it (R3).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/types.h"

namespace engine::asset {

inline constexpr std::string_view AnimationGraphFormat = "animgraph";
inline constexpr core::i64 AnimationGraphVersion = 1;
// The compound suffix that makes a file a graph, wherever it sits.
inline constexpr std::string_view AnimationGraphSuffix = ".animgraph.json";

[[nodiscard]] bool isAnimationGraphPath(std::string_view path) noexcept;

enum class GraphParameterKind : core::u8
{
    Number,
    Boolean,
    // True from when it is fired until a transition takes it or the step ends.
    Trigger,
};

struct GraphParameter
{
    std::string name;
    GraphParameterKind kind = GraphParameterKind::Number;
    // Its value while nothing has set it. A boolean's is 1 or 0; a trigger's 0.
    core::f32 rest = 0.0f;
    // Where it comes from when nothing has set it, as written
    // (`CharacterBody.Speed`, `Attribute.Attack`), or empty. Only its shape is
    // checked here: what it names is the world's.
    std::string from;
};

// A named moment of a clip, by how far through the clip it is: 0 to 1.
struct GraphEvent
{
    core::f32 at = 0.0f;
    std::string name;
};

// **One use of a clip in a state.** A clip two states play is two of these,
// each with its own time when the graph is stepped.
struct GraphClip
{
    // As written: a clip's name in the graph's library, or `asset://file.glb#Name`.
    std::string clip;
    core::u32 layer = 0;
    core::u32 state = 0;
    // Where it sits in its state's blend: `at[0]` along one parameter, both
    // across two. Nought for a state that is one clip.
    core::f32 at[2] = {0.0f, 0.0f};
    // The file's `events` for this clip's name, sorted by `at`; those written
    // at one moment keep the file's order.
    std::vector<GraphEvent> events;
};

enum class GraphStateKind : core::u8
{
    // No clip: the layer under shows through.
    None,
    Clip,
    // A blend along one parameter.
    Blend1,
    // A blend across two.
    Blend2,
};

struct GraphState
{
    std::string name;
    GraphStateKind kind = GraphStateKind::None;
    bool loop = true;
    // A blend's clips share one phase, so clips of different lengths keep
    // their feet together.
    bool sync = false;
    // Never negative: a graph does not play a clip backwards.
    core::f32 speed = 1.0f;
    // The parameters a blend reads, as indices; -1 where there is none.
    core::i32 blendX = -1;
    core::i32 blendY = -1;
    // Its clips, as a run in `AnimationGraph::clips`. A `Blend1`'s are sorted
    // by `at[0]` ascending; a `Blend2`'s keep the file's order.
    core::u32 firstClip = 0;
    core::u32 clipCount = 0;
    // `Blend2` only: the Delaunay triangles of its clips' points, each three
    // indices relative to `firstClip`, counter-clockwise. Empty for fewer than
    // three points or points all on one line.
    std::vector<std::array<core::u32, 3>> triangles;
};

enum class GraphOp : core::u8
{
    // A lone name: a trigger that is fired, a boolean that is true, a number
    // that is not nought.
    IsSet,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

struct GraphCondition
{
    core::u32 parameter = 0;
    GraphOp op = GraphOp::IsSet;
    // What it is compared with; `true` is 1 and `false` 0.
    core::f32 value = 0.0f;
};

struct GraphTransition
{
    // The state it leaves, or -1 for `*`: any state but `to`.
    core::i32 from = -1;
    core::u32 to = 0;
    // Every one must hold. None is "always".
    std::vector<GraphCondition> when;
    // Not before this much of the state has played (1 is its end); -1 when the
    // file does not say.
    core::f32 after = -1.0f;
    // Seconds the two states are cross-faded over.
    core::f32 fade = 0.0f;
};

struct GraphLayer
{
    std::string name;
    // Joints, each with everything below it. Empty is the whole rig.
    std::vector<std::string> mask;
    bool additive = false;
    // 0 to 1, when `weightParameter` is -1; otherwise that parameter decides.
    core::f32 weight = 1.0f;
    core::i32 weightParameter = -1;
    core::u32 start = 0;
    std::vector<GraphState> states;
    std::vector<GraphTransition> transitions;
};

struct AnimationGraph
{
    // The file plainly named clips come from; empty is the mesh's own.
    std::string library;
    // Parameters, and a layer's states, keep the order the file wrote them in.
    std::vector<GraphParameter> parameters;
    // Bottom first.
    std::vector<GraphLayer> layers;
    // Every use of a clip, in layer, state and clip order.
    std::vector<GraphClip> clips;

    // The index of the parameter or the layer of that name, or -1.
    [[nodiscard]] core::i32 parameterNamed(std::string_view name) const noexcept;
    [[nodiscard]] core::i32 layerNamed(std::string_view name) const noexcept;
};

enum class GraphReadCode : core::u8
{
    None,
    // Not JSON at all. `what` is the parser's own diagnostic.
    Malformed,
    // Not an object, or its `format` is not a graph's.
    NotAGraph,
    // `what` is the version the file says.
    UnsupportedVersion,
    // `where` is the key's path, `what` the key.
    UnknownKey,
    // `where` is where the key should have been, `what` the key.
    MissingKey,
    // `what` is the shape that was expected there ("number", "string", ...).
    WrongType,
    // Two parameters, two layers, two states of one layer, or one key twice.
    DuplicateName,
    // A name, or a list that needs an entry, with nothing in it: no layer, a
    // layer with no state, a blend with no clip.
    Empty,
    UnknownParameter,
    UnknownState,
    // `events` names a clip no state plays.
    UnknownClip,
    // A blend along a parameter that is not a number, or a layer weighted by a
    // trigger. `what` is the parameter.
    ParameterKind,
    // A `from` that is neither `CharacterBody.<known>` nor `Attribute.<name>`.
    BadSource,
    // A condition that is neither `[name]` nor `[name, op, value]`; a state
    // that says both `clip` and `blend`; a parameter that is not exactly one
    // of `number`, `boolean` and `trigger`.
    BadShape,
    // `what` is the operator as written.
    BadOperator,
    // A number outside what the key allows: a negative fade, an event past the
    // end of its clip, a number no f32 holds. `what` is the number.
    OutOfRange,
};

struct GraphReadError
{
    GraphReadCode code = GraphReadCode::None;
    // A path into the file: `layers[1].transitions[0].to`, `parameters.Speed.from`.
    std::string where;
    // The name or value at fault.
    std::string what;
};

// The graph, or nothing and why. `error` is left alone on success.
[[nodiscard]] std::optional<AnimationGraph> readAnimationGraph(std::string_view json, GraphReadError* error = nullptr);

} // namespace engine::asset
