// Native packed _UnitData (0x99 bytes), shared by the lobby and battle setup.
// Historical/unused reserved bytes stay zero; no statistics are invented.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace sdgo::owned_unit_record {
inline uint16_t WordBits(int64_t value) {
    // SQL SMALLINT stores RGB565 and packed custom values as signed words.
    if(value < -32768 || value > 65535) throw std::runtime_error("Inventory word outside 16-bit bounds");
    return static_cast<uint16_t>(value);
}
struct Row {
    uint32_t unit = 0;
    uint16_t slot = 0;
    uint64_t serial = 0;
    uint8_t level = 0, selected = 0, saleAllowed = 1;
    uint16_t validity = 0;
    uint32_t kills = 0;
    std::array<uint16_t,6> colours{};
    std::array<uint32_t,3> decals{};
    uint16_t shine = 0;
    float energy = 0;
    uint32_t battles = 0, skill = 0;
    std::array<uint8_t,16> acquired{}, expires{}; // native SQL_TIMESTAMP_STRUCT
    uint32_t category = 0, shopIndex = 0, experience = 0;
    uint8_t shop = 0, runOut = 0, forbidden = 0, timeLimit = 0;
    uint16_t deckCount = 0, resetCount = 0;
    uint8_t normalMax = 0, locked = 0;
    std::array<uint16_t,4> customSkills{};
    uint8_t unitClass = 0, classMark = 0, composeExp = 0, composeRepair = 0, composeSkill = 0;
};
template<class T> inline void Put(std::array<uint8_t,0x99>& dst, size_t offset, const T& value) {
    std::memcpy(dst.data()+offset, &value, sizeof(value));
}
inline std::array<uint8_t,0x99> Pack(const Row& r) {
    std::array<uint8_t,0x99> out{};
    Put(out,0,r.unit); Put(out,4,r.slot); Put(out,6,r.selected); Put(out,7,r.saleAllowed);
    Put(out,8,r.serial); Put(out,16,r.level);
    Put(out,17,r.kills);
    for (size_t i=0;i<6;++i) Put(out,46+2*i,r.colours[i]);
    Put(out,58,r.shine);
    for (size_t i=0;i<3;++i) Put(out,60+4*i,r.decals[i]);
    Put(out,72,r.validity);
    Put(out,74,r.energy); Put(out,78,r.battles); Put(out,82,r.skill);
    Put(out,86,r.acquired); Put(out,102,r.expires); Put(out,118,r.category);
    Put(out,122,r.shop); Put(out,123,r.runOut); Put(out,124,r.forbidden); Put(out,125,r.timeLimit);
    Put(out,126,r.shopIndex); Put(out,130,r.deckCount); Put(out,132,r.experience);
    Put(out,136,r.normalMax); Put(out,137,r.locked);
    for (size_t i=0;i<4;++i) Put(out,138+2*i,r.customSkills[i]);
    Put(out,146,r.resetCount); Put(out,148,r.unitClass); Put(out,149,r.classMark);
    Put(out,150,r.composeExp); Put(out,151,r.composeRepair); Put(out,152,r.composeSkill);
    return out;
}
}
