// qas_parser.cpp - IGI QAS AI script decompiler
// Parses binary AI pathing/action scripts from IGI.exe

#include "qas_parser.h"
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>

namespace igi {

// QAS instruction opcodes (from IGI.exe AI system)
enum class QASOp : uint8_t {
    NOP         = 0x00,
    MOVE_TO     = 0x01,
    RUN_TO      = 0x02,
    FIRE_AT     = 0x03,
    PATROL      = 0x04,
    COMBAT      = 0x05,
    IDLE        = 0x06,
    FLAT        = 0x07,
    STUNNED     = 0x08,
    PANIC       = 0x09,
    LOOK_AT     = 0x0A,
    PLAY_ANIM   = 0x0B,
    PLAY_SOUND  = 0x0C,
    ACTIVATE    = 0x0D,
    KICK_GRENADE = 0x0E,
    DEAD        = 0x0F,
    WALK_NODE   = 0x10,
    RUN_NODE    = 0x11,
};

static const char* qas_op_name(uint8_t op) {
    switch ((QASOp)op) {
        case QASOp::NOP: return "NOP";
        case QASOp::MOVE_TO: return "MOVE_TO";
        case QASOp::RUN_TO: return "RUN_TO";
        case QASOp::FIRE_AT: return "FIRE_AT";
        case QASOp::PATROL: return "PATROL";
        case QASOp::COMBAT: return "COMBAT";
        case QASOp::IDLE: return "IDLE";
        case QASOp::FLAT: return "FLAT";
        case QASOp::STUNNED: return "STUNNED";
        case QASOp::PANIC: return "PANIC";
        case QASOp::LOOK_AT: return "LOOK_AT";
        case QASOp::PLAY_ANIM: return "PLAY_ANIM";
        case QASOp::PLAY_SOUND: return "PLAY_SOUND";
        case QASOp::ACTIVATE: return "ACTIVATE";
        case QASOp::KICK_GRENADE: return "KICK_GRENADE";
        case QASOp::DEAD: return "DEAD";
        case QASOp::WALK_NODE: return "WALK_NODE";
        case QASOp::RUN_NODE: return "RUN_NODE";
        default: return "UNKNOWN";
    }
}

int decompile_qas(const std::string& input_path, const std::string& output_path) {
    std::ifstream infile(input_path, std::ios::binary);
    if (!infile) {
        fprintf(stderr, "Error: Cannot open input file %s\n", input_path.c_str());
        return 1;
    }

    infile.seekg(0, std::ios::end);
    size_t size = infile.tellg();
    infile.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> data(size);
    infile.read(reinterpret_cast<char*>(data.data()), size);
    infile.close();

    FILE* out = fopen(output_path.c_str(), "w");
    if (!out) {
        fprintf(stderr, "Error: Cannot create output file %s\n", output_path.c_str());
        return 1;
    }

    fprintf(out, "; QAS AI Script Decompiled\n");
    fprintf(out, "; Source: %s\n\n", input_path.c_str());

    size_t pos = 0;
    while (pos < size) {
        uint8_t op = data[pos];
        fprintf(out, "%04zu  %s", pos, qas_op_name(op));
        
        // Parse instruction-specific operands
        switch ((QASOp)op) {
            case QASOp::MOVE_TO:
            case QASOp::RUN_TO:
            case QASOp::FIRE_AT:
                if (pos + 5 <= size) {
                    uint32_t x = data[pos+1] | (data[pos+2] << 8);
                    uint32_t y = data[pos+3] | (data[pos+4] << 8);
                    fprintf(out, "  (%u, %u)", x, y);
                    pos += 5;
                } else { pos++; }
                break;
            case QASOp::PATROL:
                if (pos + 3 <= size) {
                    uint16_t start = data[pos+1] | (data[pos+2] << 8);
                    fprintf(out, "  start_node=%u", start);
                    pos += 3;
                } else { pos++; }
                break;
            case QASOp::PLAY_ANIM:
            case QASOp::PLAY_SOUND:
                if (pos + 2 <= size) {
                    fprintf(out, "  id=%u", data[pos+1]);
                    pos += 2;
                } else { pos++; }
                break;
            default:
                pos++;
                break;
        }
        fprintf(out, "\n");
    }

    fclose(out);
    printf("Decompiled %s -> %s\n", input_path.c_str(), output_path.c_str());
    return 0;
}

} // namespace igi
