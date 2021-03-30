#ifndef _CORE_JSONITEM_H
#define _CORE_JSONITEM_H

#include "baseitem.h"
#include "../luaglue/luainterface.h"
#include <algorithm>
#include <json/json.hpp>

class JsonItem final : public LuaInterface<JsonItem>, public BaseItem {
    friend class LuaInterface;

public:
    static JsonItem FromJSON(nlohmann::json& j);
    static JsonItem FromJSON(nlohmann::json&& j);
    static JsonItem FromJSONString(const std::string& json);
    
    class Stage final {
    protected:
        std::list<std::string> _codes;
        std::list<std::string> _secondaryCodes;
        std::string _img;
        std::string _disabledImg;
        std::list<std::string> _imgMods;
        std::list<std::string> _disabledImgMods;
        bool _inheritCodes = true;
        
    public:
        static Stage FromJSON(nlohmann::json& j);
        static Stage FromJSON(nlohmann::json&& j);
        static Stage FromJSONString(const std::string& json);
        
        const std::string& getImage() const { return _img; }
        const std::string& getDisabledImage() const { return _disabledImg; }
        const std::list<std::string>& getImageMods() const { return _imgMods; }
        const std::list<std::string>& getDisabledImageMods() const { return _disabledImgMods; }
        const std::list<std::string>& getCodes() const { return _codes; }
        const std::list<std::string>& getSecondaryCodes() const { return _secondaryCodes; }
        std::string getCodesString() const;
        bool hasCode(const std::string& code) const { // NOTE: this is called canProvideCode in lua
            return std::find(_codes.begin(), _codes.end(), code) != _codes.end();
        }
        bool hasSecondaryCode(const std::string& code) const {
            return std::find(_secondaryCodes.begin(), _secondaryCodes.end(), code) != _secondaryCodes.end();
        }
        const bool getInheritCodes() const { return _inheritCodes; }
    };
    
protected:
    std::vector<Stage> _stages;
    
public:    
    virtual size_t getStageCount() const override { return _stages.size(); }
    
    virtual const std::string& getImage(size_t stage) const override {
        if (_type == Type::TOGGLE) return _img;
        if (_stages.size()>stage) return _stages[stage].getImage();
        return _img;
    }
    virtual const std::string& getDisabledImage(size_t stage) const override {
        if (_type == Type::TOGGLE) return _disabledImg;
        if (_stages.size()>stage) return _stages[stage].getDisabledImage();
        return _disabledImg;
    }
    
    virtual const std::list<std::string>& getImageMods(int stage) const override {
        if (_type == Type::TOGGLE) return _imgMods;
        if ((int)_stages.size()>stage) return _stages[stage].getImageMods();
        return _imgMods;
    }
    virtual const std::list<std::string>& getDisabledImageMods(int stage) const override {
        if (_type == Type::TOGGLE) return _disabledImgMods;
        if ((int)_stages.size()>stage) return _stages[stage].getDisabledImageMods();
        return _disabledImgMods;
    }
    
    virtual bool canProvideCode(const std::string& code) const override {
        if (std::find(_codes.begin(), _codes.end(), code) != _codes.end()) return true;
        for (const auto& stage: _stages)
            if (stage.hasCode(code))
                return true;
        return false;
    }
    
    virtual int providesCode(const std::string code) const override {
        // TODO: split at ':' for consumables to be able to check for a specific amount?
        if ((int)_stages.size()>_stage2) {
            if (_allowDisabled && !_stage1) return 0;
            for (int i=_stage2; i>=0; i--) {
                if (_stages[i].hasCode(code)) return 1;
                if (!_stages[i].getInheritCodes()) break;
            }
            return false;
        }
        if (_count && canProvideCode(code)) return _count;
        return (_stage1 && canProvideCode(code));
    }
    virtual std::string getCodesString() const override;
    
    virtual bool changeState(BaseItem::Action action) override {
        if (_changeStateImpl(action)) {
            onChange.emit(this);
            return true;
        }
        return false;
    }
    
    virtual void SetOverlay(const char* text) override {
        if (_overlay == text) return;
        _overlay = text;
        onChange.emit(this);
    }
    
    virtual nlohmann::json save() const;
    virtual bool load(nlohmann::json& j);
    
protected:
    
    bool _changeStateImpl(Action action);
    
protected: // LUA interface implementation
    
    static constexpr const char Lua_Name[] = "JsonItem";
    static const LuaInterface::MethodMap Lua_Methods;
    
    virtual int Lua_Index(lua_State *L, const char* key) override;
    virtual bool Lua_NewIndex(lua_State *L, const char *key) override;
};

#endif /* _CORE_JSONITEM_H */
