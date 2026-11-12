#ifndef ECS_HPP
#define ECS_HPP

#include "netcode/netcode_common.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <typeindex>
#include <unordered_map>   // no longer used here; kept for files that got it through this header
#include <utility>
#include <vector>
#include <queue>
#include <stdexcept>
#include <mutex>
#include "Utils/Debug/Debug.hpp"

using Entity = uint32_t;
constexpr Entity NULL_ENTITY = 0;

class IComponent {
public:
    virtual ~IComponent() = default;
    virtual void Destroy() {};
};

// ---------------------------------------------------------------------------------------------------------------------
// Component storage: a sparse set with STABLE slots.
//
//   sparse[entity]  -> slot + 1 (0 = the entity has no such component)    O(1) lookup, no hashing
//   pages           -> the components themselves, kPageSize per page       contiguous, never moved
//   dense           -> the entities that have one, packed                  what queries iterate
//
// Unlike a textbook sparse set, removing a component does NOT move another one into its place: its slot goes to a free
// list and only the (cheap, Entity-sized) dense entry is swapped. So a T* stays valid for the component's whole life,
// exactly as with the old unordered_map<Entity, unique_ptr<T>>: code that keeps component pointers (lambdas capturing
// SettingsPanelData*, colliders' Transform*, pointers taken before adding more components of the same type) is safe.
// Pages are never reallocated either, so adding components never moves existing ones.
// ---------------------------------------------------------------------------------------------------------------------

class IComponentArray {
public:
    virtual ~IComponentArray() = default;
    virtual void RemoveComponent(Entity entity) = 0;   // calls Destroy() first
    virtual bool HasComponent(Entity entity) const = 0;
    virtual IComponent* GetComponentBase(Entity entity) = 0;
    virtual size_t Size() const = 0;
    virtual void Clear() = 0;  // Destroy and remove all components
};

template<typename T>
class ComponentArray : public IComponentArray {
    static_assert(std::is_base_of<IComponent, T>::value, "ComponentArray<T>: T must derive from IComponent");

    static constexpr uint32_t kPageSize = 128;
    struct alignas(T) Slot { unsigned char bytes[sizeof(T)]; };

    std::vector<uint32_t> sparse;                 // entity -> slot + 1
    std::vector<std::unique_ptr<Slot[]>> pages;   // slot storage, never reallocated
    std::vector<uint32_t> freeSlots;              // slots of removed components, reused first
    uint32_t slotCount = 0;                       // slots ever handed out
    std::vector<Entity> dense;                    // entities with this component, packed
    std::vector<uint32_t> denseIndex;             // slot -> index in dense

    T* At(uint32_t slot) {
        return std::launder(reinterpret_cast<T*>(pages[slot / kPageSize][slot % kPageSize].bytes));
    }

    uint32_t AllocateSlot() {
        if (!freeSlots.empty()) {
            const uint32_t slot = freeSlots.back();
            freeSlots.pop_back();
            return slot;
        }
        const uint32_t slot = slotCount++;
        if (slot / kPageSize >= pages.size())
            pages.emplace_back(new Slot[kPageSize]);
        if (slot >= denseIndex.size())
            denseIndex.resize(slot + 1, 0);
        return slot;
    }

public:
    ComponentArray() = default;
    ComponentArray(const ComponentArray&) = delete;
    ComponentArray& operator=(const ComponentArray&) = delete;

    ~ComponentArray() override {
        // Same as the old unique_ptr storage: the destructor runs, Destroy() does not (Clear() calls both).
        for (Entity e : dense) At(sparse[e] - 1)->~T();
    }

    bool HasComponent(Entity entity) const override {
        return entity < sparse.size() && sparse[entity] != 0;
    }

    T* Get(Entity entity) {
        return (entity < sparse.size() && sparse[entity] != 0) ? At(sparse[entity] - 1) : nullptr;
    }

    IComponent* GetComponentBase(Entity entity) override { return Get(entity); }

    size_t Size() const override { return dense.size(); }

    const std::vector<Entity>& Entities() const { return dense; }

    // Adding to an entity that already has one replaces it in place (old destructor runs, not Destroy(): as before).
    template<typename... Args>
    T* Emplace(Entity entity, Args&&... args) {
        if (entity >= sparse.size())
            sparse.resize(static_cast<size_t>(entity) + 1, 0);

        if (sparse[entity] != 0) {
            T* p = At(sparse[entity] - 1);
            p->~T();
            return ::new (static_cast<void*>(p)) T(std::forward<Args>(args)...);
        }

        const uint32_t slot = AllocateSlot();
        T* p = ::new (static_cast<void*>(pages[slot / kPageSize][slot % kPageSize].bytes)) T(std::forward<Args>(args)...);
        sparse[entity] = slot + 1;
        denseIndex[slot] = static_cast<uint32_t>(dense.size());
        dense.push_back(entity);
        return p;
    }

    void RemoveComponent(Entity entity) override {
        if (!HasComponent(entity)) return;
        const uint32_t slot = sparse[entity] - 1;
        T* p = At(slot);
        p->Destroy();
        p->~T();

        // Swap-and-pop on the entity list only; components never move.
        const uint32_t i = denseIndex[slot];
        const Entity last = dense.back();
        dense[i] = last;
        denseIndex[sparse[last] - 1] = i;
        dense.pop_back();

        sparse[entity] = 0;
        freeSlots.push_back(slot);
    }

    // Remove all components, calling Destroy() on each.
    void Clear() override {
        for (Entity e : dense) {
            T* p = At(sparse[e] - 1);
            p->Destroy();
            p->~T();
        }
        dense.clear();
        sparse.clear();
        pages.clear();
        freeSlots.clear();
        denseIndex.clear();
        slotCount = 0;
    }
};

// Dense per-type ids (0, 1, 2...) for indexing EntityManager's arrays: no typeid / type_index hashing per call.
// Process-wide, so the same type has the same id in every EntityManager.
inline uint32_t NextComponentTypeId() {
    static std::atomic<uint32_t> next{ 0 };
    return next.fetch_add(1, std::memory_order_relaxed);
}

template<typename T>
uint32_t ComponentTypeId() {
    static const uint32_t id = NextComponentTypeId();
    return id;
}


class EntityManager {
    std::vector<std::unique_ptr<IComponentArray>> componentArrays;   // indexed by ComponentTypeId<T>()
    std::vector<bool> activeEntities;
    std::queue<Entity> availableEntityIds;
    std::queue<Entity> entitiesToDestroy;
    Entity nextEntityId = 1;
    size_t entityCount = 0;

    // Recursive: acquireMutex()/releaseMutex() can nest on the same thread (RenderSystem inside IECSGameRenderer::Render()); a plain mutex would self-deadlock.
    std::recursive_mutex entityMutex;

    template<typename T>
    ComponentArray<T>* GetArray() {
        const uint32_t id = ComponentTypeId<T>();
        return id < componentArrays.size() ? static_cast<ComponentArray<T>*>(componentArrays[id].get()) : nullptr;
    }

    template<typename T>
    const ComponentArray<T>* GetArray() const {
        const uint32_t id = ComponentTypeId<T>();
        return id < componentArrays.size() ? static_cast<const ComponentArray<T>*>(componentArrays[id].get()) : nullptr;
    }

public:

    void acquireMutex() {
        entityMutex.lock();
    }

    void releaseMutex() {
        entityMutex.unlock();
    }

    // RAII form of acquireMutex()/releaseMutex(): released on every return and on exceptions.
    [[nodiscard]] std::unique_lock<std::recursive_mutex> Lock() {
        return std::unique_lock<std::recursive_mutex>(entityMutex);
    }

    // Resets to a clean state: destroys every component, clears entities/arrays/ID counters, and clears registered component types.
    void Reset() {
        for (auto& array : componentArrays) {
            if (array) array->Clear();
        }

        componentArrays.clear();
        activeEntities.clear();

        while (!entitiesToDestroy.empty()) entitiesToDestroy.pop();
        while (!availableEntityIds.empty()) availableEntityIds.pop();

        nextEntityId = 1;
        entityCount = 0;
    }

    Entity CreateEntity() {
        Entity entityId;

        if (!availableEntityIds.empty()) {
            entityId = availableEntityIds.front();
            availableEntityIds.pop();
            activeEntities[entityId] = true;
        }
        else {
            entityId = nextEntityId++;
            if (entityId >= activeEntities.size()) {
                activeEntities.resize(entityId + 1, false);
            }
            activeEntities[entityId] = true;
        }

        entityCount++;
        return entityId;
    }

    void DestroyEntity(Entity entity) {
        if (IsEntityValid(entity)) {
            entitiesToDestroy.push(entity);
        }
    }

    void FlushDestroyedEntities() {
        while (!entitiesToDestroy.empty()) {
            Entity entity = entitiesToDestroy.front();
            entitiesToDestroy.pop();
            if (!IsEntityValid(entity)) {
                continue;
            }

            for (auto& componentArray : componentArrays) {
                if (componentArray && componentArray->HasComponent(entity)) {
                    componentArray->RemoveComponent(entity);
                }
            }

            activeEntities[entity] = false;
            availableEntityIds.push(entity);
            entityCount--;
        }
    }

    bool IsEntityValid(Entity entity) const {
        return entity != NULL_ENTITY &&
            entity < activeEntities.size() &&
            activeEntities[entity];
    }

    size_t GetEntityCount() const {
        return entityCount;
    }

    template<typename T>
    void RegisterComponentType() {
        const uint32_t id = ComponentTypeId<T>();
        if (id >= componentArrays.size()) {
            componentArrays.resize(static_cast<size_t>(id) + 1);
        }
        if (componentArrays[id]) {
            throw std::invalid_argument("Component type already registered");
        }
        componentArrays[id] = std::make_unique<ComponentArray<T>>();
    }

    template<typename T>
    bool IsComponentTypeRegistered() const {
        return GetArray<T>() != nullptr;
    }

    template<typename T, typename... Args>
    T* AddComponent(Entity entity, Args&&... args) {
        if (!IsEntityValid(entity)) {
            return nullptr;
        }

        ComponentArray<T>* componentArray = GetArray<T>();
        if (!componentArray) {
            Debug::Info("ECS") << typeid(T).name() << "\n";
            throw std::invalid_argument("Component type not registered");
        }

        return componentArray->Emplace(entity, std::forward<Args>(args)...);
    }

    template<typename T>
    bool RemoveComponent(Entity entity) {
        if (!IsEntityValid(entity)) {
            return false;
        }

        ComponentArray<T>* componentArray = GetArray<T>();
        if (!componentArray || !componentArray->HasComponent(entity)) {
            return false;
        }

        componentArray->RemoveComponent(entity);
        return true;
    }

    template<typename T>
    T* GetComponent(Entity entity) {
        if (!IsEntityValid(entity)) {
            return nullptr;
        }

        ComponentArray<T>* componentArray = GetArray<T>();
        return componentArray ? componentArray->Get(entity) : nullptr;
    }

    template<typename T>
    bool HasComponent(Entity entity) const {
        if (!IsEntityValid(entity)) {
            return false;
        }

        const ComponentArray<T>* componentArray = GetArray<T>();
        return componentArray && componentArray->HasComponent(entity);
    }

    // Entities having every one of Components, in ascending entity id order (as the old full scan produced: keeps
    // iteration order, and with it any order-dependent logic, unchanged). Built by walking the smallest of the
    // component sets, so the cost follows what the query matches, not how many entities the world holds.
    template<typename... Components>
    class Query {
        EntityManager* manager;
        std::tuple<ComponentArray<Components>*...> arrays;
        std::vector<Entity> cachedEntities;
        bool cacheDirty = true;

    public:
        explicit Query(EntityManager* mgr)
            : manager(mgr), arrays(mgr->GetArray<Components>()...) {}

        class Iterator {
            EntityManager* manager;
            const std::tuple<ComponentArray<Components>*...>* arrays;
            std::vector<Entity>::const_iterator entityIt;

        public:
            Iterator(EntityManager* mgr, const std::tuple<ComponentArray<Components>*...>* arr,
                std::vector<Entity>::const_iterator it)
                : manager(mgr), arrays(arr), entityIt(it) {
            }

            std::tuple<Entity, Components*...> operator*() const {
                Entity entity = *entityIt;
                return std::make_tuple(entity, std::get<ComponentArray<Components>*>(*arrays)->Get(entity)...);
            }

            Entity GetEntity() const {
                return *entityIt;
            }

            Iterator& operator++() {
                ++entityIt;
                return *this;
            }

            bool operator!=(const Iterator& other) const {
                return entityIt != other.entityIt;
            }
        };

        Iterator begin() {
            if (cacheDirty) UpdateCache();
            return Iterator(manager, &arrays, cachedEntities.begin());
        }

        Iterator end() {
            if (cacheDirty) UpdateCache();
            return Iterator(manager, &arrays, cachedEntities.end());
        }

        void Refresh() { cacheDirty = true; }

        size_t Count() {
            if (cacheDirty) UpdateCache();
            return cachedEntities.size();
        }

        template<typename Func>
        void ForEach(Func&& func) {
            for (auto it = begin(); it != end(); ++it) {
                std::apply(func, *it);
            }
        }

        template<typename Func>
        void ForEachEntity(Func&& func) {
            for (auto it = begin(); it != end(); ++it) {
                func(it.GetEntity(), std::get<Components*>(*it)...);
            }
        }

    private:
        void UpdateCache() {
            cachedEntities.clear();
            cacheDirty = false;

            // A type not registered in this world: nothing can match (as before).
            const bool allRegistered = ((std::get<ComponentArray<Components>*>(arrays) != nullptr) && ...);
            if (!allRegistered) return;

            // Smallest set first: walk it, test the others.
            const IComponentArray* smallest = nullptr;
            const std::vector<Entity>* candidates = nullptr;
            auto pick = [&](auto* arr) {
                if (!smallest || arr->Size() < smallest->Size()) {
                    smallest = arr;
                    candidates = &arr->Entities();
                }
            };
            (pick(std::get<ComponentArray<Components>*>(arrays)), ...);

            cachedEntities.reserve(candidates->size());
            for (Entity entity : *candidates) {
                if (!manager->IsEntityValid(entity)) continue;
                if ((std::get<ComponentArray<Components>*>(arrays)->HasComponent(entity) && ...)) {
                    cachedEntities.push_back(entity);
                }
            }
            std::sort(cachedEntities.begin(), cachedEntities.end());
        }
    };

    template<typename... Components>
    Query<Components...> CreateQuery() {
        static_assert((std::is_base_of_v<IComponent, Components> && ...),
            "All types must derive from Component");
        return Query<Components...>(this);
    }

    template<typename... Components, typename Func>
    void ForEach(Func&& func) {
        auto query = CreateQuery<Components...>();
        query.ForEach(std::forward<Func>(func));
    }

    template<typename... Components, typename Func>
    void ForEachEntity(Func&& func) {
        auto query = CreateQuery<Components...>();
        query.ForEachEntity(std::forward<Func>(func));
    }
};

class ISystem {
public:
    bool emitGameFinishEvent = false;
	bool requestRenderReinit = false;

    // True for systems whose Update() only issues GL draw calls (no game/ECS
    // state); ECSWorld::Update() can skip them while the window is minimized.
    bool drawsFrame = false;

    virtual ~ISystem() = default;
    virtual void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) = 0;
};

class ECSWorld {
    EntityManager entityManager;
    std::vector<std::unique_ptr<ISystem>> systems;
    std::vector<EventEntry> events;

public:
    EntityManager& GetEntityManager() {
        return entityManager;
    }

    void AddSystem(std::unique_ptr<ISystem> system) {
        systems.push_back(std::move(system));
    }

    // Resets the world to a freshly-constructed state so InitECSLogic/InitECSRenderer can run again without double-registering.
    void Reset() {
        events.clear();
        systems.clear();
        entityManager.Reset();
    }

    // skipDrawSystems: run every system except the ones flagged drawsFrame,
    // so game/audio state keeps advancing with nothing to draw into.
    bool Update(bool isServer, float deltaTime, bool skipDrawSystems = false) {
        bool gameFinished = false;

        for (auto& system : systems) {
            if (skipDrawSystems && system->drawsFrame) continue;
            system->Update(entityManager, events, isServer, deltaTime);
            if (system->emitGameFinishEvent) gameFinished = true;
        }

        return gameFinished;
    }

    template<typename T>
    T* GetSystem() {
        static_assert(std::is_base_of<ISystem, T>::value, "T must inherit from ISystem");
        for (auto& system : systems) {
            if (auto casted = dynamic_cast<T*>(system.get())) {
                return casted;
            }
        }
        return nullptr;
    }

	std::vector<std::unique_ptr<ISystem>>& GetSystems() {
		return systems;
	}

    std::vector<EventEntry>& GetEvents() {
        return events;
    }

    void ClearEvents() {
        events.clear();
    }
};

class DestroyingSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, std::vector<EventEntry>& events, bool isServer, float deltaTime) override {
        entityManager.FlushDestroyedEntities();
    }
};

#endif // ECS_HPP