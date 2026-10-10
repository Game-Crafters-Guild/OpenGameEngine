#include <GenerationalVector/GenerationalVector.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>

struct GameObject {
    int id;
    std::string name;
    float x, y, z;
    
    GameObject(int id, const std::string& name, float x = 0, float y = 0, float z = 0)
        : id(id), name(name), x(x), y(y), z(z) {}
    
    void Print() const {
        std::cout << "GameObject[" << id << "]: " << name 
                  << " at (" << x << ", " << y << ", " << z << ")\n";
    }
};

int main() {
    std::cout << "=== GenerationalVector Basic Example ===\n\n";
    
    // Create a container for game objects
    GenerationalVector::GenerationalVector<GameObject> gameObjects;
    
    std::cout << "1. Creating game objects...\n";
    
    // Create some game objects
    auto player = gameObjects.Create(1, "Player", 0, 0, 0);
    auto enemy1 = gameObjects.Create(2, "Enemy1", 10, 0, 5);
    auto enemy2 = gameObjects.Create(3, "Enemy2", -5, 0, 10);
    auto powerup = gameObjects.Create(4, "PowerUp", 3, 0, 7);
    
    std::cout << "Created " << gameObjects.Size() << " objects\n\n";
    
    std::cout << "2. Accessing objects via handles...\n";
    
    // Access objects safely
    if (gameObjects.IsValid(player)) {
        gameObjects[player].Print();
    }
    
    if (auto* obj = gameObjects.Get(enemy1)) {
        obj->Print();
    }
    
    std::cout << "\n3. Iterating over all objects...\n";
    
    // Iterate over all objects
    gameObjects.ForEach([](GenerationalVector::Handle handle, GameObject& obj) {
        std::cout << "Handle " << handle.Value() << ": ";
        obj.Print();
    });
    
    std::cout << "\n4. Destroying an object...\n";
    
    // Destroy enemy1
    gameObjects.Destroy(enemy1);
    std::cout << "Destroyed enemy1. Remaining objects: " << gameObjects.Size() << "\n";
    
    // Try to access destroyed object
    if (!gameObjects.IsValid(enemy1)) {
        std::cout << "enemy1 handle is now invalid (safe!)\n";
    }
    
    if (auto* obj = gameObjects.Get(enemy1)) {
        std::cout << "This won't print - handle is invalid\n";
    } else {
        std::cout << "Safe access returned nullptr for invalid handle\n";
    }
    
    std::cout << "\n5. Creating new object (handle reuse)...\n";
    
    // Create a new object - it will reuse the slot but with a new generation
    auto newEnemy = gameObjects.Create(5, "NewEnemy", 15, 0, 20);
    
    std::cout << "Created new enemy. Total objects: " << gameObjects.Size() << "\n";
    std::cout << "Old enemy1 handle: " << enemy1.Value() << " (index: " << enemy1.Index() 
              << ", gen: " << enemy1.Generation() << ")\n";
    std::cout << "New enemy handle: " << newEnemy.Value() << " (index: " << newEnemy.Index() 
              << ", gen: " << newEnemy.Generation() << ")\n";
    
    // Note: They might have the same index but different generations
    if (enemy1.Index() == newEnemy.Index()) {
        std::cout << "Slot was reused, but generations differ - old handle still invalid!\n";
    }
    
    std::cout << "\n6. Final object list...\n";
    
    gameObjects.ForEach([](GenerationalVector::Handle handle, GameObject& obj) {
        std::cout << "Handle " << handle.Value() << ": ";
        obj.Print();
    });
    
    std::cout << "\n7. Performance demonstration...\n";
    
    // Create many objects to show performance
    std::vector<GenerationalVector::Handle> handles;
    const int numObjects = 10000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < numObjects; ++i) {
        handles.push_back(gameObjects.Create(i + 100, "Object" + std::to_string(i)));
    }
    
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "Created " << numObjects << " objects in " << duration.count() << " microseconds\n";
    std::cout << "Average: " << (duration.count() / static_cast<double>(numObjects)) << " μs per object\n";
    
    // Test access performance
    start = std::chrono::high_resolution_clock::now();
    
    volatile int sum = 0; // Prevent optimization
    for (const auto& handle : handles) {
        sum += gameObjects[handle].id;
    }
    
    end = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    
    std::cout << "Accessed " << numObjects << " objects in " << duration.count() << " microseconds\n";
    std::cout << "Average: " << (duration.count() / static_cast<double>(numObjects)) << " μs per access\n";
    
    std::cout << "\nTotal objects in container: " << gameObjects.Size() << "\n";
    
    std::cout << "\n=== Example Complete ===\n";
    
    return 0;
}
