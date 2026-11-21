#pragma once
#include <atomic>
#include <cstdint>

// Jeton d'annulation : une tâche est périmée dès que le compteur global
// de génération ne correspond plus à celui qu'elle a reçu.
struct CancelToken {
    const std::atomic<uint64_t>* current = nullptr;
    uint64_t gen = 0;
    bool cancelled() const { return current && current->load(std::memory_order_relaxed) != gen; }
};
