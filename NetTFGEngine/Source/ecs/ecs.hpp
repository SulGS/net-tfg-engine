#ifndef ECS_HPP
#define ECS_HPP

#include "netcode/netcode_common.hpp"
#include <unordered_map>
#include <memory>
#include <type_traits>
#include <typeindex>
#include <queue>
#include <stdexcept>

using Entity = uint32_t;
constexpr Entity NULL_ENTITY = 0;

class IComponent {
public:
    virtual ~IComponent() = default;
};

class IComponentArray {
public:
    virtual ~IComponentArray() = default;
    virtual void AddComponent(Entity entity, std::unique_ptr<IComponent> component) = 0;
    virtual void RemoveComponent(Entity entity) = 0;
    virtual IComponent* GetComponent(Entity entity) = 0;
    virtual bool HasComponent(Entity entity) const = 0;
    virtual void* GetComponentRaw(Entity entity) = 0;
};

template<typename T>
class ComponentArray : public IComponentArray {
    static_assert(std::is_base_of<IComponent, T>::value, "ComponentArray<T>: T must derive from IComponent");
    // Store components polymorphically to allow abstract interface types
    std::unordered_map<Entity, std::unique_ptr<T>> components;

    friend class EntityManager;
public:
    void AddComponent(Entity entity, std::unique_ptr<IComponent> component) override {
        if (!component) throw std::invalid_argument("AddComponent: null component");
        T* derived = dynamic_cast<T*>(component.get());
        if (!derived) throw std::invalid_argument("AddComponent: component type mismatch");

        // Transfer ownership into the map as a unique_ptr<T>
        components[entity] = std::unique_ptr<T>(static_cast<T*>(component.release()));
    }

    void RemoveComponent(Entity entity) override {
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
};


class EntityManager {
    std::unordered_map<std::type_index, std::unique_ptr<IComponentArray>> componentArrays;
    std::vector<bool> activeEntities;
    std::queue<Entity> availableEntityIds;
    std::queue<Entity> entitiesToDestroy;
    Entity nextEntityId = 1; // Start from 1, reserve 0 as NULL_ENTITY
    size_t entityCount = 0;

public:
    Entity CreateEntity() {
        Entity entityId;
        
        // Reuse available IDs first
        if (!availableEntityIds.empty()) {
            entityId = availableEntityIds.front();
            availableEntityIds.pop();
            activeEntities[entityId] = true;
        } else {
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
        if(IsEntityValid(entity))
        {
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

            // Remove all components associated with this entity
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
            std::cerr << typeIndex.name() << std::endl;
            throw std::invalid_argument("Component type not registered");
        }

        auto* componentArray = static_cast<ComponentArray<T>*>(it->second.get());

        auto component = std::make_unique<T>(std::forward<Args>(args)...);
        T* ptr = component.get();
        componentArray->AddComponent(entity, std::move(component));
        // componentArray now owns the component in its unique_ptr map
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

    // Query builder for filtering entities by components
    template<typename... Components>
    class Query {
        EntityManager* manager;
        std::vector<Entity> cachedEntities;
        bool cacheDirty = true;

    public:
        explicit Query(EntityManager* mgr) : manager(mgr) {}

        // Iterator for range-based for loops
        class Iterator {
            EntityManager* manager;
            std::vector<Entity>::const_iterator entityIt;

        public:
            Iterator(EntityManager* mgr, std::vector<Entity>::const_iterator it) 
                : manager(mgr), entityIt(it) {}

            // Returns tuple with Entity first, then component pointers
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
            if (cacheDirty) {
                UpdateCache();
            }
            return Iterator(manager, cachedEntities.begin());
        }

        Iterator end() {
            if (cacheDirty) {
                UpdateCache();
            }
            return Iterator(manager, cachedEntities.end());
        }

        // Force cache update
        void Refresh() {
            cacheDirty = true;
        }

        // Get count of matching entities
        size_t Count() {
            if (cacheDirty) {
                UpdateCache();
            }
            return cachedEntities.size();
        }

        // Execute callback for each matching entity
        template<typename Func>
        void ForEach(Func&& func) {
            for (auto it = begin(); it != end(); ++it) {
                std::apply(func, *it);
            }
        }

        // Execute callback with entity ID
        template<typename Func>
        void ForEachEntity(Func&& func) {
            for (auto it = begin(); it != end(); ++it) {
                func(it.GetEntity(), std::get<Components*>(*it)...);
            }
        }

    private:
        void UpdateCache() {
            cachedEntities.clear();
            
            // Iterate through all active entities
            for (Entity entity = 1; entity < manager->activeEntities.size(); ++entity) {
                if (!manager->IsEntityValid(entity)) {
                    continue;
                }

                // Check if entity has all required components
                if ((manager->HasComponent<Components>(entity) && ...)) {
                    cachedEntities.push_back(entity);
                }
            }
            
            cacheDirty = false;
        }
    };

    // Create a query for entities with specific components
    template<typename... Components>
    Query<Components...> CreateQuery() {
        return Query<Components...>(this);
    }

    // Helper: Execute callback for all entities with specific components
    template<typename... Components, typename Func>
    void ForEach(Func&& func) {
        auto query = CreateQuery<Components...>();
        query.ForEach(std::forward<Func>(func));
    }

    // Helper: Execute callback with entity ID
    template<typename... Components, typename Func>
    void ForEachEntity(Func&& func) {
        auto query = CreateQuery<Components...>();
        query.ForEachEntity(std::forward<Func>(func));
    }
};

class ISystem {
public:
    virtual ~ISystem() = default;
    virtual void Update(EntityManager& entityManager, float deltaTime) = 0;
};

class ECSWorld {
    EntityManager entityManager;
    std::vector<std::unique_ptr<ISystem>> systems;

public:
    EntityManager& GetEntityManager() {
        return entityManager;
    }

    void AddSystem(std::unique_ptr<ISystem> system) {
        systems.push_back(std::move(system));
    }

    void Update(float deltaTime) {
        for (auto& system : systems) {
            system->Update(entityManager, deltaTime);
        }
    }

    void Clear() {
        entityManager = EntityManager();
        systems.clear();
    }

    template<typename T>
    T* GetSystem() {
        static_assert(std::is_base_of<ISystem, T>::value, "T must inherit from ISystem");
        for (auto& system : systems) {
            if (auto casted = dynamic_cast<T*>(system.get())) {
                return casted;
            }
        }
        return nullptr; // Not found
    }

};

class DestroyingSystem : public ISystem {
public:
    void Update(EntityManager& entityManager, float deltaTime) override {
        entityManager.FlushDestroyedEntities();
    }
};

#endif // ECS_HPP
