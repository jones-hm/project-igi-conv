#include "pch.h"
#include "cmd_lightmap.h"
#include "qsc_object_parser.h"
#include "lightmap_resolver.h"
#include "mef_native.h"
#include "cmd_olm.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>
#include <chrono>

namespace fs = std::filesystem;
using igi1conv::LightmapBinding;
using igi1conv::LightmapBindingSet;

static void print_lightmap_help()
{
    std::cout <<
        "Usage: igi1conv lightmap <subcommand> [options]\n"
        "\n"
        "Subcommands:\n"
        "  list    --model <id> --qsc <objects.qsc>\n"
        "          List every placement of <id> bound to a lightmap (task id,\n"
        "          name, position, logical lightmap id).\n"
        "\n"
        "  resolve --model <id> --qsc <objects.qsc> [--task-id <id> | --pos X,Y,Z]\n"
        "          Resolve which placed instance's lightmap applies to <id> and\n"
        "          print its logical id plus the matching .olm file paths.\n"
        "          The same .mef model can be placed at multiple locations, each\n"
        "          with its own baked lightmap, so a bare --model is only enough\n"
        "          when exactly one placement exists; otherwise pass --task-id\n"
        "          (exact match) or --pos (nearest match by Euclidean distance)\n"
        "          to pick one.\n"
        "\n"
        "  recalc  --model <id> --qsc <objects.qsc> --task-id <id> --mef <file.mef>\n"
        "          --rot-orig X,Y,Z --rot-new X,Y,Z --sun-dir X,Y,Z\n"
        "          [--sun-color R,G,B] [--ambient R,G,B]\n"
        "          Re-light the bound .olm files in place after the object was\n"
        "          rotated/moved: each render block's baked lightmap is rescaled\n"
        "          per-channel by how much more/less its surface now faces the\n"
        "          sun (L(N_new)/L(N_orig)), preserving the original shadow detail\n"
        "          while approximating the new orientation. Overwrites the .olm\n"
        "          files in lightmaps_unpacked; repack lightmaps.res separately\n"
        "          (res pack) for the game to pick them up.\n"
        "\n"
        "Options:\n"
        "  --model <id>     Model id / .mef filename stem (e.g. 435_01_1)\n"
        "  --qsc <path>     Path to the level's decompiled objects.qsc\n"
        "  --task-id <id>   Disambiguate by the placed instance's Task_New id\n"
        "  --pos X,Y,Z      Disambiguate by nearest placed position (raw IGI units)\n"
        "  --mef <file>     (recalc) Path to the model's .mef for geometry/normals\n"
        "  --rot-orig X,Y,Z (recalc) Euler rotation (radians) at original bake time\n"
        "  --rot-new X,Y,Z  (recalc) Euler rotation (radians) after manipulation\n"
        "  --sun-dir X,Y,Z  (recalc) World-space direction toward the sun\n"
        "  --sun-color R,G,B (recalc) Directional light color 0..1 (default 0.6)\n"
        "  --ambient R,G,B  (recalc) Ambient light color 0..1 (default 0.3)\n"
        "  --help           Show this help\n"
        "\n"
        "Exit codes: 0=success 1=bad args 2=file not found 3=no binding 4=ambiguous\n";
}

namespace {

struct LightmapArgs {
    std::string model;
    std::string qscPath;
    bool hasTaskId = false;
    int32_t taskId = -1;
    bool hasPos = false;
    double x = 0, y = 0, z = 0;
};

// Parses "--model <id> --qsc <path> [--task-id <id> | --pos X,Y,Z]" style
// args shared by both subcommands. Returns false (with a message on
// stderr) on a malformed --pos or missing required flags.
bool ParseLightmapArgs(int argc, char** argv, int startIdx, LightmapArgs& out) {
    for (int i = startIdx; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--model" && i + 1 < argc) {
            out.model = argv[++i];
        } else if (arg == "--qsc" && i + 1 < argc) {
            out.qscPath = argv[++i];
        } else if (arg == "--task-id" && i + 1 < argc) {
            out.hasTaskId = true;
            out.taskId = std::atoi(argv[++i]);
        } else if (arg == "--pos" && i + 1 < argc) {
            std::string posArg = argv[++i];
            std::istringstream iss(posArg);
            std::string xs, ys, zs;
            if (!std::getline(iss, xs, ',') || !std::getline(iss, ys, ',') || !std::getline(iss, zs, ',')) {
                std::cerr << "lightmap: --pos expects \"X,Y,Z\", got \"" << posArg << "\"\n";
                return false;
            }
            try {
                out.x = std::stod(xs);
                out.y = std::stod(ys);
                out.z = std::stod(zs);
            } catch (...) {
                std::cerr << "lightmap: --pos expects \"X,Y,Z\" (numbers), got \"" << posArg << "\"\n";
                return false;
            }
            out.hasPos = true;
        }
    }
    if (out.model.empty()) {
        std::cerr << "lightmap: --model is required\n";
        return false;
    }
    if (out.qscPath.empty()) {
        std::cerr << "lightmap: --qsc is required\n";
        return false;
    }
    if (out.hasTaskId && out.hasPos) {
        std::cerr << "lightmap: pass only one of --task-id or --pos, not both\n";
        return false;
    }
    return true;
}

void PrintBinding(const LightmapBinding& b) {
    std::cout << "  task " << b.taskId << " \"" << b.taskName << "\""
               << " -> " << b.logicalId;
    if (b.hasPos) {
        std::cout << " @ (" << b.posX << ", " << b.posY << ", " << b.posZ << ")";
    }
    std::cout << "\n";
}

int do_lightmap_list(const LightmapArgs& args) {
    if (!fs::exists(args.qscPath)) {
        std::cerr << "lightmap: file not found: " << args.qscPath << "\n";
        return 2;
    }
    std::ifstream f(args.qscPath);
    std::string qscText((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    LightmapBindingSet set = LightmapBindingSet::parse(qscText);

    auto matches = set.allBindingsForModel(args.model);
    if (matches.empty()) {
        std::cout << "lightmap: no bindings found for model \"" << args.model << "\"\n";
        return 3;
    }

    std::cout << "lightmap: " << matches.size() << " placement(s) of \"" << args.model << "\":\n";
    for (auto* b : matches) PrintBinding(*b);
    return 0;
}

int do_lightmap_resolve(const LightmapArgs& args) {
    if (!fs::exists(args.qscPath)) {
        std::cerr << "lightmap: file not found: " << args.qscPath << "\n";
        return 2;
    }
    std::ifstream f(args.qscPath);
    std::string qscText((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    LightmapBindingSet set = LightmapBindingSet::parse(qscText);

    auto matches = set.allBindingsForModel(args.model);
    if (matches.empty()) {
        std::cout << "lightmap: no bindings found for model \"" << args.model << "\"\n";
        return 3;
    }

    const LightmapBinding* chosen = nullptr;
    if (args.hasTaskId) {
        chosen = set.bindingForModelAndTaskId(args.model, args.taskId);
        if (!chosen) {
            std::cerr << "lightmap: no placement of \"" << args.model << "\" has task id "
                       << args.taskId << "\n";
            std::cerr << "lightmap: available placements:\n";
            for (auto* b : matches) PrintBinding(*b);
            return 3;
        }
    } else if (args.hasPos) {
        chosen = set.nearestBindingForModelAndPosition(args.model, args.x, args.y, args.z);
        if (!chosen) {
            std::cerr << "lightmap: no placement of \"" << args.model << "\" has a known position\n";
            return 3;
        }
    } else if (matches.size() == 1) {
        chosen = matches.front();
    } else {
        std::cerr << "lightmap: \"" << args.model << "\" is placed at " << matches.size()
                   << " locations - pass --task-id or --pos to disambiguate:\n";
        for (auto* b : matches) PrintBinding(*b);
        return 4;
    }

    std::cout << "lightmap: resolved ";
    PrintBinding(*chosen);

    auto files = igi1conv::ResolveLightmapFilesForLogicalId(args.qscPath, chosen->logicalId);
    if (files.empty()) {
        std::cerr << "lightmap: binding " << chosen->logicalId
                   << " found but no .olm files on disk\n";
        return 3;
    }
    std::cout << "lightmap: " << files.size() << " .olm file(s):\n";
    for (auto& p : files) std::cout << "  " << p << "\n";
    return 0;
}

// ─── recalc (re-light baked .olm by new sun angle) ──────────────────────────

struct RecalcArgs {
    std::string model;
    std::string qscPath;
    std::string mefPath;
    int32_t taskId = -1;
    bool hasTaskId = false;
    glm::vec3 rotOrig{0.f};
    glm::vec3 rotNew{0.f};
    glm::vec3 sunDir{0.f, 0.f, 1.f};
    glm::vec3 sunColor{0.6f};
    glm::vec3 ambient{0.3f};
    bool hasSunDir = false;
};

// Parse "X,Y,Z" (radians or colors) into a vec3. Returns false on malformed input.
static bool ParseVec3(const std::string& s, glm::vec3& out) {
    std::istringstream iss(s);
    std::string xs, ys, zs;
    if (!std::getline(iss, xs, ',') || !std::getline(iss, ys, ',') || !std::getline(iss, zs, ',')) return false;
    try {
        out.x = std::stof(xs); out.y = std::stof(ys); out.z = std::stof(zs);
    } catch (...) { return false; }
    return true;
}

// Build the same Rz * Rx * Ry rotation the editor applies to objects
// (see app.cpp object model matrix construction).
static glm::mat3 EulerToMat(const glm::vec3& e) {
    glm::mat4 m(1.0f);
    m = glm::rotate(m, e.z, glm::vec3(0, 0, 1));
    m = glm::rotate(m, e.x, glm::vec3(1, 0, 0));
    m = glm::rotate(m, e.y, glm::vec3(0, 1, 0));
    return glm::mat3(m);
}

int do_lightmap_recalc(const RecalcArgs& args) {
    if (!fs::exists(args.qscPath)) {
        std::cerr << "lightmap: file not found: " << args.qscPath << "\n";
        return 2;
    }
    if (!args.hasTaskId) {
        std::cerr << "lightmap: recalc requires --task-id (exact placement)\n";
        return 1;
    }
    if (args.mefPath.empty() || !fs::exists(args.mefPath)) {
        std::cerr << "lightmap: recalc requires --mef <file.mef> (not found: " << args.mefPath << ")\n";
        return 2;
    }
    if (!args.hasSunDir) {
        std::cerr << "lightmap: recalc requires --sun-dir X,Y,Z\n";
        return 1;
    }

    // 1. Resolve the bound .olm files for this exact placement.
    std::ifstream f(args.qscPath);
    std::string qscText((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    LightmapBindingSet set = LightmapBindingSet::parse(qscText);
    const LightmapBinding* chosen = set.bindingForModelAndTaskId(args.model, args.taskId);
    if (!chosen) {
        std::cerr << "lightmap: no placement of \"" << args.model << "\" has task id " << args.taskId << "\n";
        return 3;
    }
    auto olmFiles = igi1conv::ResolveLightmapFilesForLogicalId(args.qscPath, chosen->logicalId);
    if (olmFiles.empty()) {
        std::cerr << "lightmap: binding " << chosen->logicalId << " has no .olm files on disk\n";
        return 3;
    }

    // 2. Load the model geometry for per-render-block surface normals.
    ParsedGeometry geo;
    try {
        geo = ParseMefFile(args.mefPath);
    } catch (const std::exception& e) {
        std::cerr << "lightmap: failed to parse MEF " << args.mefPath << ": " << e.what() << "\n";
        return 3;
    }
    if (geo.renderBlocks.empty()) {
        std::cerr << "lightmap: MEF has no render blocks: " << args.mefPath << "\n";
        return 3;
    }

    std::cout << "lightmap: recalc model=" << args.model << " task=" << args.taskId
               << " logical=" << chosen->logicalId
               << " olmFiles=" << olmFiles.size() << " renderBlocks=" << geo.renderBlocks.size() << "\n";

    const glm::mat3 Rorig = EulerToMat(args.rotOrig);
    const glm::mat3 Rnew  = EulerToMat(args.rotNew);
    const glm::vec3 sunDir = glm::length(args.sunDir) > 1e-6f ? glm::normalize(args.sunDir) : glm::vec3(0, 0, 1);

    // Per-channel directional+ambient luminance for a given world normal.
    auto Lum = [&](const glm::vec3& n) -> glm::vec3 {
        float ndl = std::max(glm::dot(n, sunDir), 0.0f);
        return args.ambient + args.sunColor * ndl;
    };

    size_t blockCount = std::min(geo.renderBlocks.size(), olmFiles.size());
    struct PendingWrite {
        fs::path target;
        fs::path temporary;
        fs::path backup;
        OLMFile olm;
        glm::vec3 factor;
    };
    std::vector<PendingWrite> pending;
    pending.reserve(blockCount);

    // Prepare every output before touching any original file.  A malformed
    // later lightmap must not leave an earlier block permanently rewritten.
    for (size_t i = 0; i < blockCount; ++i) {
        const auto& block = geo.renderBlocks[i];

        // Average FACE normal of the block. For type-3 lightmap meshes the
        // per-vertex normal slot actually holds UV data, so we derive normals
        // from triangle geometry (same as the editor's renderer).
        glm::vec3 accum(0.f);
        for (size_t t = block.triangleStart; t < block.triangleStart + block.triangleCount && t < geo.triangles.size(); ++t) {
            const auto& tri = geo.triangles[t];
            if (tri[0] >= geo.vertices.size() || tri[1] >= geo.vertices.size() || tri[2] >= geo.vertices.size()) continue;
            const glm::vec3& p0 = geo.vertices[tri[0]].pos;
            const glm::vec3& p1 = geo.vertices[tri[1]].pos;
            const glm::vec3& p2 = geo.vertices[tri[2]].pos;
            glm::vec3 fn = glm::cross(p1 - p0, p2 - p0);
            float len = glm::length(fn);
            if (len > 1e-9f) accum += fn / len;
        }
        glm::vec3 nLocal = glm::length(accum) > 1e-6f ? glm::normalize(accum) : glm::vec3(0, 0, 1);
        glm::vec3 nOrig = glm::normalize(Rorig * nLocal);
        glm::vec3 nNew  = glm::normalize(Rnew  * nLocal);

        glm::vec3 lOrig = Lum(nOrig);
        glm::vec3 lNew  = Lum(nNew);
        // Per-channel rescale factor, guarded against div-by-zero and clamped so a
        // surface that was fully shadowed before (lOrig≈0) can't blow up to white.
        const float eps = 1e-3f, maxF = 4.0f;
        glm::vec3 factor(
            std::min(lNew.x / std::max(lOrig.x, eps), maxF),
            std::min(lNew.y / std::max(lOrig.y, eps), maxF),
            std::min(lNew.z / std::max(lOrig.z, eps), maxF));

        OLMFile olm = ParseOlm(olmFiles[i]);
        if (!olm.valid) {
            std::cerr << "lightmap: aborting (parse failed): " << olmFiles[i] << " (" << olm.error
                      << "); no lightmap files were changed\n";
            return 3;
        }
        for (auto& px : olm.pixels) {
            px.r = static_cast<uint8_t>(std::min(255.0f, std::round(px.r * factor.x)));
            px.g = static_cast<uint8_t>(std::min(255.0f, std::round(px.g * factor.y)));
            px.b = static_cast<uint8_t>(std::min(255.0f, std::round(px.b * factor.z)));
        }
        pending.push_back({fs::path(olmFiles[i]), {}, {}, std::move(olm), factor});
    }

    if (pending.empty()) {
        std::cerr << "lightmap: recalc produced no writable blocks; no lightmap files were changed\n";
        return 3;
    }

    const auto transactionId = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (size_t i = 0; i < pending.size(); ++i) {
        pending[i].temporary = pending[i].target;
        pending[i].temporary += ".mcp-recalc-" + transactionId + "-" + std::to_string(i) + ".tmp";
        pending[i].backup = pending[i].target;
        pending[i].backup += ".mcp-recalc-" + transactionId + "-" + std::to_string(i) + ".bak";

        std::error_code existsError;
        if (fs::exists(pending[i].temporary, existsError) || existsError) {
            std::cerr << "lightmap: temporary output already exists: " << pending[i].temporary << "\n";
            return 3;
        }
        existsError.clear();
        if (fs::exists(pending[i].backup, existsError) || existsError) {
            std::cerr << "lightmap: backup output already exists: " << pending[i].backup << "\n";
            return 3;
        }
    }

    auto removeTemporaryFiles = [&]() {
        for (const auto& item : pending) {
            std::error_code ignored;
            fs::remove(item.temporary, ignored);
        }
    };

    for (const auto& item : pending) {
        std::string writeError;
        if (!WriteOlm(item.temporary.string(), item.olm, writeError)) {
            removeTemporaryFiles();
            std::cerr << "lightmap: temporary write failed: " << item.target << " (" << writeError
                      << "); no lightmap files were changed\n";
            return 3;
        }
    }

    struct CommittedWrite { fs::path target; fs::path backup; };
    std::vector<CommittedWrite> committed;
    committed.reserve(pending.size());
    auto restoreBackup = [](const CommittedWrite& item) {
        std::error_code error;
        fs::remove(item.target, error);
        if (error) return false;
        error.clear();
        fs::rename(item.backup, item.target, error);
        return !error;
    };
    auto rollback = [&]() {
        bool restored = true;
        for (auto it = committed.rbegin(); it != committed.rend(); ++it) {
            if (!restoreBackup(*it)) restored = false;
        }
        return restored;
    };

    for (const auto& item : pending) {
        std::error_code renameError;
        fs::rename(item.target, item.backup, renameError);
        if (renameError) {
            const bool rolledBack = rollback();
            removeTemporaryFiles();
            std::cerr << "lightmap: cannot stage " << item.target << " (" << renameError.message()
                      << "); rollback " << (rolledBack ? "completed" : "failed") << "\n";
            return 3;
        }

        renameError.clear();
        fs::rename(item.temporary, item.target, renameError);
        if (renameError) {
            const bool currentRestored = restoreBackup({item.target, item.backup});
            const bool priorRolledBack = rollback();
            removeTemporaryFiles();
            std::cerr << "lightmap: cannot commit " << item.target << " (" << renameError.message()
                      << "); rollback " << (currentRestored && priorRolledBack ? "completed" : "failed") << "\n";
            return 3;
        }
        committed.push_back({item.target, item.backup});
    }

    for (const auto& item : committed) {
        std::error_code cleanupError;
        fs::remove(item.backup, cleanupError);
        if (cleanupError)
            std::cerr << "lightmap: warning: could not remove backup " << item.backup << " ("
                      << cleanupError.message() << ")\n";
    }

    for (size_t i = 0; i < pending.size(); ++i) {
        const auto& item = pending[i];
        std::cout << "  block " << i << " factor=(" << item.factor.x << "," << item.factor.y << ","
                   << item.factor.z << ") -> " << item.target.filename().string() << "\n";
    }
    std::cout << "lightmap: recalc wrote " << pending.size() << "/" << blockCount << " .olm file(s)\n";
    return 0;
}

} // namespace

int cmd_lightmap(int argc, char** argv)
{
    if (argc < 2)
    {
        print_lightmap_help();
        return 1;
    }

    std::string subcmd = argv[1];
    if (subcmd == "--help" || subcmd == "-h")
    {
        print_lightmap_help();
        return 0;
    }

    LightmapArgs args;
    if (subcmd == "list")
    {
        if (!ParseLightmapArgs(argc, argv, 2, args)) { print_lightmap_help(); return 1; }
        return do_lightmap_list(args);
    }
    else if (subcmd == "resolve")
    {
        if (!ParseLightmapArgs(argc, argv, 2, args)) { print_lightmap_help(); return 1; }
        return do_lightmap_resolve(args);
    }
    else if (subcmd == "recalc")
    {
        RecalcArgs ra;
        for (int i = 2; i < argc; ++i) {
            std::string arg = argv[i];
            auto next = [&](const char* flag) -> std::string {
                return (i + 1 < argc) ? std::string(argv[++i]) : std::string();
            };
            if (arg == "--model")          ra.model = next(arg.c_str());
            else if (arg == "--qsc")       ra.qscPath = next(arg.c_str());
            else if (arg == "--mef")       ra.mefPath = next(arg.c_str());
            else if (arg == "--task-id") { ra.taskId = std::atoi(next(arg.c_str()).c_str()); ra.hasTaskId = true; }
            else if (arg == "--rot-orig") { if (!ParseVec3(next(arg.c_str()), ra.rotOrig)) { std::cerr << "lightmap: bad --rot-orig\n"; return 1; } }
            else if (arg == "--rot-new")  { if (!ParseVec3(next(arg.c_str()), ra.rotNew))  { std::cerr << "lightmap: bad --rot-new\n"; return 1; } }
            else if (arg == "--sun-dir")  { if (!ParseVec3(next(arg.c_str()), ra.sunDir))  { std::cerr << "lightmap: bad --sun-dir\n"; return 1; } ra.hasSunDir = true; }
            else if (arg == "--sun-color"){ if (!ParseVec3(next(arg.c_str()), ra.sunColor)){ std::cerr << "lightmap: bad --sun-color\n"; return 1; } }
            else if (arg == "--ambient")  { if (!ParseVec3(next(arg.c_str()), ra.ambient)) { std::cerr << "lightmap: bad --ambient\n"; return 1; } }
        }
        if (ra.model.empty() || ra.qscPath.empty()) {
            std::cerr << "lightmap: recalc requires --model and --qsc\n";
            print_lightmap_help();
            return 1;
        }
        return do_lightmap_recalc(ra);
    }

    std::cerr << "lightmap: unknown subcommand '" << subcmd << "'\n";
    print_lightmap_help();
    return 1;
}
