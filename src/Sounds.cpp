#include "Sounds.h"

#include <algorithm>
#include <filesystem>
#include <mutex>
#include <random>
#include <unordered_map>
#include <vector>

namespace Sounds {
    namespace {
        struct Kind { std::vector<std::string> files; int last = -1; bool looked = false; };
        std::mutex g_mx;
        std::unordered_map<std::string, Kind> g_kinds;
        std::mt19937 g_rng{std::random_device{}()};

        Kind& Look(const std::string& kind) {
            Kind& k = g_kinds[kind];
            if (k.looked) return k;
            k.looked = true;
            std::error_code ec;
            const std::filesystem::path dir = std::filesystem::path("Data/Sound/Fx/Anatomica") / kind;
            for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string ext = it->path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (ext == ".wav" || ext == ".xwm") k.files.push_back("Sound\\Fx\\Anatomica\\" + kind + "\\" + it->path().filename().string());
            }
            std::sort(k.files.begin(), k.files.end());
            logger::info("Anatomica sounds: {} file(s) for \"{}\"", k.files.size(), kind);
            return k;
        }
    }

    int Count(const char* kind) {
        std::lock_guard lk(g_mx);
        return static_cast<int>(Look(kind).files.size());
    }

    bool Play(const char* kind, RE::Actor* a, float volume, const char* node) {
        if (!a || volume <= 0.0f) return false;
        auto* am = RE::BSAudioManager::GetSingleton();
        if (!am) return false;
        std::string file;
        {
            std::lock_guard lk(g_mx);
            Kind& k = Look(kind);
            if (k.files.empty()) return false;
            int i = std::uniform_int_distribution<int>(0, static_cast<int>(k.files.size()) - 1)(g_rng);
            if (k.files.size() > 1 && i == k.last) i = (i + 1) % static_cast<int>(k.files.size());
            k.last = i;
            file = k.files[static_cast<size_t>(i)];
        }
        return PlayFile(file.c_str(), a, volume, node);
    }

    bool PlayFile(const char* file, RE::Actor* a, float volume, const char* node) {
        auto* am = RE::BSAudioManager::GetSingleton();
        if (!a || !am || !file || volume <= 0.0f) return false;
        RE::BSResource::ID id;
        id.GenerateFromPath(file);
        RE::BSSoundHandle h;
        am->GetSoundHandleByFile(h, id, 0x1A, 0);
        if (!h.IsValid()) return false;
        auto* root = a->Get3D(false);
        auto* at = root ? root->GetObjectByName(RE::BSFixedString(node)) : nullptr;
        if (at) h.SetObjectToFollow(at);
        else h.SetPosition(a->GetPosition());
        h.SetVolume(std::clamp(volume, 0.0f, 2.0f));
        h.SetFrequency(std::uniform_real_distribution<float>(0.93f, 1.07f)(g_rng));
        return h.Play();
    }
}
