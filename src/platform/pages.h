#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>

// Memory straight from the system, handed back when freed. Large short-lived buffers taken from the heap
// instead leave it grown for good.
struct PageDeleter {
    void operator()(void* p) const { VirtualFree(p, 0, MEM_RELEASE); }
};

template <class T>
using Pages = std::unique_ptr<T[], PageDeleter>;

// Zeroed pages for count values of T, or null if out of memory.
template <class T>
Pages<T> AllocPages(size_t count) {
    if (count > SIZE_MAX / sizeof(T)) return nullptr;
    return Pages<T>(static_cast<T*>(VirtualAlloc(nullptr, count * sizeof(T), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)));
}
