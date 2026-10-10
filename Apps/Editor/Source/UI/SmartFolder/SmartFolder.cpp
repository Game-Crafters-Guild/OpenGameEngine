#include "UI/SmartFolder/SmartFolder.h"
#include <random>
#include <sstream>
#include <iomanip>

namespace GameEngine {

std::string GenerateSmartFolderId()
{
    // Generate a simple UUID-like identifier
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);
    
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(8) << dist(gen) << "-";
    oss << std::setw(4) << (dist(gen) & 0xFFFF) << "-";
    oss << std::setw(4) << ((dist(gen) & 0x0FFF) | 0x4000) << "-"; // Version 4
    oss << std::setw(4) << ((dist(gen) & 0x3FFF) | 0x8000) << "-"; // Variant
    oss << std::setw(8) << dist(gen) << std::setw(4) << (dist(gen) & 0xFFFF);
    
    return oss.str();
}

} // namespace GameEngine
