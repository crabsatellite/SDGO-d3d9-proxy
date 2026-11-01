// Room-scoped random loadout policy. No game pointers, SQL or process state.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace sdgo::random_loadout {
constexpr int kBudget = 10;
constexpr int kMaxUnits = 4;
constexpr size_t kMaxCandidates = 4096;

struct Candidate {
    uint32_t owner = 0;
    uint32_t unit = 0;
    uint64_t serial = 0;
    uint8_t rank = 0;
    uint8_t points = 0;
    bool eligible = false;
    std::array<uint8_t, 0x99> data{};
};

struct Restrictions {
    uint8_t minimumRank = 1;
    uint8_t maximumRank = 4;
    bool onlyC = false;
};

enum class Error { None, NoEligibleUnit, NoExactDeck, TooManyCandidates };
struct Selection {
    Error error = Error::None;
    std::vector<Candidate> units;
    bool alternativeToPrevious = false;
};

inline bool Command(const char* title, size_t length) {
    if (!title) return false;
    for (size_t pos = 0; pos < length;) {
        while (pos < length && (title[pos] == ' ' || title[pos] == '\t')) ++pos;
        size_t end = pos;
        while (end < length && title[end] != ' ' && title[end] != '\t' && title[end] != '\0') ++end;
        for (const char* word : {"!rand", "!random"}) {
            size_t n = 0;
            while (word[n]) ++n;
            if (end - pos != n) continue;
            bool same = true;
            for (size_t i = 0; i < n; ++i) {
                char ch = title[pos + i];
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
                if (ch != word[i]) { same = false; break; }
            }
            if (same) return true;
        }
        if (end >= length || title[end] == '\0') break;
        pos = end + 1;
    }
    return false;
}

inline std::vector<uint32_t> Identity(const std::vector<Candidate>& units) {
    std::vector<uint32_t> result;
    for (const auto& u : units) result.push_back(u.unit);
    std::sort(result.begin(), result.end());
    return result;
}

template<class Random>
Selection Choose(const std::vector<Candidate>& inventory, uint32_t owner,
                 Restrictions restriction, bool deck, std::vector<uint32_t> previous, Random& rng) {
    Selection result;
    std::map<uint32_t, std::vector<const Candidate*>> groups;
    std::set<uint64_t> serials;
    for (const auto& u : inventory) {
        if (!u.eligible || u.owner != owner || !u.serial || !u.unit || u.points < 1 || u.points > kBudget
            || u.rank < restriction.minimumRank || u.rank > restriction.maximumRank
            || (restriction.onlyC && u.rank != 1)) continue;
        if(serials.insert(u.serial).second) groups[u.unit].push_back(&u);
    }
    if (groups.empty()) { result.error = Error::NoEligibleUnit; return result; }
    std::vector<Candidate> pool;
    for (const auto& entry : groups) {
        const auto& copies = entry.second;
        if(deck) {
            for(const auto* copy:copies) pool.push_back(*copy);
        } else {
            size_t pick = std::uniform_int_distribution<size_t>(0, copies.size()-1)(rng);
            pool.push_back(*copies[pick]);
        }
    }
    if (pool.size() > kMaxCandidates) { result.error = Error::TooManyCandidates; return result; }
    std::sort(previous.begin(), previous.end());
    if (!deck) {
        std::vector<size_t> choices;
        for (size_t i = 0; i < pool.size(); ++i)
            if (previous.size() != 1 || pool[i].unit != previous[0]) choices.push_back(i);
        result.alternativeToPrevious = !choices.empty();
        if (choices.empty()) choices.push_back(0);
        result.units.push_back(pool[choices[std::uniform_int_distribution<size_t>(0, choices.size()-1)(rng)]]);
        return result;
    }

    // Count all unique inventory-copy subsets, up to four slots, exactly 10 points.
    // Same models are allowed when backed by different owned serials, just as in
    // the native ship setter. With <=4096 copies the counts fit in uint64_t.
    using Counts = std::array<std::array<uint64_t, kBudget+1>, kMaxUnits+1>;
    std::vector<Counts> suffix(pool.size()+1);
    suffix.back()[0][0] = 1;
    for (size_t i = pool.size(); i-- > 0;) {
        suffix[i] = suffix[i+1];
        const int p = pool[i].points;
        for (int k = 1; k <= kMaxUnits; ++k)
            for (int cost = p; cost <= kBudget; ++cost)
                suffix[i][k][cost] += suffix[i+1][k-1][cost-p];
    }
    uint64_t total = 0;
    for (int k = 1; k <= kMaxUnits; ++k) total += suffix[0][k][kBudget];
    if (!total) { result.error = Error::NoExactDeck; return result; }

    std::map<uint32_t,std::vector<size_t>> positions;
    for(size_t i=0;i<pool.size();++i) positions[pool[i].unit].push_back(i);
    std::map<uint32_t,unsigned> oldNeed;
    for(uint32_t id:previous) ++oldNeed[id];
    auto combinations=[](size_t n,unsigned k) {
        if(n<k) return uint64_t{0};
        uint64_t count=1;
        for(unsigned i=1;i<=k;++i) count=count*(n-i+1)/i;
        return count;
    };
    auto blocked=[&](size_t from,const std::map<uint32_t,unsigned>& need) {
        uint64_t count=1;
        for(const auto& entry:need) {
            const auto found=positions.find(entry.first);
            if(found==positions.end()) return uint64_t{0};
            const auto& indices=found->second;
            const size_t available=static_cast<size_t>(indices.end()-std::lower_bound(indices.begin(),indices.end(),from));
            count*=combinations(available,entry.second);
            if(!count) break;
        }
        return count;
    };
    bool exclude = !previous.empty() && previous.size() <= kMaxUnits;
    int oldCost = 0;
    for (uint32_t id : previous) {
        auto it = std::lower_bound(pool.begin(), pool.end(), id,
                                  [](const Candidate& u, uint32_t key) { return u.unit < key; });
        if (it == pool.end() || it->unit != id) { exclude = false; break; }
        oldCost += it->points;
    }
    const uint64_t oldCount=exclude && oldCost==kBudget ? blocked(0,oldNeed) : 0;
    exclude=oldCount>0 && total>oldCount;
    result.alternativeToPrevious=oldCount==0 || total>oldCount;
    uint64_t rank=std::uniform_int_distribution<uint64_t>(0,total-(exclude?oldCount:0)-1)(rng);
    int count = 1;
    for(;count<=kMaxUnits;++count) {
        const uint64_t ways=suffix[0][count][kBudget] - (exclude && static_cast<size_t>(count)==previous.size()?oldCount:0);
        if(rank<ways) break;
        rank-=ways;
    }
    bool stillOld=exclude && static_cast<size_t>(count)==previous.size();
    auto needed=oldNeed;
    int remaining = kBudget;
    for (size_t i = 0; i < pool.size() && count; ++i) {
        const uint64_t skip = suffix[i+1][count][remaining] - (stillOld?blocked(i+1,needed):0);
        if (rank < skip) continue;
        rank -= skip;
        if(stillOld) {
            auto need=needed.find(pool[i].unit);
            if(need==needed.end() || !need->second) stillOld=false;
            else if(--need->second==0) needed.erase(need);
        }
        result.units.push_back(pool[i]);
        remaining -= pool[i].points;
        --count;
    }
    std::shuffle(result.units.begin(), result.units.end(), rng);
    return result;
}
} // namespace sdgo::random_loadout
