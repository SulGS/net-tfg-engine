#ifndef ECS_HPP
#define ECS_HPP

#include "netcode/netcode_common.hpp"
#include <unordered_map>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <queue>
#include <stdexcept>
#include "Utils/Debug/Debug.hpp"

using Entity = uint32_t;
constexpr Entity NULL_ENTITY = 0;

class IComponent {
public:
    virtual ~IComponent() = default;
    virtual void Destroy() {};
};

class IComponentArray {
public:
    virtual ~IComponentArray() = default;
    virtual void AddComponent(Entity entity, std::unique_ptr<IComponent> component) = 0;
    virtual void RemoveComponent(Entity entity) = 0;
    virtual IComponent* GetComponent(Entity entity) = 0;
    virtual bool HasComponent(Entity entity) const = 0;
    virtual void* GetComponentRaw(Entity entity) = 0;
    virtual void Clear() = 0;  // Destroy and remove all components
};

template<typename T>
class ComponentArray : public IComponentArray {
    static_assert(std::is_base_of<IComponent, T>::value, "ComponentArray<T>: T must derive from IComponent");
    std::unordered_map<Entity, std::unique_ptr<T>> components;

    friend class EntityManager;
public:
    void AddComponent(Entity entity, std::unique_ptr<IComponent> component) override {
        if (!component) throw std::invalid_argument("AddComponent: null component");
        T* derived = dynamic_cast<T*>(component.get());
        if (!derived) throw std::invalid_argument("AddComponent: component type mismatch");
        components[entity] = std::unique_ptr<T>(static_cast<T*>(component.release()));
    }

    void RemoveComponent(Entity entity) override {
        if (components.find(entity) != components.end()) {
            components.at(entity).get()->Destroy();
        }
        components.erase(entity);
    }

    IComponent* GetComponent(Entity entity) override {
        auto it = components.find(entity);
        if (it != components.end()) {
            return it->second.get();
        }
        return nullptr;
    }

    void* GetComponentRaw(Entity entity) override {
        auto it = components.find(entity);
        return (it != components.end()) ? it->second.get() : nullptr;
    }

    bool HasComponent(Entity entity) const override {
        return components.find(entity) != components.end();
    }

    // Remove all components, calling Destroy() on each.
    void Clear() {
        for (auto& [entity, comp] : components) {
            if (comp) comp->Destroy();
        }
        components.clear();
    }
};


class EntityManager {
    std::unordered_map<std::type_index, std::unique_ptr<IComponentArray>> componentArrays;
    std::vector<bool> activeEntities;
    std::queue<Entity> availableEntityIds;
    std::queue<Entity> entitiesToDestroy;
    Entity nextEntityId = 1;
    size_t entityCount = 0;

    std::mutex entityMutex;

public:

    void acquireMutex() {
        entityMutex.lock();
    }

    void releaseMutex() {
        entityMutex.unlock();
    }

    // Reset to a completely clean state.
    // Calls Destroy() on every live component, clears all entities and
    // component arrays, and resets all ID counters.
    // Registered component types are also cleared so InitECSLogic /
    // InitECSRenderer can RegisterComponentType again from scratch.
    void Reset() {
        // Destroy all components in every array
        for (auto& [typeIndex, array] : componentArrays) {
            array->Clear();
        }

        componentArrays.clear();
        activeEntities.clear();

        // Drain the pending-destroy queue
        while (!entitiesToDestroy.empty()) entitiesToDestroy.pop();
        // Drain the recycled-ID queue
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

            for (auto& [typeIndex, componentArray] : componentArrays) {
                if (componentArray->HasComponent(entity)) {
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
        std::type_index typeIndex(typeid(T));
        if (componentArrays.find(typeIndex) != componentArrays.end()) {
            throw std::invalid_argument("Component type already registered");
        }
        componentArrays[typeIndex] = std::make_unique<ComponentArray<T>>();
    }

    template<typename T>
    bool IsComponentTypeRegistered() const {
        std::type_index typeIndex(typeid(T));
        return componentArrays.find(typeIndex) != componentArrays.end();
    }

    template<typename T, typename... Args>
    T* AddComponent(Entity entity, Args&&... args) {
        if (!IsEntityValid(entity)) {
            return nullptr;
        }

        std::type_index typeIndex(typeid(T));
        auto it = componentArrays.find(typeIndex);
        if (it == componentArrays.end()) {
            Debug::Info("ECS") << typeIndex.name() << "\n";
            throw std::invalid_argument("Component type not registered");
        }

        auto* componentArray = static_cast<ComponentArray<T>*>(it->second.get());

        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = component.get();
        componentArray->AddComponent(entity, std::move(component));
        return componentArray->components[entity].get();
    }

    template<typename T>
    bool RemoveComponent(Entity entity) {
        if (!IsEntityValid(entity)) {
            return false;
        }

        std::type_index typeIndex(typeid(T));
        auto it = componentArrays.find(typeIndex);
        if (it == componentArrays.end()) {
            return false;
        }

        auto* componentArray = static_cast<ComponentArray<T>*>(it->second.get());
        if (!componentArray->HasComponent(entity)) {
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

        std::type_index typeIndex(typeid(T));
        auto it = componentArrays.find(typeIndex);
        if (it == componentArrays.end()) {
            return nullptr;
        }

        auto* componentArray = static_cast<ComponentArray<T>*>(it->second.get());
        return static_cast<T*>(componentArray->GetComponentRaw(entity));
    }

    template<typename T>
    bool HasComponent(Entity entity) const {
        if (!IsEntityValid(entity)) {
            return false;
        }

        std::type_index typeIndex(typeid(T));
        auto it = componentArrays.find(typeIndex);
        if (it == componentArrays.end()) {
            return false;
        }

        return it->second->HasComponent(entity);
    }

    template<typename... Components>
    class Query {
        EntityManager* manager;
        std::vector<Entity> cachedEntities;
        bool cacheDirty = true;

    public:
        explicit Query(EntityManager* mgr) : manager(mgr) {}

        class Iterator {
            EntityManager* manager;
            std::vector<Entity>::const_iterator entityIt;

        public:
            Iterator(EntityManager* mgr, std::vector<Entity>::const_iterator it)
                : manager(mgr), entityIt(it) {
            }

            std::tuple<Entity, Components*...> operator*() const {
                Entity entity = *entityIt;
                return std::make_tuple(entity, manager->GetComponent<Components>(entity)...);
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
            return Iterator(manager, cachedEntities.begin());
        }

        Iterator end() {
            if (cacheDirty) UpdateCache();
            return Iterator(manager, cachedEntities.end());
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
            for (Entity entity = 1; entity < manager->activeEntities.size(); ++entity) {
                if (!manager->IsEntityValid(entity)) continue;
                if ((manager->HasComponent<Components>(entity) && ...)) {
                    cachedEntities.push_back(entity);
                }
            }
            cacheDirty = false;
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

    // Reset the entire world to a blank slate.
    // Destroys all components (calling Destroy() on each), clears all entities,
    // clears all systems, and clears all registered component types.
    // After this call the world is in the same state as a freshly constructed one,
    // so InitECSLogic / InitECSRenderer can run again without double-registering.
    void Reset() {
        events.clear();
        systems.clear();
        entityManager.Reset();
    }

    bool Update(bool isServer, float deltaTime) {
        bool gameFinished = false;

        for (auto& system : systems) {
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