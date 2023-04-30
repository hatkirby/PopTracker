#include "tracker.h"
#include "../luaglue/luamethod.h"
#include <cstring>
#include <nlohmann/json.hpp>
#include "jsonutil.h"
#include "util.h"
using nlohmann::json;

const LuaInterface<Tracker>::MethodMap Tracker::Lua_Methods = {
    LUA_METHOD(Tracker, AddItems, const char*),
    LUA_METHOD(Tracker, AddLocations, const char*),
    LUA_METHOD(Tracker, AddMaps, const char*),
    LUA_METHOD(Tracker, AddLayouts, const char*),
    LUA_METHOD(Tracker, ProviderCountForCode, const char*),
    LUA_METHOD(Tracker, FindObjectForCode, const char*),
    LUA_METHOD(Tracker, UiHint, const char*, const char*),
};

static LayoutNode blankLayoutNode = LayoutNode::FromJSON(json({}));
static JsonItem blankItem = JsonItem::FromJSON(json({}));
static Map blankMap = Map::FromJSON(json({}));
static Location blankLocation;// = Location::FromJSON(json({}));
static LocationSection blankLocationSection;// = LocationSection::FromJSON(json({}));

Tracker::Tracker(Pack* pack, lua_State *L)
    : _pack(pack), _L(L)
{
}

Tracker::~Tracker()
{
}

bool Tracker::AddItems(const std::string& file) {
    printf("Loading items from \"%s\"...\n", file.c_str());
    std::string s;
    if (!_pack->ReadFile(file, s)) return false; // TODO: throw lua error?
    json j = parse_jsonc(s);
    
    if (j.type() != json::value_t::array) {
        fprintf(stderr, "Bad json\n"); // TODO: throw lua error?
        return false;
    }
    
    _reachableCache.clear();
    _providerCountCache.clear();
    for (auto& v : j) {
        if (v.type() != json::value_t::object) {
            fprintf(stderr, "Bad item\n");
            continue; // ignore
        }
        _jsonItems.push_back(JsonItem::FromJSON(v));
        auto& item = _jsonItems.back();
        item.setID(++_lastItemID);
        item.onChange += {this, [this](void* sender) {
            if (!_bulkUpdate) _reachableCache.clear();
            _providerCountCache.clear();
            JsonItem* i = (JsonItem*)sender;
            if (i->getType() == BaseItem::Type::COMPOSITE_TOGGLE) {
                // update part items when changing composite
                unsigned n = (unsigned)i->getActiveStage();
                auto leftCodes = i->getCodes(1);
                auto rightCodes = i->getCodes(2);
                if (!leftCodes.empty()) {
                    auto o = FindObjectForCode(leftCodes.front().c_str());
                    if (o.type == Object::RT::JsonItem)
                        o.jsonItem->setState((n&1)?1:0); // TODO: advanceToCode() instead?
                    else if (o.type == Object::RT::LuaItem)
                        o.luaItem->setState((n&1)?1:0);
                }
                if (!rightCodes.empty()) {
                    auto o = FindObjectForCode(rightCodes.front().c_str());
                    if (o.type == Object::RT::JsonItem)
                        o.jsonItem->setState((n&2)?1:0);
                    else if (o.type == Object::RT::LuaItem)
                        o.luaItem->setState((n&2)?1:0);
                }
            }
            if (_bulkUpdate)
                _bulkItemUpdates.push_back(i->getID());
            else
                onStateChanged.emit(this, i->getID());
        }};
        if (item.getType() == BaseItem::Type::COMPOSITE_TOGGLE) {
            // update composite when changing part items (and get initial state)
            int n = 0;
            auto id = item.getID();
            auto leftCodes = item.getCodes(1);
            auto rightCodes = item.getCodes(2);
            auto update = [this,id](unsigned bit, bool value) {
                for (auto& i : _jsonItems) {
                    if (i.getID() == id) {
                        unsigned stage = (unsigned)i.getActiveStage();
                        i.setState(1, value ? (stage|bit) : (stage&~bit));
                        break;
                    }
                }
            };
            if (!leftCodes.empty()) {
                auto o = FindObjectForCode(leftCodes.front().c_str());
                if (o.type == Object::RT::JsonItem) {
                    if (o.jsonItem->getState()) n += 1;
                    o.jsonItem->onChange += { this, [update](void* sender) {
                        update(1, ((JsonItem*)sender)->getState());
                    }};
                }
                else if (o.type == Object::RT::LuaItem) {
                    if (o.luaItem->getState()) n += 1;
                    o.luaItem->onChange += { this, [update](void* sender) {
                        update(1, ((LuaItem*)sender)->getState());
                    }};
                }
            }
            if (!rightCodes.empty()) {
                auto o = FindObjectForCode(rightCodes.front().c_str());
                if (o.type == Object::RT::JsonItem) {
                    if (o.jsonItem->getState()) n += 2;
                    o.jsonItem->onChange += { this, [update](void* sender) {
                        update(2, ((JsonItem*)sender)->getState());
                    }};
                }
                else if (o.type == Object::RT::LuaItem) {
                    if (o.luaItem->getState()) n += 2;
                    o.luaItem->onChange += { this, [update](void* sender) {
                        update(2, ((LuaItem*)sender)->getState());
                    }};
                }
            }
            item.setState(1, n);
        }
        if (!item.getBaseItem().empty()) {
            // fire event for toggle_badged when base item changed
            auto id = item.getID();
            auto o = FindObjectForCode(item.getBaseItem().c_str());
            auto update = [this,id]() {
                for (auto& i : _jsonItems) {
                    if (i.getID() == id) {
                        i.onChange.emit(&i);
                        break;
                    }
                }
            };
            if (o.type == Object::RT::JsonItem) {
                o.jsonItem->onChange += {this, [update](void* sender) {
                    update();
                }};
            } else if (o.type == Object::RT::LuaItem) {
                o.luaItem->onChange += {this, [update](void* sender) {
                    update();
                }};
            }
        }
    }
    
    onLayoutChanged.emit(this, ""); // TODO: differentiate between structure and content
    return true;
}
bool Tracker::AddLocations(const std::string& file) {
    printf("Loading locations from \"%s\"...\n", file.c_str());
    std::string s;
    if (!_pack->ReadFile(file, s)) return false; // TODO: throw lua error?
    json j = parse_jsonc(s);
    
    if (j.type() != json::value_t::array) {
        fprintf(stderr, "Bad json\n"); // TODO: throw lua error?
        return false;
    }
    
    _reachableCache.clear();
    _providerCountCache.clear();
    for (auto& loc : Location::FromJSON(j, _locations)) {
        // find duplicate, warn and merge
#ifdef MERGE_DUPLICATE_LOCATIONS // this should be default in the future
        bool merged = false;
        for (auto& other: _locations) {
            if (other.getID() == loc.getID()) {
                fprintf(stderr, "WARNING: merging duplicate location \"%s\"!\n", sanitize_print(loc.getID()).c_str());
                other.merge(loc);
                merged = true;
                for (auto& sec : other.getSections()) {
                    sec.onChange -= this;
                    sec.onChange += {this,[this,&sec](void*){ onLocationSectionChanged.emit(this, sec); }};
                }
                break;
            }
        }
        if (merged) continue;
#else
        bool duplicate = false;
        for (auto& other: _locations) {
            if (other.getID() == loc.getID()) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            std::string oldID = loc.getID();
            std::string newID;
            unsigned n = 1;
            while (newID.empty()) {
                newID = oldID + "[" + std::to_string(n) + "]";
                for (const auto& other: _locations) {
                    if (other.getID() == newID) {
                        newID.clear();
                        break;
                    }
                }
                n++;
            }
            loc.setID(newID);
            fprintf(stderr, "WARNING: renaming duplicate location \"%s\" to \"%s\"!\n"
                    "  This behavior will change in the future!\n",
                    sanitize_print(oldID).c_str(), sanitize_print(newID).c_str());
        }
#endif
        _locations.push_back(std::move(loc)); // TODO: move constructor
        for (auto& sec : _locations.back().getSections()) {
            sec.onChange += {this,[this,&sec](void*){ onLocationSectionChanged.emit(this, sec); }};
        }
    }
    
    onLayoutChanged.emit(this, ""); // TODO: differentiate between structure and content
    return false;
}
bool Tracker::AddMaps(const std::string& file) {
    printf("Loading maps from \"%s\"...\n", file.c_str());
    std::string s;
    if (!_pack->ReadFile(file, s)) return false; // TODO: throw lua error?
    json j = parse_jsonc(s);
    
    if (j.type() != json::value_t::array) {
        fprintf(stderr, "Bad json\n"); // TODO: throw lua error?
        return false;
    }
    
    for (auto& v : j) {
        if (v.type() != json::value_t::object || v["name"].type() != json::value_t::string) {
            fprintf(stderr, "Bad map\n");
            continue; // ignore
        }
        _maps[v["name"]] = Map::FromJSON(v);
    }
    
    onLayoutChanged.emit(this, ""); // TODO: differentiate between structure and content
    return false;
}
bool Tracker::AddLayouts(const std::string& file) {
    printf("Loading layouts from \"%s\"...\n", file.c_str());
    std::string s;
    if (!_pack->ReadFile(file, s)) return false; // TODO: throw lua error?
    json j = parse_jsonc(s);
    
    if (j.type() != json::value_t::object) {
        fprintf(stderr, "Bad json\n"); // TODO: throw lua error?
        return false;
    }

    if (j.find("layouts") != j.end() && j["layouts"].is_object())
        j = j["layouts"]; // legacy layout file
    else if (j.find("type") != j.end() && j.find("content") != j.end()
            && j["type"].is_string() && (j["content"].is_array() || j["content"].is_object()))
        j = json{{"tracker_broadcast", j}}; // legacy broadcast_layout

    for (auto& [key,value] : j.items()) {
        if (value.type() != json::value_t::object) {
            fprintf(stderr, "Bad layout: %s (type %d)\n", key.c_str(), (int)value.type());
            continue; // ignore
        }
        if (_layouts.find(key) != _layouts.end())
            fprintf(stderr, "WARNING: replacing existing layout \"%s\"\n",
                    key.c_str());
        _layouts[key] = LayoutNode::FromJSON(value);
    }
    
    // TODO: fire for each named layout
    onLayoutChanged.emit(this, ""); // TODO: differentiate between structure and content
    return false;
}
int Tracker::ProviderCountForCode(const std::string& code)
{
    // cache this, because inefficient use can make the Lua script hang
    auto it = _providerCountCache.find(code);
    if (it != _providerCountCache.end())
        return it->second;
    // "codes" starting with $ run Lua functions
    if (!code.empty() && code[0] == '$') {
        // TODO: use a helper to access Lua instead of having _L here
        int args = 0;
        auto pos = code.find('|');
        int t;
        if (pos == code.npos) {
            t = lua_getglobal(_L, code.c_str()+1);
        } else {
            t = lua_getglobal(_L, code.substr(1, pos-1).c_str());
            if (t == LUA_TFUNCTION) {
                std::string::size_type next;
                while ((next = code.find('|', pos+1)) != code.npos) {
                    lua_pushstring(_L, code.substr(pos+1, next-pos-1).c_str());
                    args++;
                    pos = next;
                }
                lua_pushstring(_L, code.substr(pos+1).c_str());
                args++;
            }
        }
        if (t != LUA_TFUNCTION) {
            fprintf(stderr, "Missing Lua function for %s\n", code.c_str());
            lua_pop(_L, 1); // non-function variable or nil
            _providerCountCache[code] = 0;
            return 0;
        }
        else if (lua_pcall(_L, args, 1, 0) != LUA_OK) {
            auto err = lua_tostring(_L, -1);
            fprintf(stderr, "Error running %s:\n%s\n",
                code.c_str(), err ? err : "Unknown error");
            lua_pop(_L, 1); // error object
            _providerCountCache[code] = 0;
            return 0;
        } else {
            int isnum = 0;
            int n = lua_tonumberx(_L, -1, &isnum);
            if (!isnum && lua_isboolean(_L, -1) && lua_toboolean(_L, -1)) n = 1;
            lua_pop(_L, 1); // result
            _providerCountCache[code] = n;
            return n;
        }
    }
    // other codes count items
    int res=0;
    for (const auto& item : _jsonItems)
    {
        res += item.providesCode(code);
    }
    for (const auto& item : _luaItems)
    {
        res += item.providesCode(code);
    }
    _providerCountCache[code] = res;
    return res;
}
Tracker::Object Tracker::FindObjectForCode(const char* code)
{
    // TODO: locations (not just sections)?
    if (*code == '@') { // location section
        const char *start = code+1;
        const char *t = strrchr(start, '/');
        if (t) { // valid section identifier
            std::string locid = std::string(start, t-start);
            std::string secname = t+1;
            // match by ID (includes all parents)
            auto& loc = getLocation(locid, true);
            for (auto& sec: loc.getSections()) {
                if (sec.getName() != secname) continue;
                return &sec;
            }
            
        }
    }
    for (auto& item : _jsonItems) {
        if (item.canProvideCode(code)) {
            return &item;
        }
    }
    for (auto& item : _luaItems) {
        if (item.canProvideCode(code)) {
            return &item;
        }
    }
    printf("Did not find object for code \"%s\".\n", code);
    return nullptr;
}

void Tracker::UiHint(const std::string& name, const std::string& value)
{
    onUiHint.emit(this, name, value);
}

int Tracker::Lua_Index(lua_State *L, const char* key) {
    if (strcmp(key, "ActiveVariantUID") == 0) {
        lua_pushstring(L, _pack->getVariant().c_str());
        return 1;
    } else {
        printf("Tracker::Lua_Index(\"%s\") unknown\n", key);
    }
    return 0;
}

const Map& Tracker::getMap(const std::string& name) const
{
    const auto it = _maps.find(name);
    return it==_maps.end() ? blankMap : it->second;
}

std::list<std::string> Tracker::getMapNames() const
{
    std::list<std::string> res;
    for (const auto& pair : _maps) res.push_back(pair.first);
    return res;
}

const LayoutNode& Tracker::getLayout(const std::string& name) const
{
    const auto it = _layouts.find(name);
    return it==_layouts.end() ? blankLayoutNode : it->second;
}
bool Tracker::hasLayout(const std::string& name) const
{
    return _layouts.find(name) != _layouts.end();
}
const BaseItem& Tracker::getItemByCode(const std::string& code) const
{
    for (const auto& item: _jsonItems) {
        if (item.canProvideCode(code)) return item;
    }
    
    for (const auto& item: _luaItems) {
        if (item.canProvideCode(code)) return item;
    }
    
    return blankItem;
}
BaseItem& Tracker::getItemById(const std::string& id)
{
    for (auto& item: _jsonItems) {
        if (item.getID() == id) return item;
    }
    for (auto& item: _luaItems) {
        if (item.getID() == id) return item;
    }
    return blankItem;
}
std::list< std::pair<std::string, Location::MapLocation> > Tracker::getMapLocations(const std::string& mapname) const
{
    std::list< std::pair<std::string, Location::MapLocation> > res;
    for (const auto& loc : _locations) {
        for (const auto& maploc : loc.getMapLocations()) {
            if (maploc.getMap() == mapname) {
                res.push_back( std::make_pair(loc.getID(), maploc) );
            }
        }
    }
    return res;
}

Location& Tracker::getLocation(const std::string& id, bool partialMatch)
{
    for (auto& loc : _locations) {
        if (loc.getID() == id)
            return loc;
    }
    if (partialMatch) {
        if (id.find('/') == id.npos) {
            for (auto& loc : _locations) {
                if (loc.getName() == id)
                    return loc;
            }
        } else {
            std::string search = "/" + id;
            for (auto& loc : _locations) {
                const auto& s = loc.getID();
                if (s.size()>search.size() && s.compare(s.size()-search.size(), search.size(), search) == 0)
                    return loc;
            }
        }
    }
    return blankLocation;
}

LocationSection& Tracker::getLocationSection(const std::string& id)
{
    const char *start = id.c_str();
    const char *t = strrchr(start, '/');
    if (t) { // valid section identifier
        std::string locid = std::string(start, t-start);
        std::string secname = t+1;
        // match by ID (includes all parents)
        auto& loc = getLocation(locid, true);
        for (auto& sec: loc.getSections()) {
            if (sec.getName() != secname) continue;
            return sec;
        }

    }
    return blankLocationSection;
}

const Pack* Tracker::getPack() const
{
    return _pack;
}

bool Tracker::changeItemState(const std::string& id, BaseItem::Action action)
{
    std::string baseCode; // for type: toggle_badged
    for (auto& item: _jsonItems) {
        if (item.getID() != id) continue;
        if (item.changeState(action)) {
            // NOTE: item fires onChanged
            return true;
        }
        baseCode = item.getBaseItem();
        break;
    }
    for (auto& item: _luaItems) {
        if (item.getID() != id) continue;
        if (item.changeState(action)) {
            // NOTE: item fires onChanged
            return true;
        }
        baseCode = item.getBaseItem();
        break;
    }
    if (!baseCode.empty()) {
        // for items that have a base item, propagate click
        // NOTE: the items have to subscribe to their base's onChanged
        auto baseObject = FindObjectForCode(baseCode.c_str());
        if (baseObject.type == Object::RT::JsonItem) {
            return baseObject.jsonItem->changeState(BaseItem::Action::Single);
        } else if (baseObject.type == Object::RT::LuaItem) {
            return baseObject.luaItem->changeState(BaseItem::Action::Single);
        }
    }
    return false; // nothing changed
}

AccessibilityLevel Tracker::isReachable(const Location& location, const LocationSection& section)
{
    if (_parents) {
        ReachabilityResult result = isReachable(location, section, *_parents);
        return result.accessibility;
    } else {
        std::list<std::string> parents;
        ReachabilityResult result = isReachable(location, section, parents);
        return result.accessibility;
    }
}

bool Tracker::isVisible(const Location& location, const LocationSection& section)
{
    std::list<std::string> parents;
    return isVisible(location, section, parents);
}

AccessibilityLevel Tracker::isReachable(const Location& location)
{
  if (_parents) {
      ReachabilityResult result = isReachable(location, *_parents);
      return result.accessibility;
  } else {
      std::list<std::string> parents;
      ReachabilityResult result = isReachable(location, parents);
      return result.accessibility;
  }
}

AccessibilityLevel Tracker::isReachable(const LocationSection& section)
{
    for (const auto& loc: _locations) {
        if (loc.getID() == section.getParentID()) {
            return isReachable(loc, section);
        }
    }
    return AccessibilityLevel::NONE;
}

bool Tracker::isVisible(const Location& location)
{
    std::list<std::string> parents;
    return isVisible(location, parents);
}

// Each ruleset has the potential to evaluate to NONE because of recursion
// detection. For each sub-evaluation that returns NONE because of recursion,
// the set of recursion-causing locations that are above us in the evaluation
// stack will be returned. We don't want to cache the result of the
// sub-evaluation in this case, because it's possible that there could be a
// different result if evaluation started at this location instead. This
// function is expected to return a union of recursion-causing locations, even
// if the final evaluation is not NONE.
Tracker::ReachabilityResult Tracker::isReachable(const std::list< std::list<std::string> >& rules, bool visibilityRules, std::list<std::string>& parents)
{
    // TODO: return enum instead of int
    // returns 0 for unreachable, 1 for reachable, 2 for glitches required
    ReachabilityResult result;

    bool glitchedReachable = false;
    bool checkOnlyReachable = false;
    if (rules.empty()) {
        result.accessibility = AccessibilityLevel::NORMAL;
        return result;
    }
    for (const auto& ruleset : rules) { //<-- these are all to be ORed
        if (ruleset.empty()) {
            result.accessibility = AccessibilityLevel::NORMAL; // any empty rule set means true
            result.cycles.clear();
            return result;
        }
        AccessibilityLevel reachable = AccessibilityLevel::NORMAL;
        bool checkOnly = false;
        std::set<std::string> cycles;
        for (const auto& rule: ruleset) { //<-- these are all to be ANDed
            if (rule.empty()) continue; // empty/missing code is true
            std::string s = rule;
            // '[' ... ']' means optional/glitches required (different color)
            bool optional = false;
            if (s.length() > 1 && s[0] == '[' && s[s.length()-1]==']') {
                optional = true;
                s = s.substr(1,s.length()-2);
            }
            // '{' ... '}' means required to check (i.e. the rule never returns "reachable", but "checkable" instead)
            if (s.length() > 1 && s[0] == '{') {
                checkOnly = true;
                s = s.substr(1,s.length()-1);
            }
            if (checkOnly && s.length() > 0 && s[s.length()-1] == '}') {
                s = s.substr(0, s.length()-1);
            }
            if (checkOnly && s.empty()) {
                checkOnlyReachable = true;
                continue;
            }
            // '<rule>:<count>' checks count (e.g. consumables) instead of bool
            int count = 1;
            auto p = s.find(':');
            if (p != s.npos) {
                count = atoi(s.c_str()+p+1);
                s = s.substr(0,p);
            }
            auto it = _reachableCache.find(s);
            if (it != _reachableCache.end()) {
#if 0
                if (s[0] != '@') { // value is count
                    if (it->second >= count) continue;
                    if (optional) {
                        reachable = AccessibilityLevel::SEQUENCE_BREAK;
                    } else {
                        reachable = AccessibilityLevel::NONE;
                        break;
                    }
                } else
#endif
                { // value is glitched/not glitched
                    auto sub = it->second;
                    if (!checkOnly && sub == AccessibilityLevel::INSPECT) sub = AccessibilityLevel::NONE; // or set checkable = true?
                    else if (optional && sub == AccessibilityLevel::NONE) sub = AccessibilityLevel::SEQUENCE_BREAK;
                    else if (sub == AccessibilityLevel::NONE) reachable = AccessibilityLevel::NONE;
                    if (sub == AccessibilityLevel::SEQUENCE_BREAK && reachable != AccessibilityLevel::NONE) reachable = AccessibilityLevel::SEQUENCE_BREAK;
                    if (reachable == AccessibilityLevel::NONE) break;
                }
            }
            // '@' references other locations
            else if (s[0] == '@') {
                const char* start = s.c_str()+1;
                const char* t = strrchr(s.c_str()+1, '/');
                std::string locid = s.substr(1);
                auto& loc = getLocation(locid, true);
                bool match = false;
                AccessibilityLevel sub = AccessibilityLevel::NONE;
                if (!t && loc.getID().empty()) {
                    printf("Invalid location %s for access rule!\n",
                            sanitize_print(s).c_str());
                    continue; // invalid location
                } else if (!loc.getID().empty()) {
                    // @-Rule for location, not a section
                    if (visibilityRules) sub = isVisible(loc, parents) ? AccessibilityLevel::NORMAL : AccessibilityLevel::NONE;
                    else {
                        ReachabilityResult sub_result = isReachable(loc, parents);
                        sub = sub_result.accessibility;
                        cycles = sub_result.cycles;
                    }
                    match = true;
                } else {
                    // @-Rule for a section (also run for missing location)
                    std::string sublocid = locid.substr(0, t-start);
                    std::string subsecname = t+1;
                    auto& subloc = getLocation(sublocid, true);
                    for (auto& subsec: subloc.getSections()) {
                        if (subsec.getName() != subsecname) continue;
                        if (visibilityRules) sub = isVisible(subloc, subsec, parents) ? AccessibilityLevel::NONE : AccessibilityLevel::NONE;
                        else {
                            ReachabilityResult sub_result = isReachable(subloc, subsec, parents);
                            sub = sub_result.accessibility;
                            cycles = sub_result.cycles;
                        }
                        match = true;
                        break;
                    }
                }
                if (match) {
                    if (!visibilityRules && cycles.empty()) _reachableCache[s] = sub; // only cache isReachable (not isVisible) for @
                    // combine current state with sub-result
                    if (!checkOnly && sub == AccessibilityLevel::INSPECT) sub = AccessibilityLevel::NONE; // or set checkable = true?
                    else if (optional && sub == AccessibilityLevel::NONE) sub = AccessibilityLevel::SEQUENCE_BREAK;
                    else if (sub == AccessibilityLevel::NONE) reachable = AccessibilityLevel::NONE;
                    if (sub == AccessibilityLevel::SEQUENCE_BREAK && reachable != AccessibilityLevel::NONE) reachable = AccessibilityLevel::SEQUENCE_BREAK;
                } else {
                    printf("Could not find location %s for access rule!\n",
                            sanitize_print(s).c_str());
                }
                if (reachable == AccessibilityLevel::NONE) break;
            }
            // '$' calls into Lua, now also supported by ProviderCountForCode
            // other: references codes (with or without count)
            else {
                _parents = &parents;
                int n = ProviderCountForCode(s);
                _parents = nullptr;
#if 0
                _reachableCache[s] = n; // FIXME: test if commenting this out has an impact
#endif
                if (n >= count) continue;
                if (optional) {
                    reachable = AccessibilityLevel::SEQUENCE_BREAK;
                } else {
                    reachable = AccessibilityLevel::NONE;
                    break;
                }
            }
        }
        result.cycles.merge(cycles);
        if (reachable == AccessibilityLevel::NORMAL && !checkOnly) {
            result.accessibility = AccessibilityLevel::NORMAL;
            return result;
        }
        else if (reachable != AccessibilityLevel::NONE /*== 1*/ && checkOnly) checkOnlyReachable = true;
        if (reachable == AccessibilityLevel::SEQUENCE_BREAK) glitchedReachable = true;
    }
    result.accessibility = glitchedReachable ? AccessibilityLevel::SEQUENCE_BREAK :
                           checkOnlyReachable ? AccessibilityLevel::INSPECT :
                               AccessibilityLevel::NONE;
    return result;
}

// If the location evaluates to NONE because of circularity, the cycles field of
// the result will be the names of the cycle-defining location/sections above us
// in the evaluation stack. If this is the child node of a cycle, that means
// that "cycles" will contain the name of this node. If this is the parent node
// of a cycle, "cycles" will not contain the name of this node.
//
// If the location evaluates to non-NONE, the cycles field of the result will be
// empty even if we are in the middle of a cycle, because circularity cannot
// cause a false positive.
Tracker::ReachabilityResult Tracker::isReachable(const Location& location, const LocationSection& section, std::list<std::string>& parents)
{
    const LocationSection& realSection = section.getRef().empty() ? section : getLocationSection(section.getRef());
    std::string id = realSection.getParentID() + "/" + realSection.getName();
    if (std::find(parents.begin(), parents.end(), id) != parents.end()) {
        printf("access_rule recursion detected: %s!\n", id.c_str());
        // returning 0 here should mean this path is unreachable, other paths that are logical "or" should be resolved
        ReachabilityResult result;
        result.cycles.insert(id);
        return result;
    }
    parents.push_back(id);
    auto res = isReachable(realSection.getAccessRules(), false, parents);
    if (res.accessibility == AccessibilityLevel::NONE) {
        res.cycles.erase(id);
    } else {
        res.cycles.clear();
    }
    parents.pop_back();
    return res;
}

bool Tracker::isVisible(const Location& location, const LocationSection& section, std::list<std::string>& parents)
{
    const LocationSection& realSection = section.getRef().empty() ? section : getLocationSection(section.getRef());
    std::string id = realSection.getParentID() + "/" + realSection.getName();
    if (std::find(parents.begin(), parents.end(), id) != parents.end()) {
        printf("visibility_rule recursion detected: %s!\n", id.c_str());
        return 0;
    }
    parents.push_back(id);
    auto res = isReachable(realSection.getVisibilityRules(), true, parents);
    parents.pop_back();
    return (res.accessibility != AccessibilityLevel::NONE);
}

Tracker::ReachabilityResult Tracker::isReachable(const Location& location, std::list<std::string>& parents)
{
    if (std::find(parents.begin(), parents.end(), location.getID()) != parents.end()) {
        printf("access_rule recursion detected: %s!\n", location.getID().c_str());
        ReachabilityResult result;
        result.cycles.insert(location.getID());
        return result;
    }
    parents.push_back(location.getID());
    auto res = isReachable(location.getAccessRules(), false, parents);
    if (res.accessibility == AccessibilityLevel::NONE) {
        res.cycles.erase(location.getID());
    } else {
        res.cycles.clear();
    }
    parents.pop_back();
    return res;
}

bool Tracker::isVisible(const Location& location, std::list<std::string>& parents)
{
    if (std::find(parents.begin(), parents.end(), location.getID()) != parents.end()) {
        printf("visibility_rule recursion detected: %s!\n", location.getID().c_str());
        return 0;
    }
    parents.push_back(location.getID());
    auto res = isReachable(location.getVisibilityRules(), true, parents);
    parents.pop_back();
    return (res.accessibility != AccessibilityLevel::NONE);
}

LuaItem * Tracker::CreateLuaItem()
{
    _luaItems.push_back({});
    LuaItem& i = _luaItems.back();
    i.setID(++_lastItemID);
    i.onChange += {this, [this](void* sender) {
        if (!_bulkUpdate) _reachableCache.clear();
        _providerCountCache.clear();
        LuaItem* i = (LuaItem*)sender;
        if (_bulkUpdate)
            _bulkItemUpdates.push_back(i->getID());
        else
            onStateChanged.emit(this, i->getID());
    }};
    return &i;
}


json Tracker::saveState() const
{
    /* structure:
     { tracker: {
        format_version: 1,
        json_items: { "some_id": <JsonItem::saveState>, ... },
        lua_items: { "some_id": <LuaItem::saveState>, ... },
        sections: { "full_location_path": <LocationSection::saveState>, ... }
     } }
    */
    
    json jJsonItems = {};
    for (auto& item: _jsonItems) {
        jJsonItems[item.getID()] = item.save();
    }
    json jLuaItems = {};
    for (auto& item: _luaItems) {
        jLuaItems[item.getID()] = item.save();
    }
    json jSections = {};
    for (auto& loc: _locations) {
        for (auto& sec: loc.getSections()) {
            std::string id = loc.getID() + "/" + sec.getName();
            if (jSections.find(id) != jSections.end()) {
                fprintf(stderr, "WARNING: duplicate location section: \"%s\"!\n",
                        sanitize_print(id).c_str());
            }
            jSections[id] = sec.save();
        }
    }
    
    json state = { {
        "tracker", {
            {"format_version", 1},
            {"json_items", jJsonItems},
            {"lua_items", jLuaItems},
            {"sections", jSections}
        }
    } };
    
    return state;
}

bool Tracker::loadState(nlohmann::json& state)
{
    _reachableCache.clear();
    _providerCountCache.clear();
    _bulkItemUpdates.clear();
    if (state.type() != json::value_t::object) return false;
    auto& j = state["tracker"]; // state's tracker data
    if (j["format_version"] != 1) return false; // incompatible state format

    _bulkUpdate = true;
    auto& jJsonItems = j["json_items"];
    if (jJsonItems.type() == json::value_t::object) {
        for (auto it=jJsonItems.begin(); it!=jJsonItems.end(); it++) {
            for (auto& item: _jsonItems) {
                if (item.getID() != it.key()) continue;
                item.load(it.value());
            }
        }
    }
    auto& jLuaItems = j["lua_items"];
    if (jLuaItems.type() == json::value_t::object) {
        for (auto it=jLuaItems.begin(); it!=jLuaItems.end(); ++it) {
            for (auto& item: _luaItems) {
                if (item.getID() != it.key()) continue;
                item.load(it.value());
            }
        }
    }
    auto& jSections = j["sections"];
    if (jSections.type() == json::value_t::object) {
        for (auto it=jSections.begin(); it!=jSections.end(); ++it) {
            std::string key = it.key();
            for (auto& loc: _locations) {
                if (key.rfind(loc.getID(), 0) != 0) continue;
                for (auto& sec: loc.getSections()) {
                    if (key != loc.getID() + "/" + sec.getName()) continue;
                    sec.load(it.value());
                }
            }
        }
    }
    for (const auto& id: _bulkItemUpdates)
        onStateChanged.emit(this, id);
    _bulkItemUpdates.clear();
    _bulkUpdate = false;

    return true;
}
