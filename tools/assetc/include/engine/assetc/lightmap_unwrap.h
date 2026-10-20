#pragma once

#include "engine/asset/model.h"

namespace engine::assetc {

// **Whether a mesh's lightmap coordinates fail it** (ADR 0253): missing for
// some vertex, outside the unit square, or two triangles over the same texel
// -- each a surface that would be given another's light.
[[nodiscard]] bool lightmapOverlaps(const asset::Mesh& mesh);

// **A lightmap's coordinates for a mesh that has none** (ADR 0253): its
// triangles charted and packed into the unit square with no two over the same
// texel, by xatlas, and a vertex on a chart's seam made one for each chart
// it is in. The triangles keep their order, so every submesh's range is what
// it was; a morph target's deltas follow the vertex they moved to each of its
// copies.
//
// A file's own set (`TEXCOORD_1`) is kept unless `lightmapOverlaps` fails it,
// and is then charted afresh. Nothing is done to a skinned mesh -- a
// character is lit as what moves is, never baked -- or to one with no
// triangles. True when the mesh was given new ones.
bool unwrapLightmap(asset::Model& model);

} // namespace engine::assetc
