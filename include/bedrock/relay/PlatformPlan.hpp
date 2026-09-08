#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <queue>
#include <set>
#include <vector>

// No game pointers here: geometry and walking can be tested without Minecraft.
namespace bedrock::platform {
struct Pos {
    int x = 0, y = 0, z = 0;
    bool operator==(const Pos&) const = default;
    bool operator<(const Pos& b) const {
        return std::array{x, y, z} < std::array{b.x, b.y, b.z};
    }
};
enum Material { Quartz, Glowstone, Chest, Workbench, None };
struct Task { Pos pos; Material material; };
// Protected by PlatformBuilder::eventsMutex. Recording never arms this gate.
// Accept ContainerOpen in the receive hook so a following InventoryContent in
// the SAME batch can be read, without waiting for the next game tick.
struct InventoryReadGate {
    bool awaitingSupply = false;
    Pos expected{};
    int supplyWindow = -1, sourceSlot = -1, destinationSlot = -1;
    void reset() { *this = {}; }
    void clearTransfer() { sourceSlot = destinationSlot = -1; }
    void beginSupply(Pos position) { reset(); expected = position; awaitingSupply = true; }
    static bool matchesSupply(int window, int type, Pos position, Pos target) {
        const auto dx = int64_t(position.x) - target.x, dz = int64_t(position.z) - target.z;
        return window > 0 && window <= 100 && type == 0 && position.y == target.y &&
            std::abs(dx) + std::abs(dz) <= 1; // opposite half of a double chest
    }
    bool observeOpen(int window, int type, Pos position) {
        const bool accepted = awaitingSupply && matchesSupply(window, type, position, expected);
        awaitingSupply = false;
        supplyWindow = accepted ? window : -1;
        clearTransfer();
        return accepted;
    }
    void observeClose(int window) {
        if (window == supplyWindow) { supplyWindow = -1; clearTransfer(); }
        if (window == 0) clearTransfer();
    }
    void watchTransfer(int window, int source, int destination) {
        clearTransfer();
        if (window != 0 && (window < 1 || window != supplyWindow)) return;
        sourceSlot = window == 0 && source >= 0 && source < 36 ? source : -1;
        destinationSlot = destination >= 0 && destination < 36 ? destination : -1;
    }
    bool wantsContent(int window) const {
        return window == 0 ? sourceSlot >= 0 || destinationSlot >= 0 : window > 0 && window == supplyWindow;
    }
    bool wantsSlot(int window, int slot) const {
        if (slot < 0) return false;
        return window == 0 ? slot < 36 && (slot == sourceSlot || slot == destinationSlot) :
            slot < 54 && window > 0 && window == supplyWindow;
    }
};
inline int placementTicks(int milliseconds) { return (std::clamp(milliseconds,50,2000)+49)/50; }
inline int placementDelayMs(bool fast,int configured) { return fast?50:std::clamp(configured,50,2000); }
inline int equipSettleTicks(bool fast) { return fast?1:2; }
inline bool placementReady(bool acknowledged,uint64_t tick,uint64_t notBefore,bool matchingWorldBlock) {
    return acknowledged && tick>=notBefore && matchingWorldBlock;
}
// Only drain local bookkeeping (already-built blocks). The callback must stop
// on a send, changed stage or a pending delay. Never batch unconfirmed blocks.
template<class Step> void advanceBuilding(bool fast,Step step) {
    const int budget=fast?12:1; // a row contains at most ten tasks
    for (int i=0;i<budget;++i) if (!step()) break;
}
inline bool sameYaw(float a, float b) {
    return std::isfinite(a) && std::isfinite(b) && std::abs(std::remainder(a-b,360.f))<=1.f;
}
// Latest authoritative values, not sticky flags: a correction invalidates an
// earlier acknowledgement. Both ends of a whole-stack transfer must match.
struct TransferBarrier {
    int sourceWindow=-1, sourceSlot=-1, destinationSlot=-1, material=-1, count=0;
    bool sourceEmpty=false, destinationMatches=false, sourceSeen=false, destinationSeen=false;
    void begin(int window,int source,int destination,int item,int amount) {
        sourceWindow=window; sourceSlot=source; destinationSlot=destination; material=item; count=amount;
        sourceEmpty=destinationMatches=sourceSeen=destinationSeen=false;
    }
    void observe(int window,int slot,int item,int amount) {
        if (window==sourceWindow && slot==sourceSlot) { sourceSeen=true; sourceEmpty=amount==0; }
        if (window==0 && slot==destinationSlot) { destinationSeen=true; destinationMatches=item==material && amount==count && count>0; }
    }
    bool ready() const { return sourceEmpty && destinationMatches; }
    bool corrected() const { return (sourceSeen && !sourceEmpty) || (destinationSeen && !destinationMatches); }
    bool mayPredict(bool dispatched,bool settled) const { return dispatched && settled && !corrected(); }
};
// Bedrock 1.21.2 input bits. Preserve all unrelated movement/interaction bits.
inline uint64_t holdSneak(uint64_t flags, bool start) {
    flags |= (uint64_t(1)<<8) | (uint64_t(1)<<9) | (uint64_t(1)<<24);
    flags &= ~((uint64_t(1)<<28) | (uint64_t(1)<<23));
    if (start) flags |= uint64_t(1)<<27;
    return flags;
}
inline bool heldSneak(uint64_t flags) {
    return (flags & (uint64_t(1)<<8)) && !(flags & (uint64_t(1)<<28));
}
inline float walkingSpeed(float speed) {
    return std::isfinite(speed) ? std::clamp(speed,0.04f,0.20f) : 0.12f;
}
inline std::vector<Pos> supplyApproaches(Pos chest, int floorY) {
    std::vector<Pos> result;
    // Upper chests use the same walkable floor as lower chests, not their lid.
    if (chest.y < floorY-1 || chest.y > floorY+3) return result;
    for (Pos offset : {Pos{1,0,0},Pos{-1,0,0},Pos{0,0,1},Pos{0,0,-1},
                      Pos{2,0,0},Pos{-2,0,0},Pos{0,0,2},Pos{0,0,-2},
                      Pos{1,0,1},Pos{-1,0,1},Pos{1,0,-1},Pos{-1,0,-1}})
        result.push_back({chest.x+offset.x,floorY,chest.z+offset.z});
    return result;
}
inline int resourceById(const std::array<int,4>& ids,int id,int aux) {
    if (id<=0 || aux!=0) return -1;
    for (int i=0;i<4;++i) if (ids[i]==id) return i;
    return -1;
}
inline int64_t horizontalDistance(Pos a, Pos b) {
    return std::abs(int64_t(a.x)-b.x)+std::abs(int64_t(a.z)-b.z);
}
// Removal changes only the recorded coordinates, never blocks in the world.
// 0 explicitly means the entire current-world list; positive indices are 1-based.
inline bool removeRecordedSupply(std::vector<Pos>& list, int index) {
    if (list.empty() || index<0 || size_t(index)>list.size()) return false;
    if (index==0) list.clear(); else list.erase(list.begin()+index-1);
    return true;
}
inline std::vector<Pos> straightSegment(Pos start, Pos goal,
    const std::function<bool(Pos)>& walkable, int limit=16) {
    if (start.y!=goal.y || (start.x!=goal.x && start.z!=goal.z) || !walkable(start)) return {};
    std::vector<Pos> result{start};
    const Pos delta{(goal.x>start.x)-(goal.x<start.x),0,(goal.z>start.z)-(goal.z<start.z)};
    while (!(result.back()==goal) && result.size()<=size_t(std::clamp(limit,1,32))) {
        const auto p=result.back();
        const Pos next{p.x+delta.x,p.y,p.z+delta.z};
        if (!walkable(next)) break; // never step into unloaded/unknown terrain
        result.push_back(next);
    }
    if (result.size()==1 && !(start==goal)) return {};
    return result;
}
inline int chunk(int coordinate) {
    const int q = coordinate / 16;
    return q - (coordinate % 16 < 0 ? 1 : 0);
}
inline Pos direction(float yaw) {
    int quadrant = static_cast<int>(std::floor((std::remainder(yaw, 360.f) + 45.f) / 90.f));
    quadrant = (quadrant % 4 + 4) % 4;
    constexpr Pos directions[] = {{0,0,1},{-1,0,0},{0,0,-1},{1,0,0}};
    return directions[quadrant];
}
struct Plan {
    Pos origin; // floor below the player's feet, never the eye position
    Pos forward;
    int side = 1; // right; -1 = left
    int stations = 16;
    int lanes = 1, gapChunks = 2, turnSide = -1;
    int laneLength() const { return std::clamp(stations,1,1024)*16; }
    int gapLength() const { return std::clamp(gapChunks,1,16)*16; }
    Pos heading(int distance) const {
        if(lanes<=1)return forward;
        const int cycle=laneLength()+gapLength(),lane=distance/cycle,local=distance%cycle;
        if(local>=laneLength())return {-forward.z*turnSide,0,forward.x*turnSide};
        return lane%2?Pos{-forward.x,0,-forward.z}:forward;
    }
    Pos at(int distance, int lateral = 0, int up = 0) const {
        if(lanes>1) {
            const int cycle=laneLength()+gapLength(),lane=distance/cycle,local=distance%cycle;
            const int along=local<laneLength()?(lane%2?laneLength()-1-local:local):(lane%2?0:laneLength()-1);
            const int across=lane*gapLength()+(local<laneLength()?0:local-laneLength()+1);
            const auto dir=heading(distance);
            return {origin.x+forward.x*along-forward.z*turnSide*across-dir.z*lateral*side,
                origin.y+up,origin.z+forward.z*along+forward.x*turnSide*across+dir.x*lateral*side};
        }
        return {origin.x + forward.x * distance - forward.z * lateral * side,
                origin.y + up,
                origin.z + forward.z * distance + forward.x * lateral * side};
    }
    int firstStation() const {
        // Both halves lie along the travel axis, inside a single chunk. At a
        // negative boundary C++ truncating division must NOT be used.
        for (int d = 2; d < 34; ++d) {
            const auto p = at(d, 2);
            const int c = forward.x ? p.x : p.z;
            const int local = c - chunk(c) * 16;
            const int progress = forward.x + forward.z > 0 ? local : 15 - local;
            if (progress == 4) return d;
        }
        return 2;
    }
    int rows() const { return lanes>1?std::clamp(lanes,2,32)*laneLength()+(std::clamp(lanes,2,32)-1)*gapLength():
        firstStation() + (std::clamp(stations, 1, 1024) - 1) * 16 + 4; }
    std::vector<Task> row(int distance) const {
        std::vector<Task> tasks;
        if (distance < 0 || distance >= rows()) return tasks;
        // Place a supported center first, then expand sideways. One bounded
        // row is retained, not a growing copy of the entire construction.
        for (int lateral : {0, -1, 1, -2, 2})
            tasks.push_back({at(distance, lateral), lateral == 0 ? Glowstone : Quartz});
        const int station = distance - 2;
        bool addStation=station >= firstStation() && (station-firstStation())%16==0;
        if(lanes>1) {
            addStation=false;
            const int local=distance%(laneLength()+gapLength());
            if(local>=4 && local<laneLength()-3) {
                const auto d=heading(distance),p=at(station,2);
                const int c=d.x?p.x:p.z,progress=(d.x+d.z>0)?c-chunk(c)*16:15-(c-chunk(c)*16);
                const auto other=at(station+1,2);
                addStation=progress==4 && chunk(p.x)==chunk(other.x) && chunk(p.z)==chunk(other.z);
            }
        }
        if (addStation) {
            for (int up : {1, 2})
                for (int half : {0, 1}) tasks.push_back({at(station + half, 2, up), Chest});
            tasks.push_back({at(distance, 2, 1), Workbench});
        }
        return tasks;
    }
};

// Adapted from weathertop_bot/plugins/pathfinderCore.js: Manhattan A*, four
// neighbors, conservative missing chunks, bounded generated/queued nodes and
// cooperative yielding. DLL ticks replace JS await sleep(0). Flat mode disables
// the bot's drops/swimming/flight: a platform above the void has no safe drop.
class Search {
    struct Node { Pos pos; int g; int64_t f; bool operator<(const Node& b) const { return f > b.f; } };
    std::priority_queue<Node> open;
    std::map<Pos, int> cost;
    std::map<Pos, Pos> parent;
    Pos start{}, goal{};
    int expanded = 0, maxNodes = 32768;
    int segmentRadius = 0; // 0 = strict full route; >0 = bounded loaded segment
    Pos best{};
    std::map<Pos,bool> walkability;
    bool complete = false;
    std::vector<Pos> result;
    static int64_t distance(Pos a, Pos b) { return horizontalDistance(a,b); }
    void finish(Pos end) {
        result={end};
        while (!(result.back()==start)) result.push_back(parent.at(result.back()));
        std::reverse(result.begin(),result.end());
        complete=true;
    }
    void exhausted() {
        if (segmentRadius && distance(best,goal)<distance(start,goal)) finish(best);
        else complete=true;
    }
public:
    Search(Pos from, Pos to, int budget = 32768, int radius = 0)
        : start(from),goal(to),maxNodes(budget),segmentRadius(std::clamp(radius,0,32)),best(from) {
        open.push({start,0,distance(start,goal)}); cost[start]=0;
    }
    bool done() const { return complete; }
    const std::vector<Pos>& route() const { return result; }
    void step(const std::function<bool(Pos)>& walkable, int slice = 128) {
      if (complete) return;
      auto knownWalkable=[&](Pos p) {
          auto [it,inserted]=walkability.try_emplace(p,false);
          if (inserted) it->second=walkable(p);
          return it->second;
      };
      // A distant goal may not exist in the client's loaded chunks yet.
      // Only strict searches require it up front; rolling searches inspect
      // local cells and return proven progress, not an invented path to it.
      if (start.y != goal.y || !knownWalkable(start) || (!segmentRadius && !knownWalkable(goal))) { complete=true; return; }
      int visits=0;
      while (!open.empty() && visits++ < slice && expanded < maxNodes) {
        const auto current = open.top(); open.pop();
        if (current.g != cost[current.pos]) continue;
        if (current.pos == goal) {
            finish(goal); return;
        }
        if (distance(current.pos,goal)<distance(best,goal)) best=current.pos;
        ++expanded;
        for (Pos delta : {Pos{1,0,0}, Pos{-1,0,0}, Pos{0,0,1}, Pos{0,0,-1}}) {
            Pos next{current.pos.x+delta.x, start.y, current.pos.z+delta.z};
            if (segmentRadius && distance(next,start)>segmentRadius) continue;
            if (!knownWalkable(next)) continue;
            int g = current.g+1;
            auto found = cost.find(next);
            if (found != cost.end() && found->second <= g) continue;
            if (cost.size() >= size_t(maxNodes)*4 || open.size() >= size_t(maxNodes)*2) { exhausted(); return; }
            cost[next] = g; parent[next] = current.pos;
            open.push({next, g, g+distance(next,goal)});
        }
      }
      if (open.empty() || expanded >= maxNodes) exhausted();
    }
};
inline std::vector<Pos> path(Pos start, Pos goal, const std::function<bool(Pos)>& walkable, int budget=2048) {
    Search search(start,goal,budget);
    while (!search.done()) search.step(walkable);
    return search.route();
}
// Known floor topology permits backward detours to earlier junctions. Every
// live step is revalidated; a remembered cell never authorizes walking on air.
class RouteMemory {
    std::set<Pos> cells;
    Pos last{INT32_MIN,0,0};
    int scan=0;
public:
    void clear(){cells.clear();last={INT32_MIN,0,0};scan=0;}
    bool contains(Pos p) const{return cells.contains(p);}
    void observe(Pos center,const std::function<bool(Pos)>& walkable) {
        if(!(center==last)) {
          last=center;
          for(int x=-2;x<=2;++x)for(int z=-2;z<=2;++z) {
            Pos p{center.x+x,center.y,center.z+z};
            if(walkable(p)) {if(cells.size()<32768)cells.insert(p);} else cells.erase(p);
          }
        }
        // Incremental scan discovers loaded junctions/parallel lanes even when
        // the player is stationary. Unknown far cells never erase history.
        for(int n=0;n<64;++n) {
            const int i=scan++%1089;
            Pos p{center.x+i%33-16,center.y,center.z+i/33-16};
            if(cells.size()<32768 && walkable(p))cells.insert(p);
        }
    }
    size_t size() const{return cells.size();}
};
struct CloseBarrier {
    int window = -1;
    bool client = false, server = false;
    void begin(int id) { window = id; client = server = false; }
    void observe(int id, bool fromClient) {
        if (window < 0 || id != window) return;
        (fromClient ? client : server) = true;
    }
    bool ready(bool worldScreen) const { return window >= 0 && client && server && worldScreen; }
};
struct InventoryScreenGate {
    bool active=false, opened=false, closing=false;
    CloseBarrier close;
    void reset() { *this={}; }
    void begin() { reset(); active=true; }
    bool acceptOpen(int window,int type) {
        if (!active || closing || window!=0 || type!=255) return false;
        opened=true; return true;
    }
    bool canTransfer(bool worldScreen) const { return active && opened && !closing && !worldScreen; }
    bool beginClose() {
        if (!active || !opened || closing) return false;
        closing=true; close.begin(0); return true;
    }
    void observeClose(int window,bool fromClient) {
        if (!active || window!=0) return;
        if (closing) close.observe(window,fromClient);
        else opened=false;
    }
    bool ready(bool worldScreen) const { return active && closing && close.ready(worldScreen); }
};
}
