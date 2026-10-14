#include "engine/assetc/exotic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "engine/asset/image.h"
#include "engine/core/i18n.h"

#if ENG_ASSETC_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#endif

namespace engine::assetc {
namespace {

// The extensions this tool claims. A CLOSED list rather than assimp's own
// registry, because the build chooses which importers are compiled in
// (third_party/CMakeLists.txt) and a file that classifies as a mesh and then
// fails to import is a worse error than one that rides through as raw.
constexpr std::array<std::string_view, 5> ExoticExtensions{".fbx", ".obj", ".dae", ".ply", ".stl"};

} // namespace

bool isExoticMesh(std::string_view extension) noexcept
{
    return std::find(ExoticExtensions.begin(), ExoticExtensions.end(), extension) != ExoticExtensions.end();
}

#if !ENG_ASSETC_ASSIMP

std::optional<core::EngineError> importExotic(std::span<const std::byte> bytes, const std::filesystem::path& directory,
                                              std::string_view extension, asset::Model& out)
{
    (void)bytes;
    (void)directory;
    (void)out;
    // Named rather than silent. A build with the importer switched off should
    // say which file it could not read and why, not treat a model as an opaque
    // blob and produce a pack that is quietly missing a mesh.
    const core::I18nArg args[] = {{"extension", std::string(extension)}};
    return core::makeError(ENG_TR("assetc.err.exotic_disabled"), args);
}

#else

namespace {

[[nodiscard]] core::Vec3 toVec3(const aiVector3D& value) noexcept
{
    return core::Vec3{value.x, value.y, value.z};
}

[[nodiscard]] core::Color3 toColor(const aiColor3D& value) noexcept
{
    return core::Color3{value.r, value.g, value.b};
}

// One assimp material, translated. What is NOT translated is as important as
// what is: assimp's material model is a superset of glTF's and the engine's is
// glTF's, so anything with no home here is dropped rather than approximated
// into the nearest field.
[[nodiscard]] asset::MaterialDef translateMaterial(const aiMaterial& source)
{
    asset::MaterialDef material;

    aiString name;
    if (source.Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
        material.name = name.C_Str();
    }

    aiColor3D colour{1.0f, 1.0f, 1.0f};
    if (source.Get(AI_MATKEY_BASE_COLOR, colour) == AI_SUCCESS) {
        material.baseColorFactor = toColor(colour);
    }
    else if (source.Get(AI_MATKEY_COLOR_DIFFUSE, colour) == AI_SUCCESS) {
        // A format with no PBR channel at all -- an OBJ, an old FBX -- puts its
        // albedo in the diffuse slot. Read as base colour, which is the closest
        // honest reading rather than an approximation of one.
        material.baseColorFactor = toColor(colour);
    }

    float value = 0.0f;
    if (source.Get(AI_MATKEY_METALLIC_FACTOR, value) == AI_SUCCESS) {
        material.metallicFactor = value;
    }
    else {
        // **Not one.** glTF's default is fully metallic, which is right for a
        // file that declares a PBR material and says nothing; a file with no
        // PBR model at all is describing a painted surface, and treating it as
        // metal makes every imported OBJ look like a mirror.
        material.metallicFactor = 0.0f;
    }
    if (source.Get(AI_MATKEY_ROUGHNESS_FACTOR, value) == AI_SUCCESS) {
        material.roughnessFactor = value;
    }

    aiColor3D emissive{0.0f, 0.0f, 0.0f};
    if (source.Get(AI_MATKEY_COLOR_EMISSIVE, emissive) == AI_SUCCESS) {
        material.emissiveFactor = toColor(emissive);
    }

    int twoSided = 0;
    if (source.Get(AI_MATKEY_TWOSIDED, twoSided) == AI_SUCCESS) {
        material.doubleSided = twoSided != 0;
    }

    float opacity = 1.0f;
    if (source.Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 1.0f) {
        material.baseColorAlpha = opacity;
        material.alphaMode = asset::AlphaMode::Blend;
    }
    return material;
}

// --- The skeleton ------------------------------------------------------------
//
// **One skeleton, in the shape the glTF importer leaves one** (asset/model.h):
// joints sorted parents first, each with a rigid rest transform against its
// parent, an inverse bind matrix, and vertices in the space those matrices
// agree on -- here the file's world, because a file with three skinned meshes
// under three differently placed nodes has no other space they share.
//
// **A joint's rest transform is rigid and a file's is not.** The pose is
// `composeTrs(position, rotation, animatedScale)` per joint (render/
// animation.cpp), so a rest SCALE -- which an FBX nearly always has, on the
// armature that converts centimetres -- has nowhere to go. Uniform scale
// commutes with rotation, so it is folded instead: every translation below a
// scaled node is multiplied by the scale above it, and the scale itself goes
// into the joint's inverse bind matrix, which a skin multiplies last anyway.
// A non-uniform rest scale is read as its average; it is rare on a skeleton
// and the alternative is refusing the file.
//
// **The nodes above the skeleton are folded into its roots** rather than made
// joints, which is where the file's axis conversion and armature live. They
// would cost palette slots (render/shader_types.h: sixty-four) and animate
// nothing.

// The renderer's palette budget (`render::kMaxSkinJoints`, which this tool
// cannot include). A rig that fits keeps every bone the file declares -- an
// unweighted one may be what a script attaches a sword to -- and one that does
// not keeps only the bones that move a vertex.
constexpr std::size_t PaletteBudget = 64;

[[nodiscard]] core::Mat4 toMat4(const aiMatrix4x4& source) noexcept
{
    // assimp's matrix is row-major (`a4` is the x translation) and `core::Mat4`
    // is `[column][row]`, so this is a transpose. The opposite assumption gives
    // a skin that looks right until something is off-axis.
    core::Mat4 result;
    for (unsigned int row = 0; row < 4; ++row) {
        for (unsigned int column = 0; column < 4; ++column)
            result.m[column][row] = source[row][column];
    }
    return result;
}

[[nodiscard]] aiMatrix4x4 globalOf(const aiNode* node) noexcept
{
    aiMatrix4x4 global;
    for (; node != nullptr; node = node->mParent)
        global = node->mTransformation * global;
    return global;
}

// A transform as its rigid part and one uniform scale.
struct Rigid
{
    aiVector3D position;
    aiQuaternion rotation;
    float scale = 1.0f;
};

[[nodiscard]] Rigid split(const aiMatrix4x4& matrix)
{
    Rigid out;
    aiVector3D scaling;
    matrix.Decompose(scaling, out.rotation, out.position);
    out.rotation.Normalize();
    out.scale = (std::abs(scaling.x) + std::abs(scaling.y) + std::abs(scaling.z)) / 3.0f;
    if (!(out.scale > 0.0f))
        out.scale = 1.0f;
    return out;
}

struct Skeleton
{
    std::vector<const aiNode*> nodes;
    std::map<std::string, core::u32> jointOf;
    // Per joint: the scale folded into everything below it, the joint's own
    // rest scale, and -- for a root -- the rigid transform of what is above it.
    std::vector<float> scaleBelow;
    std::vector<float> ownScale;
    std::vector<float> parentScale;
    std::vector<Rigid> above;
    std::vector<bool> root;

    [[nodiscard]] core::u32 find(const aiString& name) const
    {
        const auto found = jointOf.find(std::string(name.C_Str()));
        return found == jointOf.end() ? asset::Joint::NoParent : found->second;
    }
};

void collectJoints(const aiNode* node, const std::set<const aiNode*>& members, std::vector<const aiNode*>& order)
{
    // Pre-order over the file's own graph, which is what makes parents come
    // before children without a sort.
    if (members.contains(node))
        order.push_back(node);
    for (unsigned int child = 0; child < node->mNumChildren; ++child)
        collectJoints(node->mChildren[child], members, order);
}

void meshNodes(const aiNode* node, std::vector<const aiNode*>& out)
{
    for (unsigned int index = 0; index < node->mNumMeshes; ++index) {
        const unsigned int mesh = node->mMeshes[index];
        if (mesh < out.size() && out[mesh] == nullptr)
            out[mesh] = node;
    }
    for (unsigned int child = 0; child < node->mNumChildren; ++child)
        meshNodes(node->mChildren[child], out);
}

void readSkeleton(const aiScene& scene, const std::vector<const aiNode*>& meshNodeOf, asset::Model& out,
                  Skeleton& skeleton)
{
    std::set<const aiNode*> bones;
    std::set<const aiNode*> weighted;
    for (unsigned int meshIndex = 0; meshIndex < scene.mNumMeshes; ++meshIndex) {
        const aiMesh& mesh = *scene.mMeshes[meshIndex];
        for (unsigned int boneIndex = 0; boneIndex < mesh.mNumBones; ++boneIndex) {
            const aiBone& bone = *mesh.mBones[boneIndex];
            const aiNode* node = scene.mRootNode->FindNode(bone.mName);
            if (node == nullptr)
                continue;
            bones.insert(node);
            for (unsigned int weight = 0; weight < bone.mNumWeights; ++weight) {
                if (bone.mWeights[weight].mWeight > 0.0f) {
                    weighted.insert(node);
                    break;
                }
            }
        }
    }
    const std::set<const aiNode*>& kept = bones.size() <= PaletteBudget ? bones : weighted;

    // A node between two kept bones is a joint too -- the chain from one to the
    // other has to exist for the pose to reach the lower one -- and a node
    // above the topmost is not.
    std::set<const aiNode*> members = kept;
    for (const aiNode* node : kept) {
        std::vector<const aiNode*> path;
        for (const aiNode* up = node->mParent; up != nullptr; up = up->mParent) {
            if (kept.contains(up)) {
                members.insert(path.begin(), path.end());
                break;
            }
            path.push_back(up);
        }
    }
    collectJoints(scene.mRootNode, members, skeleton.nodes);

    const std::size_t count = skeleton.nodes.size();
    skeleton.scaleBelow.assign(count, 1.0f);
    skeleton.ownScale.assign(count, 1.0f);
    skeleton.parentScale.assign(count, 1.0f);
    skeleton.above.assign(count, Rigid{});
    skeleton.root.assign(count, false);
    out.joints.resize(count);

    for (core::u32 joint = 0; joint < count; ++joint) {
        const aiNode* node = skeleton.nodes[joint];
        skeleton.jointOf.emplace(std::string(node->mName.C_Str()), joint);

        const Rigid local = split(node->mTransformation);
        const auto parent = node->mParent == nullptr ? skeleton.jointOf.end()
                                                     : skeleton.jointOf.find(std::string(node->mParent->mName.C_Str()));
        aiVector3D position;
        aiQuaternion rotation;
        asset::Joint& target = out.joints[joint];
        target.name = node->mName.C_Str();
        skeleton.ownScale[joint] = local.scale;
        if (parent != skeleton.jointOf.end() && skeleton.nodes[parent->second] == node->mParent) {
            target.parent = parent->second;
            skeleton.parentScale[joint] = skeleton.scaleBelow[parent->second];
            position = local.position * skeleton.parentScale[joint];
            rotation = local.rotation;
        }
        else {
            // A root: everything above it, down to the file's own root node,
            // folded into its rest transform (and into its channels, below).
            const Rigid above = split(globalOf(node->mParent));
            skeleton.root[joint] = true;
            skeleton.above[joint] = above;
            skeleton.parentScale[joint] = above.scale;
            position = above.position + above.rotation.Rotate(local.position * above.scale);
            rotation = above.rotation * local.rotation;
        }
        rotation.Normalize();
        skeleton.scaleBelow[joint] = skeleton.parentScale[joint] * local.scale;
        target.localBind.position = core::DVec3{static_cast<core::f64>(position.x), static_cast<core::f64>(position.y),
                                                static_cast<core::f64>(position.z)};
        target.localBind.rotation = core::fromQuaternion(rotation.x, rotation.y, rotation.z, rotation.w);
    }

    // Inverse binds: the file's offset matrices are mesh space to bone space,
    // and the vertices are moved into the world below, so each is taken from
    // the world through the mesh's own node. A joint no bone names -- one in a
    // chain between two -- binds where it rests.
    std::vector<aiMatrix4x4> inverseBind(count);
    std::vector<bool> bound(count, false);
    for (unsigned int meshIndex = 0; meshIndex < scene.mNumMeshes; ++meshIndex) {
        const aiMesh& mesh = *scene.mMeshes[meshIndex];
        if (mesh.mNumBones == 0)
            continue;
        aiMatrix4x4 worldToMesh = globalOf(meshNodeOf[meshIndex]);
        worldToMesh.Inverse();
        for (unsigned int boneIndex = 0; boneIndex < mesh.mNumBones; ++boneIndex) {
            const core::u32 joint = skeleton.find(mesh.mBones[boneIndex]->mName);
            if (joint == asset::Joint::NoParent || bound[joint])
                continue;
            inverseBind[joint] = mesh.mBones[boneIndex]->mOffsetMatrix * worldToMesh;
            bound[joint] = true;
        }
    }

    out.sourceJointCount = static_cast<core::u32>(count);
    out.restPalette.resize(count);
    for (core::u32 joint = 0; joint < count; ++joint) {
        const aiMatrix4x4 global = globalOf(skeleton.nodes[joint]);
        if (!bound[joint]) {
            inverseBind[joint] = global;
            inverseBind[joint].Inverse();
        }
        aiMatrix4x4 folded;
        const float scale = skeleton.scaleBelow[joint];
        aiMatrix4x4::Scaling(aiVector3D(scale, scale, scale), folded);
        out.joints[joint].inverseBind = toMat4(folded * inverseBind[joint]);
        out.restPalette[joint] = toMat4(global * inverseBind[joint]);
    }
}

// The joint a vertex nothing weights follows: its mesh's nearest joint above
// it, so a prop parented to a hand stays in the hand, or the first root.
[[nodiscard]] core::u32 carrierOf(const aiNode* node, const Skeleton& skeleton)
{
    for (; node != nullptr; node = node->mParent) {
        const core::u32 joint = skeleton.find(node->mName);
        if (joint != asset::Joint::NoParent && skeleton.nodes[joint] == node)
            return joint;
    }
    return 0;
}

void readClips(const aiScene& scene, const Skeleton& skeleton, asset::Model& out)
{
    for (unsigned int animationIndex = 0; animationIndex < scene.mNumAnimations; ++animationIndex) {
        const aiAnimation& animation = *scene.mAnimations[animationIndex];
        // Ticks, not seconds. Zero means the file did not say, and assimp's
        // own documentation gives twenty-five as what that means.
        const double ticksPerSecond = animation.mTicksPerSecond > 0.0 ? animation.mTicksPerSecond : 25.0;

        asset::AnimationClip clip;
        clip.name = animation.mName.C_Str();
        // An exporter names a take after its armature ("Armature|Run"); the
        // part a script asks for is the one after the bar.
        if (const std::size_t bar = clip.name.rfind('|'); bar != std::string::npos)
            clip.name = clip.name.substr(bar + 1);

        const auto seconds = [&](double ticks) {
            const auto time = static_cast<core::f32>(ticks / ticksPerSecond);
            clip.duration = std::max(clip.duration, time);
            return time;
        };

        for (unsigned int channelIndex = 0; channelIndex < animation.mNumChannels; ++channelIndex) {
            const aiNodeAnim& channel = *animation.mChannels[channelIndex];
            const core::u32 joint = skeleton.find(channel.mNodeName);
            if (joint == asset::Joint::NoParent)
                continue;
            const bool root = skeleton.root[joint];
            const Rigid& above = skeleton.above[joint];

            // Every key goes through the same fold the rest pose did, or a
            // clip would play in centimetres on a skeleton built in metres.
            if (channel.mNumPositionKeys > 0) {
                asset::AnimationChannel keys;
                keys.joint = joint;
                keys.target = asset::AnimationChannel::Target::Translation;
                keys.stride = 3;
                for (unsigned int key = 0; key < channel.mNumPositionKeys; ++key) {
                    const aiVectorKey& source = channel.mPositionKeys[key];
                    const aiVector3D value = root ? above.position + above.rotation.Rotate(source.mValue * above.scale)
                                                  : source.mValue * skeleton.parentScale[joint];
                    keys.times.push_back(seconds(source.mTime));
                    keys.values.insert(keys.values.end(), {value.x, value.y, value.z});
                }
                clip.channels.push_back(std::move(keys));
            }
            if (channel.mNumRotationKeys > 0) {
                asset::AnimationChannel keys;
                keys.joint = joint;
                keys.target = asset::AnimationChannel::Target::Rotation;
                keys.stride = 4;
                for (unsigned int key = 0; key < channel.mNumRotationKeys; ++key) {
                    const aiQuatKey& source = channel.mRotationKeys[key];
                    aiQuaternion value = root ? above.rotation * source.mValue : source.mValue;
                    value.Normalize();
                    keys.times.push_back(seconds(source.mTime));
                    keys.values.insert(keys.values.end(), {value.x, value.y, value.z, value.w});
                }
                clip.channels.push_back(std::move(keys));
            }
            if (channel.mNumScalingKeys > 0) {
                // The rest scale is already in the skeleton, so a key says how
                // far from it the joint is.
                asset::AnimationChannel keys;
                keys.joint = joint;
                keys.target = asset::AnimationChannel::Target::Scale;
                keys.stride = 3;
                const float rest = skeleton.ownScale[joint];
                for (unsigned int key = 0; key < channel.mNumScalingKeys; ++key) {
                    const aiVectorKey& source = channel.mScalingKeys[key];
                    keys.times.push_back(seconds(source.mTime));
                    keys.values.insert(keys.values.end(),
                                       {source.mValue.x / rest, source.mValue.y / rest, source.mValue.z / rest});
                }
                clip.channels.push_back(std::move(keys));
            }
        }
        if (!clip.channels.empty())
            out.clips.push_back(std::move(clip));
    }
}

} // namespace

std::optional<core::EngineError> importExotic(std::span<const std::byte> bytes, const std::filesystem::path& directory,
                                              std::string_view extension, asset::Model& out)
{
    out = asset::Model{};

    Assimp::Importer importer;
    // An FBX's pivots as their own helper nodes (`$AssimpFbx$_Rotation`) would
    // put nodes between a bone and its parent that no clip animates, and the
    // clip's channel for the bone itself would then drive only part of it.
    importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);

    // **From MEMORY with the extension as a hint**, not from a path. The caller
    // already read the bytes -- content addressing means every file is read
    // once and hashed -- and handing assimp a path would read it a second time.
    //
    // The post-process set is chosen rather than inherited:
    //
    //   Triangulate         -- the engine draws triangles and nothing else.
    //   GenSmoothNormals    -- an OBJ often has none, and a mesh with no normals
    //                          renders black. Smooth rather than flat because a
    //                          flat-shaded import of a smooth model is a visible
    //                          downgrade and the reverse is not.
    //   CalcTangentSpace    -- the vertex layout carries a tangent and the
    //                          normal-map shader needs it.
    //   JoinIdenticalVertices -- these formats commonly store one vertex per
    //                          triangle corner; without this an OBJ cube is 36
    //                          vertices instead of 24.
    //   ImproveCacheLocality is deliberately OFF: `compileMesh` already runs
    //   meshoptimizer's own vertex-cache optimisation, and two reorderings is
    //   one wasted pass over every mesh.
    //   PreTransformVertices is deliberately OFF: it would bake the scene graph
    //   into one mesh, which is right for this pipeline (a `MeshPart` is one
    //   mesh) but loses the node names a skeleton needs. The flattening
    //   happens below, explicitly, where it can be read.
    const unsigned int flags = aiProcess_Triangulate | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
                               aiProcess_JoinIdenticalVertices | aiProcess_GenUVCoords |
                               aiProcess_ValidateDataStructure;

    const std::string hint(extension.empty() || extension.front() != '.' ? extension : extension.substr(1));
    const aiScene* scene = importer.ReadFileFromMemory(bytes.data(), bytes.size(), flags, hint.c_str());
    if (scene == nullptr || scene->mRootNode == nullptr) {
        const core::I18nArg args[] = {{"detail", std::string(importer.GetErrorString())}};
        return core::makeError(ENG_TR("assetc.err.exotic_import_failed"), args);
    }
    if (scene->mNumMeshes == 0) {
        return core::makeError(ENG_TR("assetc.err.exotic_no_geometry"));
    }

    out.materials.reserve(scene->mNumMaterials);
    for (unsigned int index = 0; index < scene->mNumMaterials; ++index) {
        out.materials.push_back(translateMaterial(*scene->mMaterials[index]));
    }
    if (out.materials.empty()) {
        // Every submesh names a material, so one has to exist. `Model`'s own
        // doc says the importer appends the default rather than leaving a draw
        // the renderer cannot make.
        out.materials.emplace_back();
    }

    // **Every mesh in the file becomes one submesh of one mesh**, which is what
    // a `MeshPart` is: one drawable thing. A file with a scene graph in it is
    // flattened, and its node TRANSFORMS are deliberately not applied -- these
    // formats put a model at the origin in its own space, and a file that does
    // not is a file whose author expected a scene importer rather than a mesh
    // importer. That is a different feature and nobody has asked for it.
    //
    // **A skinned file is the exception**: its bones are placed by the node
    // graph and its vertices have to be in the space the bones agree on, so
    // there every mesh moves into the file's world (`readSkeleton`).
    std::vector<const aiNode*> meshNodeOf(scene->mNumMeshes, nullptr);
    meshNodes(scene->mRootNode, meshNodeOf);
    bool skinned = false;
    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex)
        skinned = skinned || scene->mMeshes[meshIndex]->mNumBones > 0;
    Skeleton skeleton;
    if (skinned)
        readSkeleton(*scene, meshNodeOf, out, skeleton);
    skinned = skinned && !skeleton.nodes.empty();
    // Both default to the EMPTY box, so `expand` from a default is correct and
    // a mesh nobody filled cannot be mistaken for a point at the origin
    // (`core/math.h`).
    core::AABB bounds;

    for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh& mesh = *scene->mMeshes[meshIndex];
        if (mesh.mNumVertices == 0 || mesh.mFaces == nullptr) {
            continue;
        }

        asset::Submesh submesh;
        submesh.firstIndex = static_cast<core::u32>(out.mesh.indices.size());
        submesh.material = mesh.mMaterialIndex < out.materials.size() ? mesh.mMaterialIndex : 0u;

        const auto vertexBase = static_cast<core::u32>(out.mesh.vertices.size());
        core::AABB submeshBounds;

        // Into the world for a skinned file: points through the node's whole
        // transform, directions through its 3x3 (inverse-transposed for the
        // normal, so a scaled node does not bend its shading).
        const aiMatrix4x4 place = skinned ? globalOf(meshNodeOf[meshIndex]) : aiMatrix4x4{};
        const aiMatrix3x3 direction(place);
        aiMatrix3x3 normalDirection = direction;
        normalDirection.Inverse().Transpose();
        const auto moved = [&](const aiVector3D& value, const aiMatrix3x3& by) {
            aiVector3D result = by * value;
            return result.Normalize();
        };

        std::vector<asset::SkinVertex> influences;
        if (skinned) {
            influences.resize(mesh.mNumVertices);
            for (unsigned int boneIndex = 0; boneIndex < mesh.mNumBones; ++boneIndex) {
                const aiBone& bone = *mesh.mBones[boneIndex];
                const core::u32 joint = skeleton.find(bone.mName);
                if (joint == asset::Joint::NoParent)
                    continue;
                for (unsigned int weightIndex = 0; weightIndex < bone.mNumWeights; ++weightIndex) {
                    const aiVertexWeight& weight = bone.mWeights[weightIndex];
                    if (weight.mVertexId >= mesh.mNumVertices || !(weight.mWeight > 0.0f))
                        continue;
                    // The four heaviest, by replacing the lightest lane: a
                    // fifth influence is below what anyone can see (model.h).
                    asset::SkinVertex& skin = influences[weight.mVertexId];
                    std::size_t lightest = 0;
                    for (std::size_t lane = 1; lane < 4; ++lane) {
                        if (skin.weights[lane] < skin.weights[lightest])
                            lightest = lane;
                    }
                    if (weight.mWeight > skin.weights[lightest]) {
                        skin.weights[lightest] = weight.mWeight;
                        skin.joints[lightest] = static_cast<core::f32>(joint);
                    }
                }
            }
            const auto carrier = static_cast<core::f32>(carrierOf(meshNodeOf[meshIndex], skeleton));
            for (asset::SkinVertex& skin : influences) {
                const core::f32 total = skin.weights[0] + skin.weights[1] + skin.weights[2] + skin.weights[3];
                if (total > 0.0f) {
                    for (core::f32& weight : skin.weights)
                        weight /= total;
                }
                else {
                    skin = asset::SkinVertex{};
                    skin.joints[0] = carrier;
                    skin.weights[0] = 1.0f;
                }
            }
        }

        for (unsigned int index = 0; index < mesh.mNumVertices; ++index) {
            asset::Vertex vertex;
            vertex.position = toVec3(skinned ? place * mesh.mVertices[index] : mesh.mVertices[index]);
            if (mesh.mNormals != nullptr) {
                vertex.normal = toVec3(skinned ? moved(mesh.mNormals[index], normalDirection) : mesh.mNormals[index]);
            }
            if (mesh.mTangents != nullptr && mesh.mBitangents != nullptr) {
                const core::Vec3 tangent =
                    toVec3(skinned ? moved(mesh.mTangents[index], direction) : mesh.mTangents[index]);
                const core::Vec3 bitangent =
                    toVec3(skinned ? moved(mesh.mBitangents[index], direction) : mesh.mBitangents[index]);
                vertex.tangent[0] = tangent.x;
                vertex.tangent[1] = tangent.y;
                vertex.tangent[2] = tangent.z;
                // The handedness, derived the way glTF defines it: the sign of
                // the bitangent against `cross(normal, tangent)`. Storing a
                // constant here would flip the normal map on half the meshes in
                // any file that mirrors geometry.
                const core::Vec3 expected = core::cross(vertex.normal, tangent);
                vertex.tangent[3] = core::dot(expected, bitangent) < 0.0f ? -1.0f : 1.0f;
            }
            if (mesh.mTextureCoords[0] != nullptr) {
                vertex.uv[0] = mesh.mTextureCoords[0][index].x;
                vertex.uv[1] = mesh.mTextureCoords[0][index].y;
            }
            out.mesh.vertices.push_back(vertex);
            if (skinned)
                out.skin.push_back(influences[index]);

            core::expand(submeshBounds, vertex.position);
        }

        for (unsigned int face = 0; face < mesh.mNumFaces; ++face) {
            const aiFace& triangle = mesh.mFaces[face];
            // Triangulate ran, so anything else is a degenerate assimp kept --
            // a point or a line. Skipped rather than trusted: three indices is
            // what the rest of the pipeline assumes.
            if (triangle.mNumIndices != 3) {
                continue;
            }
            for (unsigned int corner = 0; corner < 3; ++corner) {
                out.mesh.indices.push_back(vertexBase + triangle.mIndices[corner]);
            }
        }

        submesh.indexCount = static_cast<core::u32>(out.mesh.indices.size()) - submesh.firstIndex;
        if (submesh.indexCount == 0) {
            // Nothing drawable came out, so the vertices are dead weight. Rolled
            // back rather than left in the buffer.
            out.mesh.vertices.resize(vertexBase);
            if (skinned)
                out.skin.resize(vertexBase);
            continue;
        }
        submesh.bounds = submeshBounds;
        out.mesh.submeshes.push_back(submesh);

        core::expand(bounds, submeshBounds);
    }

    if (out.mesh.submeshes.empty()) {
        return core::makeError(ENG_TR("assetc.err.exotic_no_geometry"));
    }
    out.mesh.bounds = bounds;
    if (skinned)
        readClips(*scene, skeleton, out);

    // Images are NOT imported. An FBX may embed textures and an OBJ names them
    // in an MTL beside it, and following either is a second resolution path
    // with its own rules about relative paths and its own failure modes. A
    // material's factors survive; its maps do not, and the model imports as an
    // untextured surface rather than as an error.
    //
    // Stated rather than left to be discovered, because "my textures did not
    // come through" is the first thing anybody will notice.
    (void)directory;
    return std::nullopt;
}

#endif

} // namespace engine::assetc
