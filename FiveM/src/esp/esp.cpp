#include "esp.h"
#include "../math/math.h"
#include "../game/game.h"
#include "../../ImGui/imgui.h"
#include <iostream>
#include "../playerInfo/PedData.h"
#include <cmath>
#include <algorithm>
#include <Memory/Memory.h>

// Initialize global instances
esp::BoneCache esp::bone_cache;
esp::ESPStats esp::esp_stats;

// ESP Configuration globals
esp::ESPMode esp::current_esp_mode = esp::ESPMode::HEAD_CIRCLE; // Default to head circle
ImU32 esp::circle_color = IM_COL32(0, 255, 0, 255); // Green circles
ImU32 esp::skeleton_color = IM_COL32(255, 0, 0, 255); // Red skeleton
float esp::line_thickness = 2.0f;
bool esp::use_cache = true;
bool esp::use_batch_skeleton = true;  // NEW: Enable batch skeleton by default

// Enhanced skeleton data structure for caching
struct CachedSkeletonData {
    std::vector<Vec3> bone_positions;  // Cache all bone positions
    std::chrono::steady_clock::time_point last_update;
    bool is_valid;

    CachedSkeletonData() : last_update(std::chrono::steady_clock::now()), is_valid(false) {
        bone_positions.resize(9); // Pre-allocate for bones 0-8
    }
};

// Enhanced bone cache for skeleton
class EnhancedBoneCache {
public:  // Made public for batch updates
    std::unordered_map<uintptr_t, CachedSkeletonData> skeleton_cache;

private:
    static constexpr std::chrono::milliseconds SKELETON_CACHE_VALIDITY_MS{ 45 }; // Slightly longer for skeleton
    std::chrono::steady_clock::time_point last_cleanup;

public:
    EnhancedBoneCache() : last_cleanup(std::chrono::steady_clock::now()) {}

    // Get all bone positions for skeleton at once
    bool get_skeleton_bones(uintptr_t ped, std::vector<Vec3>& bone_positions, bool force_refresh = false) {
        auto now = std::chrono::steady_clock::now();

        auto it = skeleton_cache.find(ped);
        if (!force_refresh && it != skeleton_cache.end() && it->second.is_valid) {
            auto age = now - it->second.last_update;
            if (age < SKELETON_CACHE_VALIDITY_MS) {
                bone_positions = it->second.bone_positions;
                esp::esp_stats.cache_hits++;
                return true;
            }
        }

        esp::esp_stats.cache_misses++;

        // Read all skeleton bones in one batch operation
        Matrix bone_matrix = esp::bone_cache.get_bone_matrix(ped, force_refresh);

        // Read all bone offsets we need for skeleton
        std::vector<int> bone_indices = { 0, 3, 4, 5, 6, 7, 8 }; // All bones needed for skeleton
        std::vector<Vector3> bone_offsets(bone_indices.size());

        auto handle = mem.CreateScatterHandle();
        for (size_t i = 0; i < bone_indices.size(); ++i) {
            mem.AddScatterReadRequest(handle, ped + (esp::BONE_ARRAY_BASE + esp::BONE_SIZE * bone_indices[i]),
                &bone_offsets[i], sizeof(Vector3));
        }
        mem.ExecuteReadScatter(handle);
        mem.CloseScatterHandle(handle);

        // Transform all bones at once
        bone_positions.resize(9); // Ensure proper size
        for (size_t i = 0; i < bone_indices.size(); ++i) {
            DirectX::SimpleMath::Vector3 boneVec(bone_offsets[i].x, bone_offsets[i].y, bone_offsets[i].z);
            DirectX::SimpleMath::Vector3 transformedBoneVec = DirectX::XMVector3Transform(boneVec, bone_matrix);
            bone_positions[bone_indices[i]] = Vec3(transformedBoneVec.x, transformedBoneVec.y, transformedBoneVec.z);
        }

        // Cache the results
        auto& cached_data = skeleton_cache[ped];
        cached_data.bone_positions = bone_positions;
        cached_data.last_update = now;
        cached_data.is_valid = true;

        esp::esp_stats.memory_reads += bone_indices.size();
        return true;
    }

    void cleanup_skeleton_cache() {
        auto now = std::chrono::steady_clock::now();

        if (now - last_cleanup < std::chrono::seconds(3)) {
            return;
        }

        for (auto it = skeleton_cache.begin(); it != skeleton_cache.end(); ) {
            auto age = now - it->second.last_update;
            if (age > std::chrono::seconds(15)) {
                it = skeleton_cache.erase(it);
            }
            else {
                ++it;
            }
        }

        last_cleanup = now;
    }

    size_t get_skeleton_cache_size() const { return skeleton_cache.size(); }
    void clear_skeleton_cache() { skeleton_cache.clear(); }
};

// Global enhanced bone cache instance
static EnhancedBoneCache enhanced_bone_cache;

// Bone Cache Implementation (keeping your existing logic)
Matrix esp::BoneCache::get_bone_matrix(uintptr_t ped, bool force_refresh) {
    auto now = std::chrono::steady_clock::now();

    auto it = cached_bone_data.find(ped);
    if (!force_refresh && it != cached_bone_data.end() && it->second.is_valid) {
        auto age = now - it->second.last_update;
        if (age < CACHE_VALIDITY_MS) {
            esp_stats.cache_hits++;
            return it->second.bone_matrix;
        }
    }

    esp_stats.cache_misses++;
    esp_stats.memory_reads++;

    Matrix bone_matrix;
    auto handle = mem.CreateScatterHandle();
    mem.AddScatterReadRequest(handle, ped + BONE_MATRIX_OFFSET, &bone_matrix, sizeof(Matrix));
    mem.ExecuteReadScatter(handle);
    mem.CloseScatterHandle(handle);

    auto& cached_data = cached_bone_data[ped];
    cached_data.bone_matrix = bone_matrix;
    cached_data.last_update = now;
    cached_data.is_valid = true;

    return bone_matrix;
}

Vec3 esp::BoneCache::get_bone_position(uintptr_t ped, int bone_position, bool force_refresh) {
    auto now = std::chrono::steady_clock::now();

    if (bone_position == 0) {
        auto it = cached_bone_data.find(ped);
        if (!force_refresh && it != cached_bone_data.end() && it->second.is_valid) {
            auto age = now - it->second.last_update;
            if (age < CACHE_VALIDITY_MS) {
                esp_stats.cache_hits++;
                return it->second.head_position;
            }
        }
    }

    esp_stats.cache_misses++;
    esp_stats.memory_reads++;

    Matrix bone_matrix = get_bone_matrix(ped, force_refresh);

    Vector3 bone_offset;
    auto handle = mem.CreateScatterHandle();
    mem.AddScatterReadRequest(handle, ped + (BONE_ARRAY_BASE + BONE_SIZE * bone_position),
        reinterpret_cast<void*>(&bone_offset), sizeof(Vector3));
    mem.ExecuteReadScatter(handle);
    mem.CloseScatterHandle(handle);

    DirectX::SimpleMath::Vector3 boneVec(bone_offset.x, bone_offset.y, bone_offset.z);
    DirectX::SimpleMath::Vector3 transformedBoneVec = DirectX::XMVector3Transform(boneVec, bone_matrix);
    Vec3 result(transformedBoneVec.x, transformedBoneVec.y, transformedBoneVec.z);

    if (bone_position == 0) {
        auto& cached_data = cached_bone_data[ped];
        cached_data.head_position = result;
        cached_data.last_update = now;
        cached_data.is_valid = true;
    }

    return result;
}

void esp::BoneCache::update_bone_data(uintptr_t ped, const Matrix& bone_matrix, const Vec3& head_pos) {
    auto now = std::chrono::steady_clock::now();
    auto& cached_data = cached_bone_data[ped];

    cached_data.bone_matrix = bone_matrix;
    cached_data.head_position = head_pos;
    cached_data.last_update = now;
    cached_data.is_valid = true;
}

void esp::BoneCache::cleanup_old_entries() {
    auto now = std::chrono::steady_clock::now();

    if (now - last_cleanup < CLEANUP_INTERVAL) {
        return;
    }

    for (auto it = cached_bone_data.begin(); it != cached_bone_data.end(); ) {
        auto age = now - it->second.last_update;
        if (age > std::chrono::seconds(10)) {
            it = cached_bone_data.erase(it);
        }
        else {
            ++it;
        }
    }

    last_cleanup = now;
}

bool esp::BoneCache::is_data_valid(uintptr_t ped) const {
    auto it = cached_bone_data.find(ped);
    if (it == cached_bone_data.end()) return false;

    auto now = std::chrono::steady_clock::now();
    auto age = now - it->second.last_update;
    return it->second.is_valid && age < CACHE_VALIDITY_MS;
}


// ESP Mode Management
void esp::set_esp_mode(ESPMode mode) {
    current_esp_mode = mode;
}

esp::ESPMode esp::get_esp_mode() {
    return current_esp_mode;
}

const char* esp::get_esp_mode_name(ESPMode mode) {
    switch (mode) {
    case ESPMode::HEAD_CIRCLE: return "Head Circle";
    case ESPMode::SKELETON_BONES: return "Skeleton Bones";
    default: return "Unknown";
    }
}

std::vector<const char*> esp::get_esp_mode_names() {
    return {
        "Head Circle",
        "Skeleton Bones"
    };
}

// NEW: Batch skeleton reading implementation
void esp::batch_read_skeleton_data(const std::vector<uintptr_t>& peds, std::vector<BatchSkeletonData>& out_data) {
    if (peds.empty()) return;

    // Pre-allocate output data
    out_data.clear();
    out_data.resize(peds.size());

    // Define which bones we need for skeleton
    const std::vector<int> skeleton_bones = { 0, 3, 4, 5, 6, 7, 8 };
    const size_t num_bones = skeleton_bones.size();

    // Create single scatter handle for ALL reads
    auto handle = mem.CreateScatterHandle();

    // First pass: Add all bone matrix read requests
    for (size_t i = 0; i < peds.size(); ++i) {
        out_data[i].ped = peds[i];
        mem.AddScatterReadRequest(handle, peds[i] + BONE_MATRIX_OFFSET,
            &out_data[i].bone_matrix, sizeof(Matrix));
    }

    // Second pass: Add all bone offset read requests
    for (size_t ped_idx = 0; ped_idx < peds.size(); ++ped_idx) {
        for (size_t bone_idx = 0; bone_idx < skeleton_bones.size(); ++bone_idx) {
            int bone_id = skeleton_bones[bone_idx];
            mem.AddScatterReadRequest(handle,
                peds[ped_idx] + (BONE_ARRAY_BASE + BONE_SIZE * bone_id),
                &out_data[ped_idx].bone_offsets[bone_id],
                sizeof(Vector3));
        }
    }

    // Execute ALL reads in one operation
    mem.ExecuteReadScatter(handle);
    mem.CloseScatterHandle(handle);

    // Mark valid entries
    for (auto& data : out_data) {
        data.valid = true;
    }

    // Update stats
    esp_stats.memory_reads += peds.size() * (1 + skeleton_bones.size());
    esp_stats.batch_reads++;
}

// NEW: Batch skeleton rendering
void esp::render_batch_skeletons(const std::vector<BatchSkeletonData>& skeleton_data,
    Matrix viewport, uintptr_t localplayer) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    // Bone connections for skeleton
    static const int bone_connections[][2] = {
        { 0, 7 }, { 7, 6 }, { 7, 5 }, { 7, 8 }, { 8, 3 }, { 8, 4 }
    };

    // Pre-calculate local player position for distance calculations
    Vec3 local_pos = mem.Read<Vec3>(localplayer + 0x90);

    for (const auto& data : skeleton_data) {
        if (!data.valid) continue;

        // Transform all bones for this ped
        std::vector<Vec3> world_positions(9);
        std::vector<Vec2> screen_positions(9);
        std::vector<bool> on_screen(9, false);

        // Batch transform bones to world space
        for (int bone_id : {0, 3, 4, 5, 6, 7, 8}) {
            DirectX::SimpleMath::Vector3 boneVec(
                data.bone_offsets[bone_id].x,
                data.bone_offsets[bone_id].y,
                data.bone_offsets[bone_id].z
            );
            DirectX::SimpleMath::Vector3 transformedBoneVec =
                DirectX::XMVector3Transform(boneVec, data.bone_matrix);

            world_positions[bone_id] = Vec3(
                transformedBoneVec.x,
                transformedBoneVec.y,
                transformedBoneVec.z
            );

            // Convert to screen space
            on_screen[bone_id] = world_positions[bone_id].world_to_screen(
                viewport, screen_positions[bone_id]
            );
        }

        // Get health for coloring (from cache if available)
        float health = 100.0f;
        PedData cached_ped_data;
        if (g_pedCacheManager.getPedData(data.ped, cached_ped_data)) {
            health = cached_ped_data.health;
        }

        // Determine skeleton color based on health
        ImU32 current_skeleton_color = skeleton_color;
        if (health < 50.0f) {
            current_skeleton_color = IM_COL32(255, 255, 0, 255);
        }
        if (health < 25.0f) {
            current_skeleton_color = IM_COL32(255, 100, 0, 255);
        }
        if (health <= 0.0f) {
            current_skeleton_color = IM_COL32(100, 100, 100, 255);
        }

        // Draw skeleton connections
        for (const auto& connection : bone_connections) {
            int bone1 = connection[0];
            int bone2 = connection[1];

            if (on_screen[bone1] && on_screen[bone2]) {
                draw_list->AddLine(
                    ImVec2(screen_positions[bone1].x, screen_positions[bone1].y),
                    ImVec2(screen_positions[bone2].x, screen_positions[bone2].y),
                    current_skeleton_color,
                    line_thickness
                );

                // Draw joint circles
                if (health > 0.0f) {
                    float joint_radius = line_thickness * 0.75f;
                    draw_list->AddCircleFilled(
                        ImVec2(screen_positions[bone1].x, screen_positions[bone1].y),
                        joint_radius,
                        current_skeleton_color
                    );
                    draw_list->AddCircleFilled(
                        ImVec2(screen_positions[bone2].x, screen_positions[bone2].y),
                        joint_radius,
                        current_skeleton_color
                    );
                }
            }
        }

        // Draw head indicator
        if (on_screen[0]) {
            float distance = local_pos.distance_to(world_positions[0]);
            float distance_factor = 50.0f / distance;
            if (distance_factor < 0.2f) distance_factor = 0.2f;
            if (distance_factor > 2.0f) distance_factor = 2.0f;

            float head_radius = 4.0f * distance_factor;
            if (head_radius < 1.0f) head_radius = 1.0f;
            if (head_radius > 8.0f) head_radius = 8.0f;

            draw_list->AddCircle(
                ImVec2(screen_positions[0].x, screen_positions[0].y),
                head_radius,
                current_skeleton_color,
                12,
                head_radius * 0.25f
            );
        }
    }

    // Update cache with transformed data
    for (const auto& data : skeleton_data) {
        if (data.valid) {
            std::vector<Vec3> bone_positions(9);
            for (int bone_id : {0, 3, 4, 5, 6, 7, 8}) {
                DirectX::SimpleMath::Vector3 boneVec(
                    data.bone_offsets[bone_id].x,
                    data.bone_offsets[bone_id].y,
                    data.bone_offsets[bone_id].z
                );
                DirectX::SimpleMath::Vector3 transformedBoneVec =
                    DirectX::XMVector3Transform(boneVec, data.bone_matrix);
                bone_positions[bone_id] = Vec3(
                    transformedBoneVec.x,
                    transformedBoneVec.y,
                    transformedBoneVec.z
                );
            }

            // Update enhanced bone cache
            auto& cached_data = enhanced_bone_cache.skeleton_cache[data.ped];
            cached_data.bone_positions = bone_positions;
            cached_data.last_update = std::chrono::steady_clock::now();
            cached_data.is_valid = true;
        }
    }
}

// NEW: Batch skeleton ESP rendering function
void esp::render_skeleton_esp_batch() {
    // Get all valid peds
    std::vector<uintptr_t> valid_peds = g_pedCacheManager.getValidPedIds();
    if (valid_peds.empty()) return;

    // Get viewport matrix once
    Matrix view_matrix;
    auto handle = mem.CreateScatterHandle();
    mem.AddScatterReadRequest(handle, FiveM::offset::viewport + 0x24C,
        &view_matrix, sizeof(Matrix));
    mem.ExecuteReadScatter(handle);
    mem.CloseScatterHandle(handle);

    // Batch read all skeleton data
    std::vector<BatchSkeletonData> skeleton_data;
    batch_read_skeleton_data(valid_peds, skeleton_data);

    // Render all skeletons in one pass
    render_batch_skeletons(skeleton_data, view_matrix, FiveM::offset::localplayer);

    // Clean up old cache entries
    enhanced_bone_cache.cleanup_skeleton_cache();
}

// NEW: Batch head circle reading implementation
void esp::batch_read_head_data(const std::vector<uintptr_t>& peds, std::vector<BatchHeadData>& out_data) {
    if (peds.empty()) return;

    // Pre-allocate output data
    out_data.clear();
    out_data.resize(peds.size());

    // Create single scatter handle for ALL reads
    auto handle = mem.CreateScatterHandle();

    // Add all read requests
    for (size_t i = 0; i < peds.size(); ++i) {
        out_data[i].ped = peds[i];

        // Read bone matrix
        mem.AddScatterReadRequest(handle, peds[i] + BONE_MATRIX_OFFSET,
            &out_data[i].bone_matrix, sizeof(Matrix));

        // Read head offset (bone 0)
        mem.AddScatterReadRequest(handle, peds[i] + (BONE_ARRAY_BASE + BONE_SIZE * 0),
            &out_data[i].head_offset, sizeof(Vector3));
    }

    // Execute ALL reads in one operation
    mem.ExecuteReadScatter(handle);
    mem.CloseScatterHandle(handle);

    // Mark valid entries
    for (auto& data : out_data) {
        data.valid = true;
    }

    // Update stats
    esp_stats.memory_reads += peds.size() * 2;
    esp_stats.batch_reads++;
}

// NEW: Batch head circle rendering
void esp::render_batch_head_circles(const std::vector<BatchHeadData>& head_data,
    Matrix viewport, uintptr_t localplayer) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    for (const auto& data : head_data) {
        if (!data.valid) continue;

        // Transform head bone to world space
        DirectX::SimpleMath::Vector3 boneVec(
            data.head_offset.x,
            data.head_offset.y,
            data.head_offset.z
        );
        DirectX::SimpleMath::Vector3 transformedBoneVec =
            DirectX::XMVector3Transform(boneVec, data.bone_matrix);

        Vec3 head_world_pos(
            transformedBoneVec.x,
            transformedBoneVec.y,
            transformedBoneVec.z
        );

        // Convert to screen space
        Vec2 head_screen_pos;
        if (head_world_pos.world_to_screen(viewport, head_screen_pos)) {
            // Get health for coloring (from cache if available)
            float health = 100.0f;
            PedData cached_ped_data;
            if (g_pedCacheManager.getPedData(data.ped, cached_ped_data)) {
                health = cached_ped_data.health;
            }

            // Determine color based on health
            ImU32 color = circle_color;
            if (health < 50.0f) {
                color = IM_COL32(255, 255, 0, 255);
            }
            if (health < 25.0f) {
                color = IM_COL32(255, 0, 0, 255);
            }
            if (health <= 0.0f) {
                color = IM_COL32(100, 100, 100, 255);
            }

            // Draw circle
            draw_list->AddCircle(
                ImVec2(head_screen_pos.x, head_screen_pos.y),
                4.0f,
                color,
                20,
                2.0f
            );
        }

        // Update cache
        bone_cache.update_bone_data(data.ped, data.bone_matrix, head_world_pos);
    }
}

// NEW: Batch head circle ESP rendering function
void esp::render_head_circle_esp_batch() {
    // Get all valid peds
    std::vector<uintptr_t> valid_peds = g_pedCacheManager.getValidPedIds();
    if (valid_peds.empty()) return;

    // Get viewport matrix once
    Matrix view_matrix;
    auto handle = mem.CreateScatterHandle();
    mem.AddScatterReadRequest(handle, FiveM::offset::viewport + 0x24C,
        &view_matrix, sizeof(Matrix));
    mem.ExecuteReadScatter(handle);
    mem.CloseScatterHandle(handle);

    // Batch read all head data
    std::vector<BatchHeadData> head_data;
    batch_read_head_data(valid_peds, head_data);

    // Render all head circles in one pass
    render_batch_head_circles(head_data, view_matrix, FiveM::offset::localplayer);

    // Clean up old cache entries
    bone_cache.cleanup_old_entries();
}

// Main rendering dispatcher - chooses between head circle or skeleton
void esp::render_esp_for_ped(uintptr_t ped, Matrix viewport, uintptr_t localplayer) {
    switch (current_esp_mode) {
    case ESPMode::HEAD_CIRCLE:
        draw_head_circle(ped, viewport, localplayer);
        break;
    case ESPMode::SKELETON_BONES:
        draw_skeleton(ped, viewport, localplayer);
        break;
    }
}

void esp::render_esp_for_ped_cached(uintptr_t ped, Matrix viewport, uintptr_t localplayer, const PedData& cached_ped_data) {
    switch (current_esp_mode) {
    case ESPMode::HEAD_CIRCLE:
        draw_head_circle_cached(ped, viewport, localplayer, cached_ped_data);
        break;
    case ESPMode::SKELETON_BONES:
        draw_skeleton_cached(ped, viewport, localplayer, cached_ped_data);
        break;
    }
}

// HEAD CIRCLE ESP (modified to use new health bar)
void esp::draw_head_circle(uintptr_t ped, Matrix viewport, uintptr_t localplayer) {
    PedData cached_ped_data;
    if (g_pedCacheManager.getPedData(ped, cached_ped_data)) {
        draw_head_circle_cached(ped, viewport, localplayer, cached_ped_data);
        return;
    }

    // Fallback to direct memory access
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    Vec2 head_screen_pos;
    Vec3 head_world_pos = bone_cache.get_bone_position(ped, 0);

    if (head_world_pos.world_to_screen(viewport, head_screen_pos)) {
        // Basic circle
        draw_list->AddCircle(
            ImVec2(head_screen_pos.x, head_screen_pos.y),
            4.0f,
            circle_color,
            20,
            2.0f
        );
    }

    bone_cache.cleanup_old_entries();
}

void esp::draw_head_circle_cached(uintptr_t ped, Matrix viewport, uintptr_t localplayer, const PedData& cached_ped_data) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    Vec2 head_screen_pos;
    Vec3 head_world_pos;

    // Use cached position if available and recent
    if (bone_cache.is_data_valid(ped)) {
        head_world_pos = bone_cache.get_bone_position(ped, 0, false); // Use cache
        esp_stats.cache_hits++;
    }
    else {
        // Use ped position from cache as fallback
        head_world_pos = cached_ped_data.position_origin;
        head_world_pos.z += 1.0f; // Approximate head height offset
    }

    if (head_world_pos.world_to_screen(viewport, head_screen_pos)) {
        // Determine color based on health
        ImU32 color = circle_color; // Default color
        if (cached_ped_data.health < 50.0f) {
            color = IM_COL32(255, 255, 0, 255); // Yellow for injured
        }
        if (cached_ped_data.health < 25.0f) {
            color = IM_COL32(255, 0, 0, 255); // Red for critical
        }
        if (cached_ped_data.health <= 0.0f) {
            color = IM_COL32(100, 100, 100, 255); // Gray for dead
        }

        // Draw enhanced circle with health-based coloring
        draw_list->AddCircle(
            ImVec2(head_screen_pos.x, head_screen_pos.y),
            4.0f,
            color,
            20,
            2.0f
        );
    }
}

// SKELETON ESP (modified to use new health bar)
void esp::draw_skeleton_cached(uintptr_t ped, Matrix viewport, uintptr_t localplayer, const PedData& cached_ped_data) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    // Bone connections for skeleton (correct layout)
    static const int bone_connections[][2] = {
        { 0, 7 },  // Head to neck
        { 7, 6 },  // Neck to spine
        { 7, 5 },  // Neck to left shoulder  
        { 7, 8 },  // Neck to right shoulder
        { 8, 3 },  // Right shoulder to right hand
        { 8, 4 }   // Right shoulder to left hand
    };

    // Get all bone positions from cache
    std::vector<Vec3> bone_positions;
    if (!enhanced_bone_cache.get_skeleton_bones(ped, bone_positions)) {
        return; // Failed to get bone data
    }

    // Determine skeleton color based on health
    ImU32 current_skeleton_color = skeleton_color; // Default color
    if (cached_ped_data.health < 50.0f) {
        current_skeleton_color = IM_COL32(255, 255, 0, 255); // Yellow for injured
    }
    if (cached_ped_data.health < 25.0f) {
        current_skeleton_color = IM_COL32(255, 100, 0, 255); // Orange for critical
    }
    if (cached_ped_data.health <= 0.0f) {
        current_skeleton_color = IM_COL32(100, 100, 100, 255); // Gray for dead
    }

    // Pre-calculate all screen positions to avoid redundant world_to_screen calls
    std::vector<Vec2> screen_positions(bone_positions.size());
    std::vector<bool> on_screen(bone_positions.size(), false);

    for (size_t i = 0; i < bone_positions.size(); ++i) {
        on_screen[i] = bone_positions[i].world_to_screen(viewport, screen_positions[i]);
    }

    // Draw skeleton connections with enhanced visuals
    bool drew_head_info = false;
    Vec2 head_screen_pos;

    for (int i = 0; i < 6; ++i) {
        int bone1_idx = bone_connections[i][0];
        int bone2_idx = bone_connections[i][1];

        if (bone1_idx < bone_positions.size() && bone2_idx < bone_positions.size() &&
            on_screen[bone1_idx] && on_screen[bone2_idx]) {

            Vec2 bone1_screen = screen_positions[bone1_idx];
            Vec2 bone2_screen = screen_positions[bone2_idx];

            // Enhanced line drawing with health-based thickness
            float current_thickness = line_thickness;
            if (cached_ped_data.health > 75.0f) {
                current_thickness *= 1.2f; // Thicker lines for healthy targets
            }
            else if (cached_ped_data.health < 25.0f) {
                current_thickness *= 0.8f; // Thinner lines for weak targets
            }

            draw_list->AddLine(
                ImVec2(bone1_screen.x, bone1_screen.y),
                ImVec2(bone2_screen.x, bone2_screen.y),
                current_skeleton_color,
                current_thickness
            );

            // Draw joint circles for better visibility
            if (cached_ped_data.health > 0.0f) {
                float joint_radius = current_thickness * 0.75f;
                draw_list->AddCircleFilled(
                    ImVec2(bone1_screen.x, bone1_screen.y),
                    joint_radius,
                    current_skeleton_color
                );
                draw_list->AddCircleFilled(
                    ImVec2(bone2_screen.x, bone2_screen.y),
                    joint_radius,
                    current_skeleton_color
                );
            }

            // Enhanced head info drawing (only once)
            if (i == 0 && bone1_idx == 0 && !drew_head_info) { // Head bone
                head_screen_pos = bone1_screen;
                drew_head_info = true;


                // Enhanced head marker for skeleton mode - distance-based size
                Vec3 local_pos = mem.Read<Vec3>(localplayer + 0x90);
                float distance = local_pos.distance_to(bone_positions[0]);

                // Calculate head circle size based on distance (closer = larger, further = smaller)
                float base_radius = 4.0f;
                float min_radius = 1.0f;
                float max_radius = 8.0f;

                // Manual clamp implementation to avoid std::clamp issues
                float distance_factor = 50.0f / distance;
                if (distance_factor < 0.2f) distance_factor = 0.2f;
                if (distance_factor > 2.0f) distance_factor = 2.0f;

                float head_radius = base_radius * distance_factor;
                if (head_radius < min_radius) head_radius = min_radius;
                if (head_radius > max_radius) head_radius = max_radius;

                float line_thickness_head = head_radius * 0.25f;
                if (line_thickness_head < 1.0f) line_thickness_head = 1.0f;

                draw_list->AddCircle(
                    ImVec2(bone1_screen.x, bone1_screen.y),
                    head_radius,
                    current_skeleton_color,
                    12,
                    line_thickness_head
                );
            }
        }
    }

    // Cleanup old cache entries periodically
    enhanced_bone_cache.cleanup_skeleton_cache();
}

bool esp::get_skeleton_bones_for_ped(uintptr_t ped, std::vector<Vec3>& bone_positions, bool force_refresh)
{
    return enhanced_bone_cache.get_skeleton_bones(ped, bone_positions, force_refresh);
}

void esp::draw_skeleton(uintptr_t ped, Matrix viewport, uintptr_t localplayer) {
    // Try to get cached data first
    PedData cached_ped_data;
    if (g_pedCacheManager.getPedData(ped, cached_ped_data)) {
        draw_skeleton_cached(ped, viewport, localplayer, cached_ped_data);
        return;
    }

    // Fallback to direct memory access with optimizations
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    static const int bone_connections[][2] = {
        { 0, 7 }, { 7, 6 }, { 7, 5 }, { 7, 8 }, { 8, 3 }, { 8, 4 }
    };

    // Get all bone positions in one batch
    std::vector<Vec3> bone_positions;
    if (!enhanced_bone_cache.get_skeleton_bones(ped, bone_positions, true)) {
        return;
    }

    // Get health directly
    float health = mem.Read<float>(ped + 0x280);

    // Pre-calculate screen positions
    std::vector<Vec2> screen_positions(bone_positions.size());
    std::vector<bool> on_screen(bone_positions.size(), false);

    for (size_t i = 0; i < bone_positions.size(); ++i) {
        on_screen[i] = bone_positions[i].world_to_screen(viewport, screen_positions[i]);
    }

    // Draw skeleton with health-based coloring
    ImU32 current_color = skeleton_color;
    if (health < 50.0f) current_color = IM_COL32(255, 255, 0, 255);
    if (health < 25.0f) current_color = IM_COL32(255, 100, 0, 255);
    if (health <= 0.0f) current_color = IM_COL32(100, 100, 100, 255);

    for (int i = 0; i < 6; ++i) {
        int bone1_idx = bone_connections[i][0];
        int bone2_idx = bone_connections[i][1];

        if (bone1_idx < bone_positions.size() && bone2_idx < bone_positions.size() &&
            on_screen[bone1_idx] && on_screen[bone2_idx]) {

            draw_list->AddLine(
                ImVec2(screen_positions[bone1_idx].x, screen_positions[bone1_idx].y),
                ImVec2(screen_positions[bone2_idx].x, screen_positions[bone2_idx].y),
                current_color, line_thickness
            );
        }
    }
}

// Keep the old enhanced health info function for backward compatibility
void esp::draw_enhanced_health_info(const Vec2& position, float health) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    // Health bar
    float health_ratio = health / 100.0f;
    if (health_ratio < 0.0f) health_ratio = 0.0f;
    if (health_ratio > 1.0f) health_ratio = 1.0f;

    float bar_width = 40.0f;
    float bar_height = 5.0f;

    // Background bar
    draw_list->AddRectFilled(
        ImVec2(position.x - bar_width / 2, position.y - 25),
        ImVec2(position.x + bar_width / 2, position.y - 25 + bar_height),
        IM_COL32(0, 0, 0, 180)
    );

    // Health bar with gradient coloring
    ImU32 health_color;
    if (health > 75.0f) {
        health_color = IM_COL32(0, 255, 0, 255);     // Green
    }
    else if (health > 50.0f) {
        health_color = IM_COL32(255, 255, 0, 255);   // Yellow
    }
    else if (health > 25.0f) {
        health_color = IM_COL32(255, 165, 0, 255);   // Orange
    }
    else {
        health_color = IM_COL32(255, 0, 0, 255);     // Red
    }

    draw_list->AddRectFilled(
        ImVec2(position.x - bar_width / 2, position.y - 25),
        ImVec2(position.x - bar_width / 2 + bar_width * health_ratio, position.y - 25 + bar_height),
        health_color
    );

    // Health text
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%.0f", health);
    draw_list->AddText(
        ImVec2(position.x - 10, position.y - 18),
        IM_COL32(255, 255, 255, 255),
        buffer
    );
}

// Utility functions
void esp::draw_health_info(const Vec2& position, float health) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%.1f", health);

    // Color based on health
    ImU32 text_color = IM_COL32(255, 255, 255, 255); // White
    if (health < 50.0f) text_color = IM_COL32(255, 255, 0, 255); // Yellow
    if (health < 25.0f) text_color = IM_COL32(255, 0, 0, 255); // Red

    draw_list->AddText(ImVec2(position.x, position.y - 20), text_color, buffer);
}

// Your existing functions (keeping them exactly as they are)
Vec3 esp::get_bone_position(uintptr_t ped, int bone_position)
{
    Matrix bone_matrix = mem.Read<Matrix>(ped + 0x60);
    Vector3 Head = mem.Read<Vector3>(ped + (0x410 + 0x10 * bone_position));
    DirectX::SimpleMath::Vector3 boneVec(Head.x, Head.y, Head.z);
    DirectX::SimpleMath::Vector3 transformedBoneVec = DirectX::XMVector3Transform(boneVec, bone_matrix);
    return Vec3(transformedBoneVec.x, transformedBoneVec.y, transformedBoneVec.z);
}

Vec3 esp::find_closest_player(Vec3& localPlayerPosition, Matrix viewmatrix) {
    float minDistance = 1000.0f;
    Vec3 closestPedPosition = Vec3(1.0f, 1.0f, 1.0f);

    // Get all valid ped IDs
    std::vector<uintptr_t> validPedIds = g_pedCacheManager.getValidPedIds();

    for (uintptr_t pedPointer : validPedIds) {
        PedData data;
        if (g_pedCacheManager.getPedData(pedPointer, data)) {
            float dx = localPlayerPosition.x - data.position_origin.x;
            float dy = localPlayerPosition.y - data.position_origin.y;
            float distance = std::sqrt(dx * dx + dy * dy);

            if (distance < minDistance) {
                minDistance = distance;
                closestPedPosition = data.position_origin;
            }
        }
    }

    return closestPedPosition;
}

// Configuration functions
void esp::set_circle_color(ImU32 color) { circle_color = color; }
void esp::set_skeleton_color(ImU32 color) { skeleton_color = color; }
void esp::set_line_thickness(float thickness) { line_thickness = thickness; }
void esp::set_use_batch_skeleton(bool use_batch) { use_batch_skeleton = use_batch; }

ImU32 esp::get_circle_color() { return circle_color; }
ImU32 esp::get_skeleton_color() { return skeleton_color; }
float esp::get_line_thickness() { return line_thickness; }
bool esp::get_use_batch_skeleton() { return use_batch_skeleton; }

// Batch update and other functions (keeping your existing implementations)
void esp::batch_update_bone_cache(const std::vector<uintptr_t>& peds) {
    if (peds.empty()) return;

    auto handle = mem.CreateScatterHandle();
    std::vector<Matrix> bone_matrices(peds.size());
    std::vector<Vector3> head_offsets(peds.size());

    for (size_t i = 0; i < peds.size(); ++i) {
        mem.AddScatterReadRequest(handle, peds[i] + BONE_MATRIX_OFFSET,
            &bone_matrices[i], sizeof(Matrix));
        mem.AddScatterReadRequest(handle, peds[i] + (BONE_ARRAY_BASE + BONE_SIZE * 0),
            &head_offsets[i], sizeof(Vector3));
    }

    mem.ExecuteReadScatter(handle);
    mem.CloseScatterHandle(handle);

    for (size_t i = 0; i < peds.size(); ++i) {
        DirectX::SimpleMath::Vector3 boneVec(head_offsets[i].x, head_offsets[i].y, head_offsets[i].z);
        DirectX::SimpleMath::Vector3 transformedBoneVec = DirectX::XMVector3Transform(boneVec, bone_matrices[i]);
        Vec3 head_pos(transformedBoneVec.x, transformedBoneVec.y, transformedBoneVec.z);

        bone_cache.update_bone_data(peds[i], bone_matrices[i], head_pos);
    }

    esp_stats.memory_reads += peds.size() * 2;
}

// Batch skeleton update for multiple peds (performance optimization)
void esp::batch_update_skeleton_cache(const std::vector<uintptr_t>& peds) {
    if (peds.empty()) return;

    // Batch read all skeleton data
    for (uintptr_t ped : peds) {
        std::vector<Vec3> bone_positions;
        enhanced_bone_cache.get_skeleton_bones(ped, bone_positions, false); // Use cache when possible
    }
}

Vec3 esp::get_bone_position_cached(uintptr_t ped, int bone_position, const PedData& cached_ped_data) {
    if (bone_position == 0 && bone_cache.is_data_valid(ped)) {
        return bone_cache.get_bone_position(ped, 0, false);
    }
    return bone_cache.get_bone_position(ped, bone_position, true);
}

void esp::print_esp_stats() {
    static auto last_print = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();

    if (now - last_print >= std::chrono::seconds(5)) {
        double hit_ratio = esp_stats.get_hit_ratio();
        size_t cache_size = bone_cache.get_cache_size();
        size_t skeleton_cache_size = enhanced_bone_cache.get_skeleton_cache_size();

        std::cout << "[ESP Cache] Hit Ratio: " << (hit_ratio * 100.0) << "%"
            << ", Head Cache: " << cache_size
            << ", Skeleton Cache: " << skeleton_cache_size
            << ", Memory Reads: " << esp_stats.memory_reads
            << ", Batch Reads: " << esp_stats.batch_reads << std::endl;

        esp_stats.reset();
        last_print = now;
    }
}

void esp::DrawDebugLine(const Vec2& from, const Vec2& to, ImU32 color) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    draw_list->AddLine(ImVec2(from.x, from.y), ImVec2(to.x, to.y), color, 1.0f);
}

void esp::DrawDebugText(const Vec2& pos, const char* text, ImU32 color) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    draw_list->AddText(ImVec2(pos.x, pos.y), color, text);
}

void esp::DrawDebugCircle(const Vec2& center, float radius, ImU32 color) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
    draw_list->AddCircle(ImVec2(center.x, center.y), radius, color, 12, 1.0f);
}

// Cache management functions
size_t esp::get_skeleton_cache_size() {
    return enhanced_bone_cache.get_skeleton_cache_size();
}

void esp::clear_skeleton_cache() {
    enhanced_bone_cache.clear_skeleton_cache();
}

void esp::set_use_cache(bool use) { use_cache = use; }
bool esp::get_use_cache() { return use_cache; }

