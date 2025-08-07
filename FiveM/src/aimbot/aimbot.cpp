```c++ name=FiveM/src/aimbot/aimbot.cpp
#include "../math/math.h"
#include "../playerInfo/PedData.h"
#include "../game/offsets.h"
#include "../game/visibility.h"
#include "../DMALibrary/Memory/Memory.h"
#include "../globals.h"
#include <vector>
#include <algorithm>
#include <cmath>

// ---- FiveM DMA Aimbot ----
// 必要なロジックのみ。mainループなどから呼び出し可能。
// 例: aimbot::RunAimbot();

namespace aimbot {

    // 設定値
    constexpr float AIMBOT_FOV = 30.0f;        // 画面上FOV（度）
    constexpr float AIMBOT_SMOOTH = 0.4f;      // スムージング係数（0.0=瞬時, 1.0=何もしない）

    // ユーザーの視点座標取得
    bool GetLocalPlayerView(Vec3& outView) {
        uintptr_t cameraPtr = FiveM::offset::base + FiveM::offset::camera;
        if (!mem.Read(cameraPtr + 0x220, &outView, sizeof(Vec3))) { // 例: カメラ座標オフセット（要調整）
            return false;
        }
        return true;
    }

    // ユーザーのペド（Ped）座標取得
    bool GetLocalPlayerPosition(Vec3& outPos) {
        uintptr_t pedPtr = FiveM::offset::localplayer;
        if (!mem.Read(pedPtr + FiveM::offset::playerPosition, &outPos, sizeof(Vec3))) {
            return false;
        }
        return true;
    }

    // 骨座標（頭など）を取得
    bool GetPedBonePosition(uintptr_t ped, int boneId, Vec3& outPos) {
        uintptr_t bonePtr = ped + FiveM::offset::boneList; // boneListはPedからのオフセット
        uintptr_t boneMatrixPtr = bonePtr + boneId * FiveM::offset::boneMatrix;
        return mem.Read(boneMatrixPtr, &outPos, sizeof(Vec3));
    }

    // 視点調整
    void SetViewAngles(const Vec3& targetAngles) {
        // 実際のカメラ角度へのWrite
        uintptr_t cameraPtr = FiveM::offset::base + FiveM::offset::camera;
        mem.Write(cameraPtr + 0x250, (void*)&targetAngles.x, sizeof(float)); // pitch
        mem.Write(cameraPtr + 0x254, (void*)&targetAngles.y, sizeof(float)); // yaw
        // FiveMの構造体によって要調整
    }

    // エイム角度計算（自分の位置→ターゲットの位置）
    Vec3 CalculateAimAngles(const Vec3& from, const Vec3& to) {
        Vec3 delta = to - from;
        float hyp = sqrtf(delta.x * delta.x + delta.y * delta.y);
        float pitch = -atan2f(delta.z, hyp) * (180.0f / std::numbers::pi_v<float>);
        float yaw = atan2f(delta.y, delta.x) * (180.0f / std::numbers::pi_v<float>);
        return Vec3(pitch, yaw, 0.0f);
    }

    // FOV判定
    float CalcAngleDistance(const Vec3& a, const Vec3& b) {
        return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y));
    }

    // ターゲット探索（最も近い敵PedをFOV内から選択）
    uintptr_t FindBestTarget(const Vec3& myPos, const Vec3& myAngles, const std::vector<uintptr_t>& validPeds) {
        float bestFov = AIMBOT_FOV;
        uintptr_t bestTarget = 0;

        for (uintptr_t ped : validPeds) {
            PedData data;
            if (!g_pedCacheManager.getPedData(ped, data)) continue;
            if (data.health <= 0.0f) continue; // 死亡判定
            if (!FiveM::Visibility::IsPedVisible(ped)) continue; // 可視判定

            // 頭の骨座標取得（例: boneId=8は頭。FiveMのBoneIDによる）
            Vec3 targetHead;
            if (!GetPedBonePosition(ped, 8, targetHead)) continue;

            // 自分視点からターゲット頭へのエイム角度計算
            Vec3 aimAngles = CalculateAimAngles(myPos, targetHead);
            float fov = CalcAngleDistance(myAngles, aimAngles);

            if (fov < bestFov) {
                bestFov = fov;
                bestTarget = ped;
            }
        }
        return bestTarget;
    }

    // スムージング
    Vec3 SmoothAngles(const Vec3& current, const Vec3& target, float smooth) {
        return current + (target - current) * (1.0f - smooth);
    }

    // メイン処理（外部から毎フレーム呼び出し）
    void RunAimbot() {
        // 自分の座標・視点取得
        Vec3 myPos, myAngles;
        if (!GetLocalPlayerPosition(myPos)) return;
        if (!GetLocalPlayerView(myAngles)) return;

        // 有効なPedリスト取得
        std::vector<uintptr_t> validPeds = g_pedCacheManager.getValidPedIds();

        // ターゲット取得
        uintptr_t targetPed = FindBestTarget(myPos, myAngles, validPeds);
        if (!targetPed) return;

        // ターゲットの頭座標取得
        Vec3 targetHead;
        if (!GetPedBonePosition(targetPed, 8, targetHead)) return;

        // エイム角度計算
        Vec3 targetAngles = CalculateAimAngles(myPos, targetHead);

        // スムージング
        Vec3 finalAngles = SmoothAngles(myAngles, targetAngles, AIMBOT_SMOOTH);

        // カメラ角度書き込み
        SetViewAngles(finalAngles);
    }

} // namespace aimbot

// 使い方例（main.cpp等で）:
// while (overlay.shouldRun) {
//     ...
//     aimbot::RunAimbot();
//     ...
// }
```
