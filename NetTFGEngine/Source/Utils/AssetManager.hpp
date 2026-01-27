#pragma once

#include <unordered_map>
#include <string>
#include <typeindex>
#include <cassert>
#include <functional>
#include <any>

class AssetManager
{
public:
    using Key = std::string;

    static AssetManager& instance()
    {
        static AssetManager instance;
        return instance;
    }

    AssetManager(const AssetManager&) = delete;
    AssetManager& operator=(const AssetManager&) = delete;

    /* ============================
       Public API
       ============================ */

    template<typename Handle>
    std::optional<Handle> acquire(const Key& key)
    {
        auto& typeMap = assets[typeid(Handle)];
        auto it = typeMap.find(makeKey(key));

        if (it != typeMap.end())
        {
            ++it->second.refCount;
            return std::any_cast<Handle>(it->second.handle);
        }

        Handle h = load<Handle>(makeKey(key));
        if (h == 0)
            return std::nullopt;

        Entry entry;
        entry.handle = h;
        entry.refCount = 1;

        typeMap.emplace(key, std::move(entry));
        return h;
    }


    template<typename Handle>
    void release(const Key& key)
    {
        auto& typeMap = assets[typeid(Handle)];
        auto it = typeMap.find(makeKey(key));
        if (it == typeMap.end()) {
            Debug::Error("AssetManager") << "Release called on missing asset: " << key << "\n";
            return; // avoid crash
        }

        Entry& entry = it->second;
        assert(entry.refCount > 0);


        --entry.refCount;

        if (entry.refCount == 0)
        {
            destroy<Handle>(std::any_cast<Handle>(entry.handle));
            typeMap.erase(it);
        }
    }

    template<typename Handle>
    bool isLoaded(const Key& key) const
    {
        auto itType = assets.find(typeid(Handle));
        if (itType == assets.end())
            return false;

        return itType->second.find(makeKey(key)) != itType->second.end();
    }

    /* ============================
       Registration
       ============================ */

    template<typename Handle>
    void registerType(
        std::function<Handle(const Key&)> loader,
        std::function<void(Handle)> destroyer)
    {
        loaders[typeid(Handle)] = [loader](const Key& k) { return std::any(loader(k)); };
        destroyers[typeid(Handle)] = [destroyer](std::any h)
            {
                destroyer(std::any_cast<Handle>(h));
            };
    }

    template<typename Handle>
    void clearType()
    {
        auto it = assets.find(typeid(Handle));
        if (it == assets.end())
            return;

        for (auto& [key, entry] : it->second)
        {
            destroy<Handle>(std::any_cast<Handle>(entry.handle));
        }

        assets.erase(it);
    }


private:
    AssetManager() = default;
    ~AssetManager()
    {
        // Clean shutdown
        for (auto& [type, typeMap] : assets)
        {
            for (auto& [key, entry] : typeMap)
            {
                destroyers[type](entry.handle);
            }
        }
    }

    std::string makeKey(const std::string& key) {
        if (key.find("Content/") != 0) return "Content/" + key;
        return key;
    }


    struct Entry
    {
        std::any handle;
        std::size_t refCount = 0;
    };

    std::unordered_map<
        std::type_index,
        std::unordered_map<Key, Entry>
    > assets;

    std::unordered_map<
        std::type_index,
        std::function<std::any(const Key&)>
    > loaders;

    std::unordered_map<
        std::type_index,
        std::function<void(std::any)>
    > destroyers;

    template<typename Handle>
    Handle load(const Key& key)
    {
        auto it = loaders.find(typeid(Handle));
        assert(it != loaders.end());
        return std::any_cast<Handle>(it->second(key));
    }

    template<typename Handle>
    void destroy(Handle handle)
    {
        auto it = destroyers.find(typeid(Handle));
        assert(it != destroyers.end());
        it->second(std::any(handle));
    }
};
