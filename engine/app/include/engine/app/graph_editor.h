#pragma once

// **The graphs open in the editor** (ADR 0211): a material graph, which
// writes a surface shader beside itself, and an animation graph, which is the
// `.animgraph.json` a character already plays. Both are pictures on the one
// canvas (`node_canvas.h`), and this is what stands between that canvas and
// the files.
//
// A material graph IS its canvas: a node is a node, a link is a link, the
// canvas's own undo is the document's. An animation graph is not -- a state
// carries clips, an arrow carries conditions, a layer is a page -- so there
// the graph is what is kept and the canvas is a picture of one page of it,
// drawn again whenever the graph changes.
//
// **An edit to an animation graph is decided by the file's own reader.** The
// change is made to a copy, the copy is written and read back, and only a
// copy that reads is the graph. A state with no name, a blend along a
// trigger, a parameter two things are called: each is refused in the words
// the reader has for it, and the file on disk is never one the game could
// not read.
//
// Nothing here draws, and nothing here needs a window.

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/app/node_canvas.h"
#include "engine/asset/animation_graph.h"
#include "engine/asset/surface_graph.h"
#include "engine/core/types.h"

namespace engine::app {

enum class GraphKind : core::u8
{
    Surface,
    Animation,
};

// The words a list shows for a kind of node, given by whoever draws it: the
// model matches what is typed against them and never makes them up.
using GraphLabel = std::function<std::string(std::string_view kind)>;

// One line of the list a node is added from.
struct GraphChoice
{
    // What it adds: a material node's kind, or `State`, or `Any`.
    std::string kind;
    // The heading it is under.
    std::string group;
    // Where a link let go over nothing is being finished: the pin of the new
    // node it lands on. -1 where there is no such link.
    core::i32 pin = -1;
};

// The two things an animation graph's page has that are not material nodes.
inline constexpr std::string_view GraphStateNode = "State";
inline constexpr std::string_view GraphAnyNode = "Any";

// A link let go over nothing: which end is in hand.
struct GraphLoose
{
    core::u32 node = 0;
    core::u32 pin = 0;
    bool fromOutput = true;
};

// --- A material graph ----------------------------------------------------------

class SurfaceDocument
{
public:
    SurfaceDocument() = default;

    // Puts a graph's file on the canvas, every node under the id it had.
    void load(const asset::SurfaceGraph& graph, std::string_view graphFile);
    // The canvas as the file's graph.
    [[nodiscard]] asset::SurfaceGraph graph() const;
    // **After every change**: the shader is written again from what is on the
    // canvas, and each pin whose width follows from what is put into it is
    // told how wide it came out.
    void refresh();
    // The shader, the node each of its lines is, and what is wrong with the
    // graph as a shader.
    [[nodiscard]] const asset::WrittenSurface& written() const noexcept { return m_written; }
    // The node a line of the written shader is, first line 1; nought for a
    // line that is nobody's. Where a compiler's error is put.
    [[nodiscard]] core::u32 nodeOfLine(core::u32 line) const noexcept;

    // --- Edits: each is one step of the canvas's undo ---------------------------
    core::u32 addNode(std::string_view kind, CanvasPoint at);
    bool setField(core::u32 node, std::string_view name, std::string_view value);
    bool rename(core::u32 node, std::string_view title);
    core::u32 removeSelected();
    core::u32 duplicateSelected();
    bool undo();
    bool redo();
    // What a pointer did on the canvas, taken in: a link made or a node moved
    // changes the shader.
    void took(const CanvasEvent& event);

    // --- The list a node is added from ------------------------------------------
    // Every kind whose label or name has `filter` in it, whatever the case,
    // in the catalogue's order; with a link in hand, only those with a pin it
    // fits, and which pin.
    [[nodiscard]] std::vector<GraphChoice> choices(std::string_view filter, const GraphLoose* loose,
                                                   const GraphLabel& label) const;
    // Adds what was chosen, and makes the link that was in hand.
    core::u32 choose(const GraphChoice& choice, CanvasPoint at, const GraphLoose* loose);

    // --- Copy and paste, as text ------------------------------------------------
    // What is selected, as a graph's own file: empty when nothing is.
    [[nodiscard]] std::string copy() const;
    // Adds a copied graph's nodes about `at`, selected. How many; nought for
    // text that is not a material graph.
    core::u32 paste(std::string_view text, CanvasPoint at);

    NodeCanvas canvas;

private:
    std::string m_file;
    asset::WrittenSurface m_written;
};

// --- An animation graph --------------------------------------------------------

class AnimationDocument
{
public:
    AnimationDocument();

    void load(asset::AnimationGraph graph);
    [[nodiscard]] const asset::AnimationGraph& graph() const noexcept { return m_graph; }

    // The layer the canvas is a picture of.
    [[nodiscard]] core::usize page() const noexcept { return m_page; }
    void setPage(core::usize layer);

    // **Every edit.** False when the reader would not have what it made: the
    // graph is as it was, and `refusal` says where and why.
    bool edit(const std::function<void(asset::AnimationGraph&)>& change);
    [[nodiscard]] const asset::GraphReadError& refusal() const noexcept { return m_refusal; }
    void clearRefusal() noexcept { m_refusal = {}; }
    bool undo();
    bool redo();
    [[nodiscard]] bool canUndo() const noexcept { return m_undo.size() > 1; }
    [[nodiscard]] bool canRedo() const noexcept { return !m_redo.empty(); }
    [[nodiscard]] bool dirty() const noexcept { return !(m_graph == m_saved); }
    void markSaved() { m_saved = m_graph; }

    // --- The page's picture -----------------------------------------------------
    // The state a node of the canvas is, in the page's layer; nothing for the
    // box "from any" arrows leave, and for a node that is not there.
    [[nodiscard]] std::optional<core::u32> stateOf(core::u32 node) const noexcept;
    [[nodiscard]] core::u32 nodeOf(core::u32 state) const noexcept { return state + 1; }
    // The "from any" box's node, or nought when the page has none.
    [[nodiscard]] core::u32 anyNode() const noexcept { return m_anyNode; }
    // Whether a state can ever be come to: the start, one an arrow enters
    // from a state that can be, or one a "from any" arrow enters.
    [[nodiscard]] bool reachable(core::u32 state) const;
    // The page's transitions from one state to another, as indices into the
    // layer's own list, in its order. `from` is -1 for "from any".
    [[nodiscard]] std::vector<core::u32> transitionsBetween(core::i32 from, core::u32 to) const;

    // What a pointer did on the canvas, taken into the graph.
    void took(const CanvasEvent& event);

    // --- Edits by what they are for ---------------------------------------------
    // A state of a name nothing has, where the pointer is. Its index.
    std::optional<core::u32> addState(CanvasPoint at);
    bool addAny(CanvasPoint at);
    bool addTransition(core::i32 from, core::u32 to);
    // The states selected, or the arrow: with everything that named them.
    bool removeSelected();
    bool addLayer();
    bool removeLayer(core::usize layer);
    bool addParameter(asset::GraphParameterKind kind);
    // Refused, in words, for one a state or an arrow still reads.
    bool removeParameter(core::u32 parameter);

    [[nodiscard]] std::vector<GraphChoice> choices(std::string_view filter, const GraphLoose* loose,
                                                   const GraphLabel& label) const;
    bool choose(const GraphChoice& choice, CanvasPoint at, const GraphLoose* loose);

    // What is selected, as a graph's own file of one layer.
    [[nodiscard]] std::string copy() const;
    // Adds a copied page's states and the arrows among them, about `at`.
    core::u32 paste(std::string_view text, CanvasPoint at);

    NodeCanvas canvas;

private:
    void picture();

    asset::AnimationGraph m_graph;
    asset::AnimationGraph m_saved;
    std::vector<asset::AnimationGraph> m_undo;
    std::vector<asset::AnimationGraph> m_redo;
    asset::GraphReadError m_refusal;
    core::usize m_page = 0;
    core::u32 m_anyNode = 0;
};

// What an animation graph's edits are made of: the graph keeps every use of a
// clip in one run, and these keep that run true while a state gains a clip or
// a layer loses a state.
namespace graphedit {

// A state's clips, in its order.
[[nodiscard]] std::vector<asset::GraphClip> clipsOf(const asset::AnimationGraph& graph, core::usize layer,
                                                    core::u32 state);
void setClips(asset::AnimationGraph& graph, core::usize layer, core::u32 state, std::vector<asset::GraphClip> clips);
// What a state is: no clip, one, a blend along one parameter or across two.
// Its clips are kept where the new kind has room for them.
void setKind(asset::AnimationGraph& graph, core::usize layer, core::u32 state, asset::GraphStateKind kind);
void removeState(asset::AnimationGraph& graph, core::usize layer, core::u32 state);
void removeLayer(asset::AnimationGraph& graph, core::usize layer);
// Whether anything reads a parameter: a blend, a layer's weight, a condition.
[[nodiscard]] bool parameterUsed(const asset::AnimationGraph& graph, core::u32 parameter) noexcept;
void removeParameter(asset::AnimationGraph& graph, core::u32 parameter);
// A name nothing in `taken` has: `stem`, then `stem 2`, `stem 3`...
[[nodiscard]] std::string freeName(std::string_view stem, const std::vector<std::string>& taken);

} // namespace graphedit

// --- The open documents --------------------------------------------------------

// The list a node is being added from, while it is open.
struct GraphAsk
{
    bool open = false;
    // Where on the canvas what is chosen is put.
    CanvasPoint at;
    std::string filter;
    // The row the keys are on.
    core::u32 row = 0;
    std::optional<GraphLoose> loose;
};

struct GraphDocument
{
    GraphKind kind = GraphKind::Surface;
    std::filesystem::path file;
    // The file within the project's content, with `/` between the parts.
    std::string relative;
    std::string title;
    SurfaceDocument surface;
    AnimationDocument animation;
    GraphAsk ask;
    // What the last link that was refused was refused for, to be said beside
    // the pointer for a moment; and how many frames more.
    LinkRefusal refused = LinkRefusal::None;
    CanvasLink refusedLink;
    int refusedFrames = 0;
    // A running animation graph, as it was last read: the state each layer
    // was in, and the arrow just taken on the page shown, lit for a moment.
    std::vector<std::string> ranStates;
    CanvasLink taken;
    int takenFrames = 0;
    // The view has been fitted to the panel once.
    bool framed = false;
    // Where the canvas was last drawn, in the window's pixels: its top-left
    // corner and its size. What a place on the canvas is a place on the
    // screen by.
    CanvasPoint canvasAt;
    CanvasPoint canvasSize;
    // The list has just been shut: the keyboard goes back to the canvas it
    // was opened over, so Delete after adding a node is the canvas's.
    bool refocus = false;

    [[nodiscard]] NodeCanvas& canvas() noexcept
    {
        return kind == GraphKind::Surface ? surface.canvas : animation.canvas;
    }
    [[nodiscard]] const NodeCanvas& canvas() const noexcept
    {
        return kind == GraphKind::Surface ? surface.canvas : animation.canvas;
    }
    [[nodiscard]] bool dirty() const noexcept
    {
        return kind == GraphKind::Surface ? surface.canvas.dirty() : animation.dirty();
    }
};

class GraphEditor
{
public:
    // Whether a file is one this opens.
    [[nodiscard]] static bool opens(std::string_view path) noexcept;

    // Opens a graph's file, or goes to it where it is open already. Nothing,
    // and why in `error`, for a file that cannot be read as what its name
    // says.
    std::optional<core::usize> open(const std::filesystem::path& file, std::string_view relative,
                                    std::string* error = nullptr);
    // Writes the graph; a material graph writes its shader beside itself too.
    bool save(core::usize index);
    void close(core::usize index);

    [[nodiscard]] core::usize count() const noexcept { return m_documents.size(); }
    [[nodiscard]] GraphDocument* at(core::usize index) noexcept;
    [[nodiscard]] const GraphDocument* at(core::usize index) const noexcept;
    [[nodiscard]] std::optional<core::usize> active() const noexcept { return m_active; }
    void setActive(core::usize index) noexcept;
    // The document a panel should bring to the front, once.
    [[nodiscard]] std::optional<core::usize> takeFocusRequest() noexcept;

    // What a new file of each kind holds: a graph that is one already.
    [[nodiscard]] static std::string newSurfaceGraph();
    [[nodiscard]] static std::string newAnimationGraph();

private:
    // Held by pointer: a panel keeps one across the frame another is opened in.
    std::vector<std::unique_ptr<GraphDocument>> m_documents;
    std::optional<core::usize> m_active;
    std::optional<core::usize> m_focus;
};

} // namespace engine::app
