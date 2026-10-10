// Roles from names, and the map between two rigs (ADR 0199).
//
// The first half of this file is DATA: what a role is called, the names rigs
// arrive with, the words that mark a joint as no part of the body, and which
// joint a bone runs to. A rig that is not recognised is one line in a table
// here, or a `.rig.json` beside the model.
#include "engine/render/retarget.h"

#include <algorithm>
#include <cmath>

#include "engine/core/json.h"

namespace engine::render::retarget {

namespace {

using core::f32;
using core::f64;
using core::i32;
using core::u32;
using core::u8;
using core::usize;

enum class Side : u8
{
    None,
    Left,
    Right,
};

// A role with its side taken off, which is what a name says once its side has
// been read out of it. The last two are words that mean one of two parts
// depending on what else the rig has (see `Aliases`).
enum class Part : u8
{
    None,
    Hips,
    Spine,
    Chest,
    UpperChest,
    Neck,
    Head,
    Shoulder,
    UpperArm,
    LowerArm,
    Hand,
    UpperLeg,
    LowerLeg,
    Foot,
    Toes,
    Thumb1,
    Thumb2,
    Thumb3,
    Index1,
    Index2,
    Index3,
    Middle1,
    Middle2,
    Middle3,
    Ring1,
    Ring2,
    Ring3,
    Little1,
    Little2,
    Little3,
    ShoulderWord,
    LegWord,
};

// --- The roles -----------------------------------------------------------------

struct RoleRow
{
    std::string_view name;
    Part part;
    Side side;
};

// In `Role`'s own order, which the first test holds it to.
constexpr std::array<RoleRow, RoleCount> Roles{{
    {"None", Part::None, Side::None},
    {"Hips", Part::Hips, Side::None},
    {"Spine", Part::Spine, Side::None},
    {"Chest", Part::Chest, Side::None},
    {"UpperChest", Part::UpperChest, Side::None},
    {"Neck", Part::Neck, Side::None},
    {"Head", Part::Head, Side::None},
    {"LeftShoulder", Part::Shoulder, Side::Left},
    {"LeftUpperArm", Part::UpperArm, Side::Left},
    {"LeftLowerArm", Part::LowerArm, Side::Left},
    {"LeftHand", Part::Hand, Side::Left},
    {"RightShoulder", Part::Shoulder, Side::Right},
    {"RightUpperArm", Part::UpperArm, Side::Right},
    {"RightLowerArm", Part::LowerArm, Side::Right},
    {"RightHand", Part::Hand, Side::Right},
    {"LeftUpperLeg", Part::UpperLeg, Side::Left},
    {"LeftLowerLeg", Part::LowerLeg, Side::Left},
    {"LeftFoot", Part::Foot, Side::Left},
    {"LeftToes", Part::Toes, Side::Left},
    {"RightUpperLeg", Part::UpperLeg, Side::Right},
    {"RightLowerLeg", Part::LowerLeg, Side::Right},
    {"RightFoot", Part::Foot, Side::Right},
    {"RightToes", Part::Toes, Side::Right},
    {"LeftThumb1", Part::Thumb1, Side::Left},
    {"LeftThumb2", Part::Thumb2, Side::Left},
    {"LeftThumb3", Part::Thumb3, Side::Left},
    {"LeftIndex1", Part::Index1, Side::Left},
    {"LeftIndex2", Part::Index2, Side::Left},
    {"LeftIndex3", Part::Index3, Side::Left},
    {"LeftMiddle1", Part::Middle1, Side::Left},
    {"LeftMiddle2", Part::Middle2, Side::Left},
    {"LeftMiddle3", Part::Middle3, Side::Left},
    {"LeftRing1", Part::Ring1, Side::Left},
    {"LeftRing2", Part::Ring2, Side::Left},
    {"LeftRing3", Part::Ring3, Side::Left},
    {"LeftLittle1", Part::Little1, Side::Left},
    {"LeftLittle2", Part::Little2, Side::Left},
    {"LeftLittle3", Part::Little3, Side::Left},
    {"RightThumb1", Part::Thumb1, Side::Right},
    {"RightThumb2", Part::Thumb2, Side::Right},
    {"RightThumb3", Part::Thumb3, Side::Right},
    {"RightIndex1", Part::Index1, Side::Right},
    {"RightIndex2", Part::Index2, Side::Right},
    {"RightIndex3", Part::Index3, Side::Right},
    {"RightMiddle1", Part::Middle1, Side::Right},
    {"RightMiddle2", Part::Middle2, Side::Right},
    {"RightMiddle3", Part::Middle3, Side::Right},
    {"RightRing1", Part::Ring1, Side::Right},
    {"RightRing2", Part::Ring2, Side::Right},
    {"RightRing3", Part::Ring3, Side::Right},
    {"RightLittle1", Part::Little1, Side::Right},
    {"RightLittle2", Part::Little2, Side::Right},
    {"RightLittle3", Part::Little3, Side::Right},
}};

// --- The names rigs arrive with --------------------------------------------------
//
// Every name below is compared with a joint's name AFTER that has been
// normalised (`parseName`): its rig's prefix off, lower case, separators out,
// its side read and taken out, and a number that stood on its own
// (`spine_01`, `thumb.01.L`) without its leading zeros.

// What a rig puts in front of every joint, compared without case. Everything
// up to the last `:` or `|` goes before these are tried, which is the
// namespace an exporter writes whatever it is called.
//
// The avatar format's rigs say `J_Bip_` and then where the joint is -- `C_`
// for the middle, `L_` and `R_` for the sides -- so the middle's goes with the
// prefix and a side's is left to be read as any side is. Longest first: one
// that begins another is tried before it.
constexpr std::string_view Prefixes[] = {
    "mixamorig", "bip001", "bip01", "def-", "org-", "armature_", "character1_", "rig_", "j_bip_c_", "j_bip_",
};

// A joint with one of these as a word of its name is a helper beside the body
// and never has a role: the twist and roll joints along a limb, the handles of
// a control rig, the tips an exporter closes a chain with.
constexpr std::string_view HelperWords[] = {
    "twist", "roll", "ik", "pole", "ctrl", "control", "helper", "end", "nub",
};

struct Alias
{
    std::string_view name;
    Part part;
    // Whether the name is a limb's, and so means something only with a side;
    // one that is not means something only without. `hip` is both: the hips
    // of a rig, and the top of one leg of another.
    bool sided;
    // Whether the name also comes with a number after it: `spine`, `spine1`,
    // `spine_03`. Where several joints then say one thing, which is which is
    // settled by where they stand in the rig.
    bool numbered;
};

// Every joint of a back says `Part::Spine` here, whatever it is called; which
// of them is the spine, the chest and the upper chest is read off their order
// from the hips up (`resolveSpine`).
//
// Two words mean two things. `shoulder` is the collar bone where the rig has
// another joint for the upper arm, and the upper arm where it has none (a rig
// named joint by joint: shoulder, elbow, wrist). `leg` is the lower leg where
// another joint is the upper leg (`UpLeg` and `Leg`), and the upper leg where
// none is (`Leg` and `Shin`).
constexpr Alias Aliases[] = {
    {"hips", Part::Hips, false, false},
    {"hip", Part::Hips, false, false},
    {"pelvis", Part::Hips, false, false},

    {"spine", Part::Spine, false, true},
    {"chest", Part::Spine, false, true},
    {"upperchest", Part::Spine, false, false},
    {"lowerchest", Part::Spine, false, false},
    {"spinelower", Part::Spine, false, false},
    {"spinemid", Part::Spine, false, false},
    {"spinemiddle", Part::Spine, false, false},
    {"spineupper", Part::Spine, false, false},
    {"lowerspine", Part::Spine, false, false},
    {"middlespine", Part::Spine, false, false},
    {"upperspine", Part::Spine, false, false},
    {"torso", Part::Spine, false, false},
    {"abdomen", Part::Spine, false, false},

    {"neck", Part::Neck, false, true},
    {"head", Part::Head, false, false},

    {"clavicle", Part::Shoulder, true, false},
    {"collar", Part::Shoulder, true, false},
    {"collarbone", Part::Shoulder, true, false},
    {"scapula", Part::Shoulder, true, false},
    {"shoulder", Part::ShoulderWord, true, false},

    {"upperarm", Part::UpperArm, true, false},
    {"arm", Part::UpperArm, true, false},
    {"uparm", Part::UpperArm, true, false},
    {"armupper", Part::UpperArm, true, false},

    {"lowerarm", Part::LowerArm, true, false},
    {"forearm", Part::LowerArm, true, false},
    // A rig of a dozen joints named in one short word each: `ArmL`, `ForeL`,
    // `HandL`.
    {"fore", Part::LowerArm, true, false},
    {"elbow", Part::LowerArm, true, false},
    {"loarm", Part::LowerArm, true, false},
    {"armlower", Part::LowerArm, true, false},

    {"hand", Part::Hand, true, false},
    {"wrist", Part::Hand, true, false},

    {"upperleg", Part::UpperLeg, true, false},
    {"upleg", Part::UpperLeg, true, false},
    {"thigh", Part::UpperLeg, true, false},
    {"hip", Part::UpperLeg, true, false},
    {"legupper", Part::UpperLeg, true, false},
    {"leg", Part::LegWord, true, false},

    {"lowerleg", Part::LowerLeg, true, false},
    {"shin", Part::LowerLeg, true, false},
    {"calf", Part::LowerLeg, true, false},
    {"knee", Part::LowerLeg, true, false},
    {"loleg", Part::LowerLeg, true, false},
    {"leglower", Part::LowerLeg, true, false},

    {"foot", Part::Foot, true, false},
    {"ankle", Part::Foot, true, false},

    {"toe", Part::Toes, true, true},
    {"toes", Part::Toes, true, false},
    {"toebase", Part::Toes, true, false},
    {"ball", Part::Toes, true, false},

    // The 3ds-style biped numbers its fingers and then their joints: the
    // thumb is `Finger0`, `Finger01`, `Finger02`, the first finger `Finger1`,
    // `Finger11`, `Finger12`. Which is why a number glued to a word keeps its
    // zeros: `Finger01` is not `Finger1`.
    {"finger0", Part::Thumb1, true, false},
    {"finger01", Part::Thumb2, true, false},
    {"finger02", Part::Thumb3, true, false},
    {"finger1", Part::Index1, true, false},
    {"finger11", Part::Index2, true, false},
    {"finger12", Part::Index3, true, false},
    {"finger2", Part::Middle1, true, false},
    {"finger21", Part::Middle2, true, false},
    {"finger22", Part::Middle3, true, false},
    {"finger3", Part::Ring1, true, false},
    {"finger31", Part::Ring2, true, false},
    {"finger32", Part::Ring3, true, false},
    {"finger4", Part::Little1, true, false},
    {"finger41", Part::Little2, true, false},
    {"finger42", Part::Little3, true, false},
};

// A finger's name, to which its joint is added: a number from 1 to 3
// (`thumb1`, `handindex2`, `findex3`) or one of `FingerJoints`' words. The
// name with nothing added is the first joint, as a rig that numbers only the
// joints after it has it: `Thumb`, `Thumb2`.
struct FingerName
{
    std::string_view name;
    Part first;
};

constexpr FingerName FingerNames[] = {
    {"thumb", Part::Thumb1},         {"handthumb", Part::Thumb1},     {"fthumb", Part::Thumb1},
    {"thumbfinger", Part::Thumb1},   {"fingerthumb", Part::Thumb1},

    {"index", Part::Index1},         {"handindex", Part::Index1},     {"findex", Part::Index1},
    {"indexfinger", Part::Index1},   {"fingerindex", Part::Index1},   {"pointer", Part::Index1},

    {"middle", Part::Middle1},       {"handmiddle", Part::Middle1},   {"fmiddle", Part::Middle1},
    {"middlefinger", Part::Middle1}, {"fingermiddle", Part::Middle1},

    {"ring", Part::Ring1},           {"handring", Part::Ring1},       {"fring", Part::Ring1},
    {"ringfinger", Part::Ring1},     {"fingerring", Part::Ring1},

    {"little", Part::Little1},       {"handlittle", Part::Little1},   {"flittle", Part::Little1},
    {"littlefinger", Part::Little1}, {"fingerlittle", Part::Little1}, {"pinky", Part::Little1},
    {"handpinky", Part::Little1},    {"fpinky", Part::Little1},       {"pinkyfinger", Part::Little1},
    {"fingerpinky", Part::Little1},  {"pinkie", Part::Little1},
};

struct FingerJoint
{
    std::string_view word;
    u8 joint;
};

constexpr FingerJoint FingerJoints[] = {
    {"proximal", 0},
    {"intermediate", 1},
    {"distal", 2},
};

// Which joint a bone runs to, for the stance: the first of these that both
// rigs have. Nearest first, so a rig with a shorter back is compared over the
// same stretch of it. A hand and a foot have none -- which way a hand's
// fingers or a foot's toes rest is the body's build (a heel) and not its
// stance.
struct Bone
{
    Part from;
    Part to[4];
};

constexpr Bone Bones[] = {
    {Part::Hips, {Part::Spine, Part::Chest, Part::UpperChest, Part::None}},
    {Part::Spine, {Part::Chest, Part::UpperChest, Part::Neck, Part::Head}},
    {Part::Chest, {Part::UpperChest, Part::Neck, Part::Head, Part::None}},
    {Part::UpperChest, {Part::Neck, Part::Head, Part::None, Part::None}},
    {Part::Neck, {Part::Head, Part::None, Part::None, Part::None}},
    {Part::Shoulder, {Part::UpperArm, Part::None, Part::None, Part::None}},
    {Part::UpperArm, {Part::LowerArm, Part::None, Part::None, Part::None}},
    {Part::LowerArm, {Part::Hand, Part::None, Part::None, Part::None}},
    {Part::UpperLeg, {Part::LowerLeg, Part::None, Part::None, Part::None}},
    {Part::LowerLeg, {Part::Foot, Part::None, Part::None, Part::None}},
    {Part::Thumb1, {Part::Thumb2, Part::None, Part::None, Part::None}},
    {Part::Thumb2, {Part::Thumb3, Part::None, Part::None, Part::None}},
    {Part::Index1, {Part::Index2, Part::None, Part::None, Part::None}},
    {Part::Index2, {Part::Index3, Part::None, Part::None, Part::None}},
    {Part::Middle1, {Part::Middle2, Part::None, Part::None, Part::None}},
    {Part::Middle2, {Part::Middle3, Part::None, Part::None, Part::None}},
    {Part::Ring1, {Part::Ring2, Part::None, Part::None, Part::None}},
    {Part::Ring2, {Part::Ring3, Part::None, Part::None, Part::None}},
    {Part::Little1, {Part::Little2, Part::None, Part::None, Part::None}},
    {Part::Little2, {Part::Little3, Part::None, Part::None, Part::None}},
};

// --- Reading a name --------------------------------------------------------------

[[nodiscard]] constexpr bool isSeparator(char c) noexcept
{
    return c == ' ' || c == '_' || c == '-' || c == '.';
}

[[nodiscard]] constexpr bool isUpper(char c) noexcept
{
    return c >= 'A' && c <= 'Z';
}

[[nodiscard]] constexpr bool isLower(char c) noexcept
{
    return c >= 'a' && c <= 'z';
}

[[nodiscard]] constexpr bool isDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// ASCII only, deliberately: the tables are ASCII, and a locale's idea of case
// is exactly what must not decide a role on one machine and not on another.
[[nodiscard]] constexpr char lower(char c) noexcept
{
    return isUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}

[[nodiscard]] bool startsWithNoCase(std::string_view text, std::string_view prefix) noexcept
{
    if (text.size() < prefix.size())
        return false;
    for (usize index = 0; index < prefix.size(); ++index) {
        if (lower(text[index]) != prefix[index])
            return false;
    }
    return true;
}

[[nodiscard]] Side sideOfLetter(char c) noexcept
{
    const char letter = lower(c);
    return letter == 'l' ? Side::Left : (letter == 'r' ? Side::Right : Side::None);
}

// A joint's name with its rig's own prefix off.
[[nodiscard]] std::string_view stemOf(std::string_view name) noexcept
{
    if (const usize cut = name.find_last_of(":|"); cut != std::string_view::npos)
        name.remove_prefix(cut + 1);
    for (bool stripped = true; stripped;) {
        stripped = false;
        while (!name.empty() && isSeparator(name.front()))
            name.remove_prefix(1);
        for (const std::string_view prefix : Prefixes) {
            if (startsWithNoCase(name, prefix)) {
                name.remove_prefix(prefix.size());
                stripped = true;
                break;
            }
        }
    }
    while (!name.empty() && isSeparator(name.back()))
        name.remove_suffix(1);
    return name;
}

// Whether one of the words of `stem` marks a helper. A word ends at a
// separator, where letters meet digits, and where a capital starts one
// (`ForeTwist`, `IKFoot`, `HeadTop_End`).
[[nodiscard]] bool hasHelperWord(std::string_view stem)
{
    std::string word;
    const auto helper = [&word]() {
        const bool found = std::find(std::begin(HelperWords), std::end(HelperWords), word) != std::end(HelperWords);
        word.clear();
        return found;
    };
    for (usize index = 0; index < stem.size(); ++index) {
        const char c = stem[index];
        if (isSeparator(c)) {
            if (helper())
                return true;
            continue;
        }
        if (!word.empty()) {
            const char before = stem[index - 1];
            const bool nextLower = index + 1 < stem.size() && isLower(stem[index + 1]);
            const bool starts = (isDigit(c) != isDigit(before)) || (isUpper(c) && isLower(before)) ||
                                (isUpper(c) && isUpper(before) && nextLower);
            if (starts && helper())
                return true;
        }
        word.push_back(lower(c));
    }
    return helper();
}

struct ParsedName
{
    // What is looked up: lower case, no separators, no side.
    std::string key;
    Side side = Side::None;
    bool helper = false;
};

[[nodiscard]] ParsedName parseName(std::string_view name)
{
    ParsedName out;
    std::string_view stem = stemOf(name);
    out.helper = hasHelperWord(stem);

    // The pieces between separators.
    std::vector<std::string_view> tokens;
    for (usize index = 0; index < stem.size();) {
        while (index < stem.size() && isSeparator(stem[index]))
            ++index;
        const usize from = index;
        while (index < stem.size() && !isSeparator(stem[index]))
            ++index;
        if (index > from)
            tokens.push_back(stem.substr(from, index - from));
    }
    if (tokens.empty())
        return out;

    // A side said as one letter, set off at either end: by a separator
    // (`upperarm_l`, `L_UpperArm`, `thigh.L`, `Bip01 L Thigh`), or by its case
    // (`lHand`, `HandL`).
    if (tokens.size() >= 2 && tokens.front().size() == 1 && sideOfLetter(tokens.front()[0]) != Side::None) {
        out.side = sideOfLetter(tokens.front()[0]);
        tokens.erase(tokens.begin());
    }
    else if (tokens.size() >= 2 && tokens.back().size() == 1 && sideOfLetter(tokens.back()[0]) != Side::None) {
        out.side = sideOfLetter(tokens.back()[0]);
        tokens.pop_back();
    }
    else if (const std::string_view first = tokens.front();
             first.size() >= 2 && isLower(first[0]) && isUpper(first[1]) && sideOfLetter(first[0]) != Side::None) {
        out.side = sideOfLetter(first[0]);
        tokens.front().remove_prefix(1);
    }
    else if (const std::string_view last = tokens.back();
             last.size() >= 2 && isUpper(last.back()) && sideOfLetter(last.back()) != Side::None &&
             (isLower(last[last.size() - 2]) || isDigit(last[last.size() - 2]))) {
        out.side = sideOfLetter(last.back());
        tokens.back().remove_suffix(1);
    }

    for (std::string_view token : tokens) {
        // A number on its own is the same number however many zeros lead it.
        if (std::all_of(token.begin(), token.end(), isDigit)) {
            while (token.size() > 1 && token.front() == '0')
                token.remove_prefix(1);
        }
        for (const char c : token)
            out.key.push_back(lower(c));
    }

    // A side said as a word, in front or behind (`LeftArm`, `hand_right`).
    if (out.side == Side::None) {
        constexpr std::string_view Left = "left";
        constexpr std::string_view Right = "right";
        const std::string_view key = out.key;
        if (key.size() > Left.size() && key.starts_with(Left)) {
            out.side = Side::Left;
            out.key.erase(0, Left.size());
        }
        else if (key.size() > Right.size() && key.starts_with(Right)) {
            out.side = Side::Right;
            out.key.erase(0, Right.size());
        }
        else if (key.size() > Left.size() && key.ends_with(Left)) {
            out.side = Side::Left;
            out.key.erase(out.key.size() - Left.size());
        }
        else if (key.size() > Right.size() && key.ends_with(Right)) {
            out.side = Side::Right;
            out.key.erase(out.key.size() - Right.size());
        }
    }
    return out;
}

// The part a parsed name says, or `None`.
[[nodiscard]] Part partOf(const ParsedName& parsed) noexcept
{
    if (parsed.helper || parsed.key.empty())
        return Part::None;
    const std::string_view key = parsed.key;
    const bool sided = parsed.side != Side::None;

    for (const Alias& alias : Aliases) {
        if (alias.sided == sided && alias.name == key)
            return alias.part;
    }

    usize letters = key.size();
    while (letters > 0 && isDigit(key[letters - 1]))
        --letters;
    const std::string_view base = key.substr(0, letters);
    const std::string_view digits = key.substr(letters);

    if (!digits.empty() && !base.empty()) {
        for (const Alias& alias : Aliases) {
            if (alias.numbered && alias.sided == sided && alias.name == base)
                return alias.part;
        }
    }

    if (!sided)
        return Part::None;
    for (const FingerName& finger : FingerNames) {
        if (finger.name == key)
            return finger.first;
        if (!digits.empty() && finger.name == base) {
            u32 number = 0;
            for (const char c : digits)
                number = number < 100 ? number * 10 + static_cast<u32>(c - '0') : number;
            if (number >= 1 && number <= 3)
                return static_cast<Part>(static_cast<u32>(finger.first) + number - 1);
            return Part::None;
        }
        if (key.size() > finger.name.size() && key.starts_with(finger.name)) {
            const std::string_view word = key.substr(finger.name.size());
            for (const FingerJoint& joint : FingerJoints) {
                if (joint.word == word)
                    return static_cast<Part>(static_cast<u32>(finger.first) + static_cast<u32>(joint.joint));
            }
        }
    }
    return Part::None;
}

[[nodiscard]] Role roleOf(Part part, Side side) noexcept
{
    if (part == Part::None)
        return Role::None;
    for (usize index = 1; index < Roles.size(); ++index) {
        if (Roles[index].part == part && Roles[index].side == side)
            return static_cast<Role>(index);
    }
    return Role::None;
}

// --- Roles from structure ----------------------------------------------------------

struct Claim
{
    Part part = Part::None;
    Side side = Side::None;
};

[[nodiscard]] bool hasParent(std::span<const asset::Joint> joints, usize joint) noexcept
{
    // A parent is always earlier (asset/model.h); anything else is a root, so
    // a list that broke that cannot send a walk round in a circle.
    return joints[joint].parent < joint;
}

[[nodiscard]] bool descendsFrom(std::span<const asset::Joint> joints, usize joint, usize ancestor) noexcept
{
    while (hasParent(joints, joint)) {
        joint = joints[joint].parent;
        if (joint == ancestor)
            return true;
    }
    return false;
}

// Nearer the root first; then by name, so that two siblings saying one thing
// are told apart the same way whichever the file wrote first.
struct RootFirst
{
    std::span<const asset::Joint> joints;
    const std::vector<u32>& depth;

    [[nodiscard]] bool operator()(usize a, usize b) const noexcept
    {
        if (depth[a] != depth[b])
            return depth[a] < depth[b];
        if (joints[a].name != joints[b].name)
            return joints[a].name < joints[b].name;
        return a < b;
    }
};

// `shoulder` and `leg`, each settled by what else its side of the rig has.
void resolveWords(std::vector<Claim>& claims) noexcept
{
    for (const Side side : {Side::Left, Side::Right}) {
        bool upperArm = false;
        bool upperLeg = false;
        for (const Claim& claim : claims) {
            if (claim.side != side)
                continue;
            upperArm = upperArm || claim.part == Part::UpperArm;
            upperLeg = upperLeg || claim.part == Part::UpperLeg;
        }
        for (Claim& claim : claims) {
            if (claim.side != side)
                continue;
            if (claim.part == Part::ShoulderWord)
                claim.part = upperArm ? Part::Shoulder : Part::UpperArm;
            else if (claim.part == Part::LegWord)
                claim.part = upperLeg ? Part::LowerLeg : Part::UpperLeg;
        }
    }
}

// **A limb's joint hangs from the joint above it in the limb.** A rig that was
// animated with handles and exported with them has joints called `Foot.L`
// lying under its root, where the leg's handle was, and they are no part of
// the leg: a clip cannot turn them with the shin, and one of them nearer the
// root would take the role from the real foot. Where a side has a joint for
// the part above, a joint that does not hang from one says nothing.
void resolveLimbs(std::span<const asset::Joint> joints, std::vector<Claim>& claims) noexcept
{
    struct Link
    {
        Part part;
        Part above;
    };
    // Top down, so that a joint dropped here is not what the next hangs from.
    constexpr Link Links[] = {
        {Part::LowerArm, Part::UpperArm}, {Part::Hand, Part::LowerArm}, {Part::LowerLeg, Part::UpperLeg},
        {Part::Foot, Part::LowerLeg},     {Part::Toes, Part::Foot},
    };
    for (const Link& link : Links) {
        for (const Side side : {Side::Left, Side::Right}) {
            bool hasAbove = false;
            for (const Claim& claim : claims)
                hasAbove = hasAbove || (claim.part == link.above && claim.side == side);
            if (!hasAbove)
                continue;
            for (usize joint = 0; joint < claims.size(); ++joint) {
                if (claims[joint].part != link.part || claims[joint].side != side)
                    continue;
                bool hangs = false;
                for (usize up = joint; hasParent(joints, up) && !hangs;) {
                    up = joints[up].parent;
                    hangs = claims[up].part == link.above && claims[up].side == side;
                }
                if (!hangs)
                    claims[joint] = Claim{};
            }
        }
    }
}

// **The hips are the joint the legs hang from.** Some rigs put a joint for the
// whole body under the root, hang the legs and the back from it, and call the
// first joint of the back `Hips`. What a clip moves as the hips of such a rig
// is the joint above: the nearest one that holds the named hips and every
// upper leg, when it is not the rig's root (a root stays where the character
// is put) and says nothing else. The joint that was called the hips is then
// the first of the back.
void resolveHips(std::span<const asset::Joint> joints, const std::vector<u32>& depth, std::vector<Claim>& claims)
{
    const RootFirst rootFirst{joints, depth};
    i32 named = -1;
    for (usize joint = 0; joint < claims.size(); ++joint) {
        if (claims[joint].part != Part::Hips)
            continue;
        if (named < 0 || rootFirst(joint, static_cast<usize>(named)))
            named = static_cast<i32>(joint);
    }
    if (named < 0)
        return;
    std::vector<usize> legs;
    for (usize joint = 0; joint < claims.size(); ++joint) {
        if (claims[joint].part != Part::UpperLeg)
            continue;
        if (descendsFrom(joints, joint, static_cast<usize>(named)))
            return;
        legs.push_back(joint);
    }
    if (legs.empty())
        return;
    for (usize up = static_cast<usize>(named); hasParent(joints, up);) {
        up = joints[up].parent;
        bool holdsAll = true;
        for (const usize leg : legs)
            holdsAll = holdsAll && descendsFrom(joints, leg, up);
        if (!holdsAll)
            continue;
        if (hasParent(joints, up) && claims[up].part == Part::None) {
            claims[up] = Claim{Part::Hips, Side::None};
            claims[static_cast<usize>(named)].part = Part::Spine;
        }
        return;
    }
}

// The joints of the back, each of which said only "spine", given the parts
// their order gives them.
void resolveSpine(std::span<const asset::Joint> joints, const std::vector<u32>& depth, std::vector<Claim>& claims)
{
    // The same rig numbers that column from the hips: its first `spine` is the
    // joint the legs hang from, and nothing is called hips. A joint of the
    // back with a leg on it is the hips.
    bool hips = false;
    for (const Claim& claim : claims)
        hips = hips || claim.part == Part::Hips;
    if (!hips) {
        i32 legsFrom = -1;
        for (usize joint = 0; joint < claims.size(); ++joint) {
            if (claims[joint].part != Part::UpperLeg)
                continue;
            for (usize up = joint; hasParent(joints, up);) {
                up = joints[up].parent;
                if (claims[up].part == Part::Spine) {
                    if (legsFrom < 0 || depth[up] < depth[static_cast<usize>(legsFrom)])
                        legsFrom = static_cast<i32>(up);
                    break;
                }
            }
        }
        if (legsFrom >= 0)
            claims[static_cast<usize>(legsFrom)].part = Part::Hips;
    }

    std::vector<usize> back;
    bool neckOrHead = false;
    for (usize joint = 0; joint < claims.size(); ++joint) {
        if (claims[joint].part == Part::Spine)
            back.push_back(joint);
        neckOrHead = neckOrHead || claims[joint].part == Part::Neck || claims[joint].part == Part::Head;
    }
    std::sort(back.begin(), back.end(), RootFirst{joints, depth});

    // One common rig numbers its whole column as one spine, neck and head
    // included (`spine` to `spine.006`). Where nothing is called a neck or a
    // head, what stands above the joint the arms hang from is them: the first
    // the neck, the last the head.
    if (!neckOrHead) {
        i32 armsFrom = -1;
        for (usize joint = 0; joint < claims.size(); ++joint) {
            if (claims[joint].part != Part::Shoulder && claims[joint].part != Part::UpperArm)
                continue;
            for (usize up = joint; hasParent(joints, up);) {
                up = joints[up].parent;
                if (claims[up].part == Part::Spine) {
                    if (armsFrom < 0 || depth[up] > depth[static_cast<usize>(armsFrom)])
                        armsFrom = static_cast<i32>(up);
                    break;
                }
            }
        }
        if (armsFrom >= 0) {
            std::vector<usize> above;
            for (const usize joint : back) {
                if (descendsFrom(joints, joint, static_cast<usize>(armsFrom)))
                    above.push_back(joint);
            }
            if (above.size() >= 2) {
                for (const usize joint : above) {
                    claims[joint].part = Part::None;
                    back.erase(std::find(back.begin(), back.end(), joint));
                }
                claims[above.front()].part = Part::Neck;
                claims[above.back()].part = Part::Head;
            }
        }
    }

    for (usize place = 0; place < back.size(); ++place) {
        Part part = Part::None;
        if (place == 0)
            part = Part::Spine;
        else if (place == 1)
            part = Part::Chest;
        else if (place + 1 == back.size())
            part = Part::UpperChest;
        claims[back[place]].part = part;
    }
}

// --- Quaternions -------------------------------------------------------------------
//
// The map is worked out in f64 and handed over in f32: a chain of a dozen rest
// rotations multiplied in f32 would arrive a few units in the fifth place out,
// and it is multiplied into every sample of every clip from then on.

struct Vec
{
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;
};

struct Quat
{
    f64 x = 0.0;
    f64 y = 0.0;
    f64 z = 0.0;
    f64 w = 1.0;
};

[[nodiscard]] Vec operator-(Vec a, Vec b) noexcept
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] Vec operator+(Vec a, Vec b) noexcept
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] Vec cross(Vec a, Vec b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] f64 dot(Vec a, Vec b) noexcept
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] f64 length(Vec v) noexcept
{
    return std::sqrt(dot(v, v));
}

[[nodiscard]] Vec toVec(core::DVec3 v) noexcept
{
    return {v.x, v.y, v.z};
}

[[nodiscard]] Quat normalized(Quat q) noexcept
{
    const f64 norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(norm > 0.0))
        return {};
    return {q.x / norm, q.y / norm, q.z / norm, q.w / norm};
}

// "First `b`, then `a`", as the engine's matrices compose: the rotation of
// `a * b` is `fromQuaternion(a) * fromQuaternion(b)`, which is how a child's
// rotation goes onto its parent's in the pose walk.
[[nodiscard]] Quat operator*(Quat a, Quat b) noexcept
{
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

[[nodiscard]] Quat conjugate(Quat q) noexcept
{
    return {-q.x, -q.y, -q.z, q.w};
}

[[nodiscard]] Vec rotate(Quat q, Vec v) noexcept
{
    const Vec axis{q.x, q.y, q.z};
    const Vec once = cross(axis, v);
    const Vec twice = cross(axis, once);
    return {v.x + 2.0 * (q.w * once.x + twice.x), v.y + 2.0 * (q.w * once.y + twice.y),
            v.z + 2.0 * (q.w * once.z + twice.z)};
}

[[nodiscard]] Quat toQuat(const core::Mat3& rotation) noexcept
{
    f32 x = 0.0f;
    f32 y = 0.0f;
    f32 z = 0.0f;
    f32 w = 1.0f;
    core::toQuaternion(rotation, x, y, z, w);
    return normalized(Quat{static_cast<f64>(x), static_cast<f64>(y), static_cast<f64>(z), static_cast<f64>(w)});
}

// The shortest turn that takes the direction of `from` to that of `to`; the
// identity when either has no direction. For two that point opposite ways
// every half turn is as short as another, and the one about the axis found
// below is taken -- a fixed choice, so the same on every machine.
[[nodiscard]] Quat shortestArc(Vec from, Vec to) noexcept
{
    constexpr f64 Smallest = 1.0e-9;
    const f64 fromLength = length(from);
    const f64 toLength = length(to);
    if (!(fromLength > Smallest) || !(toLength > Smallest))
        return {};
    const Vec a{from.x / fromLength, from.y / fromLength, from.z / fromLength};
    const Vec b{to.x / toLength, to.y / toLength, to.z / toLength};
    const f64 cosine = dot(a, b);
    if (cosine < -1.0 + 1.0e-9) {
        const Vec other = std::fabs(a.x) < 0.9 ? Vec{1.0, 0.0, 0.0} : Vec{0.0, 1.0, 0.0};
        const Vec axis = cross(a, other);
        return normalized(Quat{axis.x, axis.y, axis.z, 0.0});
    }
    const Vec axis = cross(a, b);
    return normalized(Quat{axis.x, axis.y, axis.z, 1.0 + cosine});
}

void store(std::vector<f32>& into, usize joint, Quat q) noexcept
{
    into[joint * 4 + 0] = static_cast<f32>(q.x);
    into[joint * 4 + 1] = static_cast<f32>(q.y);
    into[joint * 4 + 2] = static_cast<f32>(q.z);
    into[joint * 4 + 3] = static_cast<f32>(q.w);
}

void multiply(const f32* a, const f32* b, f32* out) noexcept
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// A rig at rest in its model's space: where each joint is and how it is turned.
struct RestPose
{
    std::vector<Quat> rotation;
    std::vector<Vec> position;
};

[[nodiscard]] RestPose restOf(std::span<const asset::Joint> joints)
{
    RestPose rest;
    rest.rotation.resize(joints.size());
    rest.position.resize(joints.size());
    for (usize joint = 0; joint < joints.size(); ++joint) {
        const Quat local = toQuat(joints[joint].localBind.rotation);
        const Vec offset = toVec(joints[joint].localBind.position);
        if (hasParent(joints, joint)) {
            const usize parent = joints[joint].parent;
            rest.rotation[joint] = rest.rotation[parent] * local;
            rest.position[joint] = rest.position[parent] + rotate(rest.rotation[parent], offset);
        }
        else {
            rest.rotation[joint] = local;
            rest.position[joint] = offset;
        }
    }
    return rest;
}

[[nodiscard]] bool consistent(std::span<const asset::Joint> joints, const RigRoles& roles) noexcept
{
    if (roles.ofJoint.size() != joints.size())
        return false;
    for (const i32 joint : roles.jointOf) {
        if (joint >= 0 && static_cast<usize>(joint) >= joints.size())
            return false;
    }
    return true;
}

// The joint of `rig` that has `role`, when that joint says so too.
[[nodiscard]] i32 jointWith(const RigRoles& rig, Role role) noexcept
{
    const i32 joint = rig.joint(role);
    return joint >= 0 && rig.ofJoint[static_cast<usize>(joint)] == role ? joint : -1;
}

// --- Which way a rig stands ----------------------------------------------------------
//
// Nothing says which way a rig's own space is up or which way its body faces.
// A model drawn for one tool looks along +Z and one drawn for another along
// -Z; and a rig converted from an older format hangs under a node turned a
// quarter turn that is no joint, so the space its joints rest in has another
// axis for up than the file has. A turn carried between two such rigs as it
// is swings a leg backwards, or leans a body sideways for a bow.
//
// So each rig's body is read from where it rests: up is from its hips to the
// top of its back, and its left is from its right leg to its left. Both are
// taken as whole axes -- a body that slouches or stands a little turned is
// still a body standing up along one axis and facing along another -- so the
// turn between two rigs is one of the twenty-four that take axes to axes, and
// is exactly none where they agree.

struct Axis
{
    usize lane = 0;
    f64 sign = 0.0;
};

[[nodiscard]] f64 lane(Vec v, usize index) noexcept
{
    return index == 0 ? v.x : (index == 1 ? v.y : v.z);
}

[[nodiscard]] Vec unit(Axis axis) noexcept
{
    return {axis.lane == 0 ? axis.sign : 0.0, axis.lane == 1 ? axis.sign : 0.0, axis.lane == 2 ? axis.sign : 0.0};
}

// The axis `v` lies nearest, of those that are not `skip` (3 for none). A
// `sign` of nought for a vector with no length along any of them.
[[nodiscard]] Axis nearestAxis(Vec v, usize skip) noexcept
{
    Axis best;
    f64 longest = 1.0e-9;
    for (usize index = 0; index < 3; ++index) {
        const f64 along = lane(v, index);
        if (index == skip || !(std::fabs(along) > longest))
            continue;
        longest = std::fabs(along);
        best = Axis{index, along > 0.0 ? 1.0 : -1.0};
    }
    return best;
}

struct BodyFrame
{
    Vec left;
    Vec up;
    Vec forward;
    bool known = false;
};

[[nodiscard]] BodyFrame frameOf(const RestPose& rest, const RigRoles& roles) noexcept
{
    BodyFrame frame;
    const i32 hips = jointWith(roles, Role::Hips);
    i32 top = -1;
    for (const Role role : {Role::Head, Role::Neck, Role::UpperChest, Role::Chest, Role::Spine}) {
        if (top < 0)
            top = jointWith(roles, role);
    }
    if (hips < 0 || top < 0)
        return frame;
    const Axis up = nearestAxis(rest.position[static_cast<usize>(top)] - rest.position[static_cast<usize>(hips)], 3);
    if (up.sign == 0.0)
        return frame;

    Axis left;
    constexpr Role Pairs[2][2] = {{Role::LeftUpperLeg, Role::RightUpperLeg}, {Role::LeftUpperArm, Role::RightUpperArm}};
    for (const auto& pair : Pairs) {
        const i32 leftJoint = jointWith(roles, pair[0]);
        const i32 rightJoint = jointWith(roles, pair[1]);
        if (left.sign != 0.0 || leftJoint < 0 || rightJoint < 0)
            continue;
        left = nearestAxis(rest.position[static_cast<usize>(leftJoint)] - rest.position[static_cast<usize>(rightJoint)],
                           up.lane);
    }
    if (left.sign == 0.0)
        return frame;

    frame.left = unit(left);
    frame.up = unit(up);
    // +X to the left and +Y up is +Z ahead: the file format's own body.
    frame.forward = cross(frame.left, frame.up);
    frame.known = true;
    return frame;
}

// The rotation whose matrix has these columns, which are whole axes here; the
// branch on the largest of the diagonal keeps the square root away from nought.
[[nodiscard]] Quat quatOfColumns(Vec x, Vec y, Vec z) noexcept
{
    const f64 trace = x.x + y.y + z.z;
    if (trace > 0.0) {
        const f64 s = std::sqrt(trace + 1.0) * 2.0;
        return normalized(Quat{(y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25 * s});
    }
    if (x.x > y.y && x.x > z.z) {
        const f64 s = std::sqrt(1.0 + x.x - y.y - z.z) * 2.0;
        return normalized(Quat{0.25 * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s});
    }
    if (y.y > z.z) {
        const f64 s = std::sqrt(1.0 + y.y - x.x - z.z) * 2.0;
        return normalized(Quat{(y.x + x.y) / s, 0.25 * s, (z.y + y.z) / s, (z.x - x.z) / s});
    }
    const f64 s = std::sqrt(1.0 + z.z - x.x - y.y) * 2.0;
    return normalized(Quat{(z.x + x.z) / s, (z.y + y.z) / s, 0.25 * s, (x.y - y.x) / s});
}

// The turn that lays the source's body on the target's: its left on the
// target's left, its up on the target's up. The identity -- exactly -- where
// the two stand alike, and where either cannot be read.
[[nodiscard]] Quat turnBetween(const BodyFrame& source, const BodyFrame& target) noexcept
{
    if (!source.known || !target.known)
        return {};
    // M = T S^-1, with S and T the two frames as columns; a column of M is
    // where M takes one axis.
    const auto column = [&](usize index) {
        const f64 l = lane(source.left, index);
        const f64 u = lane(source.up, index);
        const f64 f = lane(source.forward, index);
        return Vec{target.left.x * l + target.up.x * u + target.forward.x * f,
                   target.left.y * l + target.up.y * u + target.forward.y * f,
                   target.left.z * l + target.up.z * u + target.forward.z * f};
    };
    return quatOfColumns(column(0), column(1), column(2));
}

[[nodiscard]] bool isIdentity(Quat q) noexcept
{
    return q.x == 0.0 && q.y == 0.0 && q.z == 0.0;
}

// The joint a bone of `role` runs to in both rigs, or `None`.
[[nodiscard]] Role boneEnd(Role role, const RigRoles& source, const RigRoles& target) noexcept
{
    const RoleRow& row = Roles[static_cast<usize>(role)];
    for (const Bone& bone : Bones) {
        if (bone.from != row.part)
            continue;
        for (const Part part : bone.to) {
            // A part of the back has no side; a limb's end is on the limb's.
            Role end = roleOf(part, row.side);
            if (end == Role::None)
                end = roleOf(part, Side::None);
            if (end != Role::None && jointWith(source, end) >= 0 && jointWith(target, end) >= 0)
                return end;
        }
    }
    return Role::None;
}

// The length of the legs the two rigs can be compared by: thigh and shin, a
// leg both have a foot on; the thigh alone where one does not.
void legLengths(const RestPose& source, const RigRoles& sourceRoles, const RestPose& target,
                const RigRoles& targetRoles, f64& sourceLength, f64& targetLength) noexcept
{
    constexpr Role Legs[2][3] = {
        {Role::LeftUpperLeg, Role::LeftLowerLeg, Role::LeftFoot},
        {Role::RightUpperLeg, Role::RightLowerLeg, Role::RightFoot},
    };
    sourceLength = 0.0;
    targetLength = 0.0;
    for (const auto& leg : Legs) {
        i32 sourceJoint[3]{};
        i32 targetJoint[3]{};
        for (usize part = 0; part < 3; ++part) {
            sourceJoint[part] = jointWith(sourceRoles, leg[part]);
            targetJoint[part] = jointWith(targetRoles, leg[part]);
        }
        for (usize part = 0; part + 1 < 3; ++part) {
            if (sourceJoint[part] < 0 || sourceJoint[part + 1] < 0 || targetJoint[part] < 0 ||
                targetJoint[part + 1] < 0)
                break;
            sourceLength += length(source.position[static_cast<usize>(sourceJoint[part + 1])] -
                                   source.position[static_cast<usize>(sourceJoint[part])]);
            targetLength += length(target.position[static_cast<usize>(targetJoint[part + 1])] -
                                   target.position[static_cast<usize>(targetJoint[part])]);
        }
    }
}

void fail(std::string* error, std::string text)
{
    if (error != nullptr)
        *error = std::move(text);
}

} // namespace

// --- Roles -------------------------------------------------------------------------

std::string_view roleName(Role role) noexcept
{
    const usize index = static_cast<usize>(role);
    return index < Roles.size() ? Roles[index].name : std::string_view{};
}

Role roleFromName(std::string_view name) noexcept
{
    for (usize index = 1; index < Roles.size(); ++index) {
        if (Roles[index].name == name)
            return static_cast<Role>(index);
    }
    return Role::None;
}

std::optional<std::vector<RoleOverride>> readRigRoles(std::string_view json, std::string* error)
{
    constexpr std::string_view Format = "rig";
    constexpr core::i64 Version = 1;

    core::JsonDocument document;
    if (const core::JsonDocument::ParseResult parsed = document.parse(json, "rig"); !parsed) {
        fail(error, parsed.diagnostic);
        return std::nullopt;
    }
    const core::JsonValue root = document.root();
    if (root.type() != core::JsonType::Object || root["format"].asString() != Format) {
        fail(error, "not a rig file");
        return std::nullopt;
    }
    if (root["version"].asInteger() != Version) {
        fail(error, "rig version " + std::to_string(root["version"].asInteger()) + " is not one this engine reads");
        return std::nullopt;
    }
    const core::JsonValue roles = root["roles"];
    if (roles.type() != core::JsonType::Object) {
        fail(error, "roles: expected an object of role to joint name");
        return std::nullopt;
    }

    std::vector<RoleOverride> out;
    out.reserve(roles.size());
    for (usize index = 0; index < roles.size(); ++index) {
        const std::string_view key = roles.keyAt(index);
        const Role role = roleFromName(key);
        if (role == Role::None) {
            fail(error, "roles." + std::string(key) + ": no such role");
            return std::nullopt;
        }
        const core::JsonValue joint = roles[key];
        if (joint.type() != core::JsonType::String) {
            fail(error, "roles." + std::string(key) + ": expected a joint's name");
            return std::nullopt;
        }
        out.push_back(RoleOverride{role, std::string(joint.asString())});
    }
    return out;
}

bool RigRoles::body() const noexcept
{
    constexpr Role Needed[] = {
        Role::Hips,         Role::LeftUpperLeg, Role::LeftLowerLeg,  Role::RightUpperLeg, Role::RightLowerLeg,
        Role::LeftUpperArm, Role::LeftLowerArm, Role::RightUpperArm, Role::RightLowerArm,
    };
    for (const Role role : Needed) {
        if (joint(role) < 0)
            return false;
    }
    return joint(Role::Spine) >= 0 || joint(Role::Chest) >= 0;
}

RigRoles assignRoles(std::span<const asset::Joint> joints, std::span<const RoleOverride> overrides)
{
    RigRoles out;
    out.ofJoint.assign(joints.size(), Role::None);

    std::vector<u32> depth(joints.size(), 0);
    std::vector<Claim> claims(joints.size());
    for (usize joint = 0; joint < joints.size(); ++joint) {
        if (hasParent(joints, joint))
            depth[joint] = depth[joints[joint].parent] + 1;
        const ParsedName parsed = parseName(joints[joint].name);
        claims[joint] = Claim{partOf(parsed), parsed.side};
    }
    resolveWords(claims);
    resolveLimbs(joints, claims);
    resolveHips(joints, depth, claims);
    resolveSpine(joints, depth, claims);

    // Two joints saying one role: the one nearer the root has it, and the
    // other has none -- a second segment of an arm, a second neck.
    const RootFirst rootFirst{joints, depth};
    for (usize joint = 0; joint < joints.size(); ++joint) {
        const Role role = roleOf(claims[joint].part, claims[joint].side);
        if (role == Role::None)
            continue;
        i32& holder = out.jointOf[static_cast<usize>(role)];
        if (holder < 0 || rootFirst(joint, static_cast<usize>(holder)))
            holder = static_cast<i32>(joint);
    }
    for (usize role = 1; role < RoleCount; ++role) {
        if (out.jointOf[role] >= 0)
            out.ofJoint[static_cast<usize>(out.jointOf[role])] = static_cast<Role>(role);
    }

    for (const RoleOverride& said : overrides) {
        const usize role = static_cast<usize>(said.role);
        if (role == 0 || role >= RoleCount)
            continue;
        i32 named = -1;
        if (!said.joint.empty()) {
            for (usize joint = 0; joint < joints.size(); ++joint) {
                if (joints[joint].name == said.joint) {
                    named = static_cast<i32>(joint);
                    break;
                }
            }
            if (named < 0)
                continue;
        }
        if (out.jointOf[role] >= 0)
            out.ofJoint[static_cast<usize>(out.jointOf[role])] = Role::None;
        out.jointOf[role] = named;
        if (named < 0)
            continue;
        // The joint leaves whatever it was guessed to be.
        const Role was = out.ofJoint[static_cast<usize>(named)];
        if (was != Role::None)
            out.jointOf[static_cast<usize>(was)] = -1;
        out.ofJoint[static_cast<usize>(named)] = said.role;
    }
    return out;
}

// --- The map -----------------------------------------------------------------------

Map buildMap(std::span<const asset::Joint> source, const RigRoles& sourceRoles, std::span<const asset::Joint> target,
             const RigRoles& targetRoles)
{
    Map map;
    map.slots.assign(source.size(), -1);
    map.byRole.assign(source.size(), 0);
    map.pre.assign(source.size() * 4, 0.0f);
    map.post.assign(source.size() * 4, 0.0f);
    map.rest.assign(source.size() * 4, 0.0f);
    for (usize joint = 0; joint < source.size(); ++joint) {
        map.pre[joint * 4 + 3] = 1.0f;
        map.post[joint * 4 + 3] = 1.0f;
        map.rest[joint * 4 + 3] = 1.0f;
    }

    map.roles =
        consistent(source, sourceRoles) && consistent(target, targetRoles) && sourceRoles.body() && targetRoles.body();

    // **What stands above the hips is the rig's own.** A root joint says where
    // a body is put and how its space lies -- one rig's rests upright and the
    // next one's a quarter turn over, both called `root` -- and a clip keys it
    // at its own rest. Carried by its name it laid a whole character on its
    // back. Under roles neither rig's joints above its hips are matched: the
    // target's stay as they rest, and the hips carry the body.
    const auto aboveHips = [&map](std::span<const asset::Joint> joints, const RigRoles& roles) {
        std::vector<u8> above(joints.size(), 0);
        const i32 hips = map.roles ? jointWith(roles, Role::Hips) : -1;
        if (hips < 0)
            return above;
        for (usize up = static_cast<usize>(hips); hasParent(joints, up);) {
            up = joints[up].parent;
            above[up] = 1;
        }
        return above;
    };
    const std::vector<u8> sourceAbove = aboveHips(source, sourceRoles);
    const std::vector<u8> targetAbove = aboveHips(target, targetRoles);

    // By equal name: the first target joint called the same. Under roles, one
    // that has no role either -- a target joint with a role is driven by the
    // source joint of that role, and would otherwise be written twice.
    const auto byName = [&](usize joint) {
        if (sourceAbove[joint] != 0)
            return;
        for (usize other = 0; other < target.size(); ++other) {
            if (source[joint].name != target[other].name)
                continue;
            if (map.roles && (targetRoles.ofJoint[other] != Role::None || targetAbove[other] != 0))
                continue;
            map.slots[joint] = static_cast<i32>(other);
            return;
        }
    };

    if (!map.roles) {
        for (usize joint = 0; joint < source.size(); ++joint)
            byName(joint);
        return map;
    }

    RestPose sourceRest = restOf(source);
    const RestPose targetRest = restOf(target);

    // **The two rigs' own spaces, laid one on the other.** From here on the
    // source rests in the TARGET's space: turned so that its up and its left
    // are the target's. `facing` then stands for the source's missing parent
    // wherever a root's is asked for, and everything below -- a bone's
    // direction, a turn from rest, the hips' travel -- is compared in one
    // space. Nothing is touched where the two already agree.
    const Quat facing = turnBetween(frameOf(sourceRest, sourceRoles), frameOf(targetRest, targetRoles));
    if (!isIdentity(facing)) {
        for (usize joint = 0; joint < source.size(); ++joint) {
            sourceRest.rotation[joint] = normalized(facing * sourceRest.rotation[joint]);
            sourceRest.position[joint] = rotate(facing, sourceRest.position[joint]);
        }
    }

    // **The stance.** The target's rest, turned joint by joint from the root
    // down until each of its bones lies along the source's: `aligned` is the
    // pose the target would stand in if it rested as the source does, with its
    // own bone lengths. One pass, because parents come first -- a joint is
    // placed under its already turned parent, compared, and turned, and what
    // hangs below it follows when its own turn in the pass comes.
    RestPose aligned;
    aligned.rotation.resize(target.size());
    aligned.position.resize(target.size());
    for (usize joint = 0; joint < target.size(); ++joint) {
        const Quat local = toQuat(target[joint].localBind.rotation);
        const Vec offset = toVec(target[joint].localBind.position);
        if (hasParent(target, joint)) {
            const usize parent = target[joint].parent;
            aligned.rotation[joint] = aligned.rotation[parent] * local;
            aligned.position[joint] = aligned.position[parent] + rotate(aligned.rotation[parent], offset);
        }
        else {
            aligned.rotation[joint] = local;
            aligned.position[joint] = offset;
        }

        const Role role = targetRoles.ofJoint[joint];
        if (role == Role::None || jointWith(targetRoles, role) != static_cast<i32>(joint))
            continue;
        const i32 sourceJoint = jointWith(sourceRoles, role);
        if (sourceJoint < 0)
            continue;
        const Role end = boneEnd(role, sourceRoles, targetRoles);
        if (end == Role::None)
            continue;
        const usize sourceEnd = static_cast<usize>(jointWith(sourceRoles, end));
        const usize targetEnd = static_cast<usize>(jointWith(targetRoles, end));
        // A bone runs DOWN the rig. Roles put on a rig by hand may say
        // otherwise, and there is then no bone to compare.
        if (!descendsFrom(source, sourceEnd, static_cast<usize>(sourceJoint)) ||
            !descendsFrom(target, targetEnd, joint))
            continue;

        const Vec sourceBone = sourceRest.position[sourceEnd] - sourceRest.position[static_cast<usize>(sourceJoint)];
        // The end rides on this joint as it did at rest: nothing between the
        // two has been turned, since nothing between them has a bone of its own
        // to compare.
        const Vec restBone = targetRest.position[targetEnd] - targetRest.position[joint];
        // **Two rests more than a third of a turn apart are not two stances.**
        // An arm held out and one hanging are a quarter turn apart; a bone that
        // points the other way altogether is a rig that put the joint at the
        // other end of it -- one whose first joint of the back lies BELOW its
        // hips had the other rig's pelvis turned upside down to match. Such a
        // joint is left as it rests under its parent.
        if (dot(restBone, sourceBone) < -0.5 * length(restBone) * length(sourceBone))
            continue;
        const Quat carried = aligned.rotation[joint] * conjugate(targetRest.rotation[joint]);
        const Vec targetBone = rotate(carried, restBone);
        aligned.rotation[joint] = normalized(shortestArc(targetBone, sourceBone) * aligned.rotation[joint]);
    }

    // **The turn from rest.** With `Gs` and `Gt` the two parents' rest
    // rotations in model space, `Rs` and `Rt` the two joints' rest rotations
    // from their parents, and `q` the clip's sample: the source joint stands at
    // `Gs q` and rests at `Gs Rs`, so it has turned by `D = Gs q Rs^-1 Gs^-1`
    // in the model's space. The target joint given the same turn stands at
    // `D Gt Rt`, which from its parent is
    //
    //     Gt^-1 D Gt Rt  =  (Gt^-1 Gs)  q  (Rs^-1 Gs^-1 Gt Rt)
    //
    // -- `pre`, the sample, `post`. Down a chain matched joint for joint the
    // parents' own turns cancel, and every target joint stands at the source
    // joint's whole turn from rest times its own rest.
    for (usize joint = 0; joint < source.size(); ++joint) {
        const Role role = sourceRoles.ofJoint[joint];
        if (role == Role::None || jointWith(sourceRoles, role) != static_cast<i32>(joint)) {
            byName(joint);
            continue;
        }
        map.byRole[joint] = 1;
        const i32 other = jointWith(targetRoles, role);
        if (other < 0) {
            // Each role has one joint, so each is met once; and in the rig's
            // order, which is put into `Role`'s below.
            map.unmapped.push_back(role);
            continue;
        }
        map.slots[joint] = other;

        const usize targetJoint = static_cast<usize>(other);
        const Quat sourceParent = hasParent(source, joint) ? sourceRest.rotation[source[joint].parent] : facing;
        const Quat targetParent =
            hasParent(target, targetJoint) ? aligned.rotation[target[targetJoint].parent] : Quat{};
        const Quat sourceLocal = toQuat(source[joint].localBind.rotation);
        const Quat targetLocal = normalized(conjugate(targetParent) * aligned.rotation[targetJoint]);

        store(map.pre, joint, normalized(conjugate(targetParent) * sourceParent));
        store(map.post, joint,
              normalized(conjugate(sourceLocal) * conjugate(sourceParent) * targetParent * targetLocal));
        store(map.rest, joint, targetLocal);

        if (role == Role::Hips) {
            map.hips = static_cast<i32>(joint);
            map.hipsSourceRest = source[joint].localBind.position;
            map.hipsTargetRest = target[targetJoint].localBind.position;
        }
    }
    std::sort(map.unmapped.begin(), map.unmapped.end());

    f64 sourceLegs = 0.0;
    f64 targetLegs = 0.0;
    legLengths(sourceRest, sourceRoles, targetRest, targetRoles, sourceLegs, targetLegs);
    map.hipsScale = sourceLegs > 0.0 ? targetLegs / sourceLegs : 1.0;
    return map;
}

void rotation(const Map& map, core::u32 sourceJoint, const core::f32 sample[4], core::f32 out[4]) noexcept
{
    if (sourceJoint >= map.byRole.size() || map.byRole[sourceJoint] == 0) {
        for (usize lane = 0; lane < 4; ++lane)
            out[lane] = sample[lane];
        return;
    }
    f32 turned[4];
    multiply(&map.pre[static_cast<usize>(sourceJoint) * 4], sample, turned);
    f32 result[4];
    multiply(turned, &map.post[static_cast<usize>(sourceJoint) * 4], result);
    for (usize lane = 0; lane < 4; ++lane)
        out[lane] = result[lane];
}

core::DVec3 hipsTranslation(const Map& map, core::DVec3 sample) noexcept
{
    if (map.hips < 0)
        return sample;
    const f32* pre = &map.pre[static_cast<usize>(map.hips) * 4];
    const Quat turn{static_cast<f64>(pre[0]), static_cast<f64>(pre[1]), static_cast<f64>(pre[2]),
                    static_cast<f64>(pre[3])};
    const Vec travel = rotate(turn, toVec(sample - map.hipsSourceRest));
    return core::DVec3{map.hipsTargetRest.x + map.hipsScale * travel.x, map.hipsTargetRest.y + map.hipsScale * travel.y,
                       map.hipsTargetRest.z + map.hipsScale * travel.z};
}

} // namespace engine::render::retarget
