// The Graph panel's seam (ADR 0211).
//
// Each open graph is a window beside the Viewport, as each open script is: the
// dockspace makes siblings in one node into a tab strip, and lets one be
// dragged out to stand beside the world. The panel draws the canvas, the
// fields beside it and the list a node is added from; it edits the document
// it is given and nothing else -- writing a file is asked for here and done
// by the frame loop.
//
// Declared unconditionally and inert in a shipping build (ADR 0011).
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "engine/app/node_canvas.h"
#include "engine/core/types.h"
#include "engine/rhi/types.h"

namespace engine::app {

class GraphEditor;
class SurfaceCompiler;
struct GraphDocument;

// **What is running**, for an animation graph's page: filled by the frame
// loop from the world, a copy -- a panel holds nothing a reload destroys.
struct GraphLive
{
    // The game is running: an animation graph is then looked at, not edited.
    bool playing = false;
    // The graph the rest is a reading of, as content names it
    // (`anim/hero.animgraph.json`); empty when nothing in the world runs one.
    std::string graph;

    struct Layer
    {
        std::string name;
        std::string state;
        core::f32 progress = 0.0f;
        core::f32 weight = 1.0f;
        // What it is still fading out of, each with its weight.
        std::vector<std::pair<std::string, core::f32>> fading;
    };
    std::vector<Layer> layers;

    struct Parameter
    {
        std::string name;
        core::f32 value = 0.0f;
        // Set on this machine over what the graph would read.
        bool overridden = false;
    };
    std::vector<Parameter> parameters;
};

// What the panel decided while it drew.
struct GraphPanelCommands
{
    std::optional<std::size_t> save;
    std::optional<std::size_t> close;

    [[nodiscard]] bool any() const noexcept { return save.has_value() || close.has_value(); }
};

// What compiles a material graph's shader, and the bytecode the device takes.
// With no compiler a graph is still drawn and written; only its compile
// errors are missing.
struct GraphShaders
{
    SurfaceCompiler* compiler = nullptr;
    rhi::ShaderFormat format = rhi::ShaderFormat::Unknown;
};

// Every open graph, each a window docked with the Viewport (`dockNode`, nought
// to leave them where they are).
void drawGraphEditor(GraphEditor& graphs, core::u32 dockNode, const GraphLive& live, const GraphShaders& shaders,
                     GraphPanelCommands& out);

// One graph's pane, in the window being drawn: what a test with no dock draws.
void drawGraphPane(GraphDocument& document, std::size_t index, const GraphLive& live, const GraphShaders& shaders,
                   GraphPanelCommands& out);

// The numbers a node's shape follows from in the panel, for the font it is
// drawn in: a material graph's nodes, or an animation graph's states.
[[nodiscard]] CanvasMetrics graphCanvasMetrics(bool arrows) noexcept;

// Whether a graph's window had the keyboard the last time one was drawn: the
// shell's own Delete, Ctrl+Z, Ctrl+C and Ctrl+S are then the graph's.
[[nodiscard]] bool graphPaneHasKeyboard() noexcept;

} // namespace engine::app
