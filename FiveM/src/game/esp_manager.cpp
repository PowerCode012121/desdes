#include "esp_manager.h"
#include "offsets.h"
#include "../esp/esp.h"
#include "../playerInfo/PedData.h"
#include "../DMALibrary/Memory/Memory.h"
#include <iostream>

namespace FiveM {
    namespace ESP {
        // Constants definition
        const int MAX_PEDS = 110;

        // Global containers
        std::vector<uintptr_t> rawPedPointers;
        std::vector<Vec3> positions;
        std::vector<uintptr_t> validPeds;
        std::vector<Vec2> screenPositions;

        // Performance tracking
        int frameCount = 0;
        std::chrono::steady_clock::time_point lastFrameTime;

        // Initialize containers with reserved memory
        void InitializeContainers() {
            static bool initialized = false;
            if (!initialized) {
                rawPedPointers.reserve(MAX_PEDS);
                positions.reserve(MAX_PEDS);
                validPeds.reserve(MAX_PEDS);
                screenPositions.reserve(MAX_PEDS);
                initialized = true;
                lastFrameTime = std::chrono::steady_clock::now();
            }
        }

        // Single-threaded main loop - called every frame
        void RunESP() {
            InitializeContainers();

            // Performance tracking
            frameCount++;
            auto currentTime = std::chrono::steady_clock::now();

            // Collect frame data
            collectFrameData();

            // Render ESP
            renderESP();

            // Update cache with collected data (fast cache) - only if cache is enabled
            if (!validPeds.empty() && esp::get_use_cache()) {
                g_pedCacheManager.fastCache(validPeds, positions);
                // Update the cache manager (handles slow cache updates when needed)
                g_pedCacheManager.update();
            }

            lastFrameTime = currentTime;
        }

        // Data collection (now synchronous)
        void collectFrameData() {
            // Single scatter handle for fast operations
            auto handle = mem.CreateScatterHandle();

            // Fast critical data reads
            Matrix view_matrix;
            Vec3 localPos;
            uintptr_t ped_replay_interface = 0;
            uintptr_t pedListBase = 0;

            // Batch critical reads
            mem.AddScatterReadRequest(handle, offset::viewport + 0x24C,
                &view_matrix, sizeof(Matrix));
            mem.AddScatterReadRequest(handle, offset::localplayer + offset::playerPosition,
                &localPos, sizeof(Vec3));
            mem.AddScatterReadRequest(handle, offset::replay + 0x18,
                &ped_replay_interface, sizeof(uintptr_t));

            mem.ExecuteReadScatter(handle);

            if (ped_replay_interface) {
                mem.AddScatterReadRequest(handle, ped_replay_interface + 0x100,
                    &pedListBase, sizeof(uintptr_t));
                mem.ExecuteReadScatter(handle);

                if (pedListBase) {
                    rawPedPointers.clear();
                    rawPedPointers.resize(MAX_PEDS);

                    // First read all ped pointers
                    mem.AddScatterReadRequest(handle, pedListBase,
                        rawPedPointers.data(), sizeof(uintptr_t) * MAX_PEDS);
                    mem.ExecuteReadScatter(handle);

                    // Then batch read playerInfo for all peds
                    std::vector<uintptr_t> playerInfoPtrs(MAX_PEDS);
                    for (int i = 0; i < MAX_PEDS; i++) {
                        if (rawPedPointers[i] && rawPedPointers[i] != offset::localplayer) {
                            mem.AddScatterReadRequest(handle, rawPedPointers[i] + offset::playerInfo,
                                &playerInfoPtrs[i], sizeof(uintptr_t));
                        }
                    }
                    mem.ExecuteReadScatter(handle);

                    // Now filter based on playerInfo
                    validPeds.clear();
                    for (int i = 0; i < MAX_PEDS; i++) {
                        if (rawPedPointers[i] && rawPedPointers[i] != offset::localplayer && playerInfoPtrs[i]) {
                            validPeds.push_back(rawPedPointers[i]);
                        }
                    }

                    // Fast position reads
                    if (!validPeds.empty()) {
                        positions.clear();
                        positions.resize(validPeds.size());

                        for (size_t i = 0; i < validPeds.size(); i++) {
                            mem.AddScatterReadRequest(handle, validPeds[i] + offset::playerPosition,
                                &positions[i], sizeof(Vec3));
                        }
                        mem.ExecuteReadScatter(handle);
                    }
                }
            }

            mem.CloseScatterHandle(handle);
        }

        // Rendering operations (now with batch skeleton support)
        void renderESP() {
           if (validPeds.empty() || positions.empty()) {
               return;
           }

            // Get view matrix from cache or current frame
            Matrix view_matrix;
            auto handle = mem.CreateScatterHandle();
            mem.AddScatterReadRequest(handle, offset::viewport + 0x24C,
                &view_matrix, sizeof(Matrix));
            mem.ExecuteReadScatter(handle);
            mem.CloseScatterHandle(handle);

            // Check current ESP mode
            esp::ESPMode current_mode = esp::get_esp_mode();

            // NEW: Use batch rendering if enabled and we have enough peds
            if (esp::get_use_batch_skeleton() && validPeds.size() >= 3) {
                if (current_mode == esp::ESPMode::SKELETON_BONES) {
                    // Use batch skeleton rendering
                    esp::render_skeleton_esp_batch();
                }
                else if (current_mode == esp::ESPMode::HEAD_CIRCLE) {
                    // Use batch head circle rendering
                    esp::render_head_circle_esp_batch();
                }
            }
            else {
                // Use existing individual rendering logic

                // Batch update bone cache for better performance - only if cache is enabled
                if (esp::get_use_cache()) {
                    if (current_mode == esp::ESPMode::HEAD_CIRCLE) {
                        esp::batch_update_bone_cache(validPeds);
                    }
                    else if (current_mode == esp::ESPMode::SKELETON_BONES) {
                        // Pre-load skeleton data for individual rendering
                        esp::batch_update_skeleton_cache(validPeds);
                    }
                }

                // Render peds using the existing system
                screenPositions.clear();
                screenPositions.resize(validPeds.size());

                for (size_t i = 0; i < validPeds.size(); i++) {
                    Vec2 screenPos;
                    if (positions[i].world_to_screen(view_matrix, screenPos)) {
                        screenPositions[i] = screenPos;

                        // Use the existing ESP rendering system
                        PedData cachedData;
                        if (esp::get_use_cache() && g_pedCacheManager.getPedData(validPeds[i], cachedData)) {
                            // Enhanced rendering with cached data - uses current ESP mode
                            esp::render_esp_for_ped_cached(validPeds[i], view_matrix, offset::localplayer, cachedData);
                        }
                        else {
                            // Fallback rendering - uses current ESP mode
                            esp::render_esp_for_ped(validPeds[i], view_matrix, offset::localplayer);
                        }
                    }
                }
            }

            // Print performance stats periodically
            esp::print_esp_stats();
        }

        // Performance monitoring
        void printPerformanceStats() {
            static auto lastPrint = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();

            if (now - lastPrint >= std::chrono::seconds(5)) {
                auto fps = frameCount / 5.0;
                auto cacheSize = g_pedCacheManager.getCacheSize();

                std::cout << "[ESP] FPS: " << fps
                    << ", Cache Size: " << cacheSize
                    << ", Valid Peds: " << validPeds.size()
                    << ", Batch Mode: " << (esp::get_use_batch_skeleton() ? "ON" : "OFF") << std::endl;

                frameCount = 0;
                lastPrint = now;
            }
        }

        // Manual cache refresh (called when needed)
        void refreshCache() {
            g_pedCacheManager.manualCache();
        }
    }
}