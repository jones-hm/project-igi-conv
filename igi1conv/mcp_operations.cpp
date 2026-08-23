#include "mcp_operations.h"

#include <algorithm>

namespace igi1conv {
namespace {

struct OperationSeed {
    const char* name;
    const char* description;
    bool writesGame;
    std::initializer_list<const char*> commandPrefix;
};

const std::vector<GameOperation>& BuildOperations() {
    static const std::vector<GameOperation> operations = [] {
        const OperationSeed seeds[] = {
            {"dat.export", "Export game model/texture mappings for inspection or editing", false, {"dat", "export"}},
            {"dat.info", "Inspect a game DAT mapping", false, {"dat", "info"}},
            {"dat.to-mtp", "Compile game DAT mappings into an MTP package", true, {"dat", "to-mtp"}},
            {"fnt.export", "Export a game font atlas for inspection", false, {"fnt", "export"}},
            {"fnt.info", "Inspect a game font", false, {"fnt", "info"}},
            {"graph.dump", "Inspect game AI navigation graph records", false, {"graph", "dump"}},
            {"graph.export", "Export game AI navigation data for inspection or editing", false, {"graph", "export"}},
            {"graph.info", "Inspect game AI navigation metadata", false, {"graph", "info"}},
            {"graph.table", "Export game AI navigation data as a table", false, {"graph", "table"}},
            {"iff.convert", "Convert game skeletal animation data to BEF assets", true, {"iff", "convert"}},
            {"iff.create", "Create a game IFF animation from BEF assets", true, {"iff", "create"}},
            {"iff.decompile", "Decompile game IFF animation data", false, {"iff", "decompile"}},
            {"iff.emit-qsc", "Generate game animation QSC data from BEF assets", true, {"iff", "emit-qsc"}},
            {"iff.info", "Inspect game skeletal animation metadata", false, {"iff", "info"}},
            {"iff.rebuild", "Rebuild a game IFF animation", true, {"iff", "rebuild"}},
            {"iff.test", "Validate game skeletal animation data", false, {"iff", "test"}},
            {"lightmap.list", "List game lightmap bindings for placed models", false, {"lightmap", "list"}},
            {"lightmap.recalc", "Recalculate game lightmaps after a placement rotation change", true, {"lightmap", "recalc"}},
            {"lightmap.resolve", "Resolve game lightmap files for a placed model", false, {"lightmap", "resolve"}},
            {"mef.build-rigid", "Build a game rigid mesh from an attached model", true, {"mef", "build-rigid"}},
            {"mef.bundle", "Bundle a game mesh with its game textures", true, {"mef", "bundle"}},
            {"mef.compile", "Compile edited text mesh data into a game MEF", true, {"mef", "compile"}},
            {"mef.dump", "Inspect game mesh structure", false, {"mef", "dump"}},
            {"mef.export", "Export game mesh geometry for inspection or editing", false, {"mef", "export"}},
            {"mef.info", "Inspect game mesh metadata", false, {"mef", "info"}},
            {"mef.to-text", "Convert a game mesh into editable text", false, {"mef", "to-text"}},
            {"mtp.dump", "Inspect game model/texture package mappings", false, {"mtp", "dump"}},
            {"mtp.info", "Inspect a game MTP package", false, {"mtp", "info"}},
            {"mtp.repair", "Repair game MTP mapping counts", true, {"mtp", "repair"}},
            {"mtp.to-dat", "Compile a game MTP package into DAT mappings", true, {"mtp", "to-dat"}},
            {"olm.from-png", "Build a game OLM lightmap from edited image data", true, {"olm", "from-png"}},
            {"olm.info", "Inspect a game OLM lightmap", false, {"olm", "info"}},
            {"olm.to-png", "Export a game OLM lightmap for inspection or editing", false, {"olm", "to-png"}},
            {"olm.to-tga", "Export a game OLM lightmap for inspection or editing", false, {"olm", "to-tga"}},
            {"qsc.compile", "Compile edited game task/script source into QVM", true, {"qsc", "compile"}},
            {"qsc.edit-object", "Edit game object task parameters such as position, rotation, model, team, or AI fields", true, {"qsc", "edit-object"}},
            {"qsc.list-objects", "List game object task parameters for safe selection", false, {"qsc", "list-objects"}},
            {"qsc.validate", "Validate game task/script source", false, {"qsc", "validate"}},
            {"qvm.decompile", "Decompile game script bytecode for editing", false, {"qvm", "decompile"}},
            {"qvm.disasm", "Inspect game script bytecode instructions", false, {"qvm", "disasm"}},
            {"qvm.info", "Inspect game script bytecode metadata", false, {"qvm", "info"}},
            {"res.append", "Append edited game assets to a resource archive", true, {"res", "append"}},
            {"res.compile", "Compile a game resource script into an archive", true, {"res", "compile"}},
            {"res.extract", "Extract game resource archive entries for editing", false, {"res", "extract"}},
            {"res.list", "List game resource archive entries", false, {"res", "list"}},
            {"res.pack", "Pack edited game assets into a resource archive", true, {"res", "pack"}},
            {"res.repack", "Repack a game resource archive while preserving entry names", true, {"res", "repack"}},
            {"res.unpack", "Unpack a game resource archive for editing", false, {"res", "unpack"}},
            {"terrain.export-ctr", "Export game terrain cube data for inspection or editing", false, {"terrain", "export-ctr"}},
            {"terrain.export-lmp", "Export game terrain light data for inspection or editing", false, {"terrain", "export-lmp"}},
            {"terrain.info", "Inspect game terrain data", false, {"terrain", "info"}},
            {"tex.decode", "Export game texture images for inspection or editing", false, {"tex", "decode"}},
            {"tex.info", "Inspect game texture metadata", false, {"tex", "info"}},
            {"tex.to-png", "Export game texture pixels for inspection or editing", false, {"tex", "to-png"}},
            {"tex.to-spr", "Build a game SPR texture from edited image data", true, {"tex", "to-spr"}},
            {"tex.to-tga", "Export game texture pixels for inspection or editing", false, {"tex", "to-tga"}},
            {"test.run", "Validate a game directory with the converter test suite", false, {"test"}},
            {"wav.convert", "Convert game audio data to standard WAV for inspection", false, {"wav", "convert"}},
            {"wav.convert-dir", "Convert a directory of game audio data for inspection", false, {"wav", "convert-dir"}},
            {"wav.info", "Inspect game audio metadata", false, {"wav", "info"}},
        };

        std::vector<GameOperation> result;
        result.reserve(std::size(seeds));
        for (const auto& seed : seeds) {
            GameOperation operation;
            operation.name = seed.name;
            operation.description = seed.description;
            operation.writesGame = seed.writesGame;
            for (const char* part : seed.commandPrefix)
                operation.commandPrefix.emplace_back(part);
            result.push_back(std::move(operation));
        }
        std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
            return left.name < right.name;
        });
        return result;
    }();
    return operations;
}

} // namespace

const std::vector<GameOperation>& GameOperations() {
    return BuildOperations();
}

bool IsAllowedGameCommand(const std::vector<std::string>& argv, std::string& error) {
    error.clear();
    if (argv.empty()) {
        error = "game command must not be empty";
        return false;
    }

    std::vector<std::string> normalized = argv;
    if (normalized.front() == "mex")
        normalized.front() = "mef";

    for (const auto& operation : GameOperations()) {
        if (normalized.size() < operation.commandPrefix.size())
            continue;
        if (std::equal(operation.commandPrefix.begin(), operation.commandPrefix.end(), normalized.begin()))
            return true;
    }

    error = "command is not an allowed game operation: " + argv.front();
    return false;
}

} // namespace igi1conv
