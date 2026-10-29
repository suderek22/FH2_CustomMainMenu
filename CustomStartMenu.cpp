// CustomStartMenu: replaces the main-menu music of Forza Horizon 2.
//
// The menu song (HZ2_PressStartFINAL_01, 13976064 samples) is started by the game's
// own FMOD Studio event, which ends up calling FMOD Core System::playSound. This mod
// hooks that call and, when it sees the menu sound, hands FMOD a different sound
// instead. Start, loop, volume and fades stay in the game's hands.
#include "fh2mod.h"

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace {

namespace fs = std::filesystem;

constexpr std::uintptr_t kPlaySound = 0x199B0F0;
constexpr std::uintptr_t kCreateSound = 0x1997740;
constexpr std::uintptr_t kSoundGetLength = 0x199E5A0;
constexpr std::uintptr_t kSetLoopPoints = 0x19A0960;

constexpr unsigned kMenuMusicSamples = 13976064;
constexpr unsigned kModeLoop = 0x2;
constexpr unsigned kMode2D = 0x8;
constexpr unsigned kModeCreateSample = 0x100;
constexpr unsigned kModeOpenMemory = 0x800;
constexpr unsigned kExInfoSize = 0xD8;
constexpr int kRate = 44100;

constexpr char kSettingKey[] = "MenuMusic";
constexpr wchar_t kIniName[] = L"CustomStartMenu.ini";
constexpr wchar_t kIniSection[] = L"CustomStartMenu";
constexpr wchar_t kBankName[] = L"CustomStartMenu.bank";

using PlaySoundFn = int(__fastcall*)(void*, void*, void*, bool, void**);
using CreateSoundFn = int(__fastcall*)(void*, const void*, unsigned, void*, void**);
using GetLengthFn = int(__fastcall*)(void*, unsigned*, unsigned);
using LoopPointsFn = int(__fastcall*)(void*, unsigned, unsigned, unsigned, unsigned);

struct Track {
    std::string name;
    std::uint64_t offset = 0; // bank tracks: byte offset of the PCM16 data
    std::uint64_t frames = 0;
    std::vector<char> wav;    // ready-to-play in-memory WAV (fade and gain applied)
    unsigned fade_frames = 0;
    unsigned total_frames = 0;
    bool custom = false;
    void* sound = nullptr;
    bool ready = false;
};

const FH2ModApi* g_api = nullptr;
FH2Mod* g_mod = nullptr;
std::uint8_t* g_base = nullptr;
PlaySoundFn g_original_play = nullptr;
fs::path g_dir, g_ini, g_bank, g_custom_dir;

// Selector layout: 0 = Default, 1..N = bank tracks, N+1 = Custom.
std::vector<Track> g_tracks;
std::vector<std::string> g_choices;
std::mutex g_lock;
std::atomic<int> g_selected{0};
float g_gain = 1.0f;
int g_fade_ms = 600;
std::atomic<bool> g_logged_swap{false};

void logf(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (g_api && g_mod) g_api->log(g_mod, buf);
}

fs::path module_directory() {
    std::wstring p(32768, L'\0');
    const DWORD n = GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase),
                                       p.data(), static_cast<DWORD>(p.size()));
    if (n == 0 || n >= p.size()) return {};
    p.resize(n);
    return fs::path(p).parent_path();
}

template <typename Fn>
Fn fn(std::uintptr_t rva) {
    return reinterpret_cast<Fn>(g_base + rva);
}

// ---------------------------------------------------------------- audio data

// FSB5 bank, mode 2 (PCM16), stereo 44.1 kHz; names come from the name table.
bool read_bank_index(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char hdr[60];
    in.read(hdr, sizeof(hdr));
    if (!in || std::memcmp(hdr, "FSB5", 4) != 0) return false;
    std::uint32_t h[6];
    std::memcpy(h, hdr + 4, sizeof(h));
    const std::uint32_t n = h[1], shs = h[2], nts = h[3], mode = h[5];
    if (mode != 2 || n == 0 || n > 64) return false;
    std::vector<char> sh(shs), nt(nts);
    in.read(sh.data(), shs);
    in.read(nt.data(), nts);
    if (!in) return false;
    const std::uint64_t data_start = 60ull + shs + nts;
    std::size_t p = 0;
    for (std::uint32_t i = 0; i < n; ++i) {
        std::uint64_t v;
        if (p + 8 > sh.size()) return false;
        std::memcpy(&v, sh.data() + p, 8);
        p += 8;
        std::uint32_t more = v & 1;
        while (more) {
            std::uint32_t c;
            if (p + 4 > sh.size()) return false;
            std::memcpy(&c, sh.data() + p, 4);
            p += 4;
            more = c & 1;
            p += (c >> 1) & 0xFFFFFF;
        }
        Track t;
        t.offset = data_start + ((v >> 7) & 0x7FFFFFF) * 32;
        t.frames = v >> 34;
        std::uint32_t no;
        if (i * 4 + 4 > nt.size()) return false;
        std::memcpy(&no, nt.data() + i * 4, 4);
        if (no >= nt.size()) return false;
        t.name.assign(nt.data() + no, strnlen(nt.data() + no, nt.size() - no));
        g_tracks.push_back(std::move(t));
    }
    return true;
}

bool read_bank_pcm(const Track& t, std::vector<std::int16_t>& pcm) {
    std::ifstream in(g_bank, std::ios::binary);
    if (!in) return false;
    pcm.resize(t.frames * 2);
    in.seekg(static_cast<std::streamoff>(t.offset));
    in.read(reinterpret_cast<char*>(pcm.data()), pcm.size() * 2);
    return static_cast<bool>(in);
}

// Windows Media Foundation decodes mp3 and wav (any rate/channels) to PCM16 stereo.
bool decode_media(const fs::path& path, std::vector<std::int16_t>& out) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ok = false;
    if (SUCCEEDED(MFStartup(MF_VERSION))) {
        IMFSourceReader* rd = nullptr;
        if (SUCCEEDED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &rd))) {
            IMFMediaType* t = nullptr;
            MFCreateMediaType(&t);
            t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
            t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
            rd->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
            rd->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
            if (SUCCEEDED(rd->SetCurrentMediaType(
                    MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t))) {
                for (;;) {
                    DWORD flags = 0;
                    IMFSample* s = nullptr;
                    if (FAILED(rd->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                              0, nullptr, &flags, nullptr, &s)))
                        break;
                    if (s) {
                        IMFMediaBuffer* b = nullptr;
                        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
                            BYTE* data = nullptr;
                            DWORD len = 0;
                            if (SUCCEEDED(b->Lock(&data, nullptr, &len))) {
                                const std::size_t old = out.size();
                                out.resize(old + len / 2);
                                std::memcpy(out.data() + old, data, len / 4 * 4);
                                b->Unlock();
                            }
                            b->Release();
                        }
                        s->Release();
                    }
                    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                        ok = !out.empty();
                        break;
                    }
                }
            }
            t->Release();
            rd->Release();
        }
        MFShutdown();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return ok;
}

fs::path find_custom_file() {
    std::error_code ec;
    std::vector<fs::path> found;
    for (const auto& e : fs::directory_iterator(g_custom_dir, ec)) {
        if (!e.is_regular_file()) continue;
        std::wstring ext = e.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext == L".wav" || ext == L".mp3") found.push_back(e.path());
    }
    std::sort(found.begin(), found.end());
    return found.empty() ? fs::path() : found.front();
}

// Applies gain and a fade-in to the PCM and wraps it as an in-memory WAV.
void finish_track(Track& t, std::vector<std::int16_t>& pcm) {
    const std::size_t frames = pcm.size() / 2;
    const std::size_t fade =
        std::min<std::size_t>(frames, static_cast<std::size_t>(kRate) * g_fade_ms / 1000);
    for (std::size_t i = 0; i < frames; ++i) {
        float g = g_gain;
        if (i < fade) {
            const float x = static_cast<float>(i) / static_cast<float>(fade);
            g *= x * x * (3.0f - 2.0f * x); // smoothstep
        }
        if (g == 1.0f) continue;
        for (int c = 0; c < 2; ++c) {
            const float v = pcm[i * 2 + c] * g;
            pcm[i * 2 + c] = static_cast<std::int16_t>(
                std::max(-32768.0f, std::min(32767.0f, v)));
        }
    }
    const std::uint32_t bytes = static_cast<std::uint32_t>(pcm.size() * 2);
    const std::uint32_t riff = 36 + bytes, fmt_size = 16, rate = kRate, byte_rate = kRate * 4;
    const std::uint16_t pcm_tag = 1, ch = 2, align = 4, bits = 16;
    std::vector<char> w(44 + bytes);
    char* q = w.data();
    std::memcpy(q, "RIFF", 4);
    std::memcpy(q + 4, &riff, 4);
    std::memcpy(q + 8, "WAVEfmt ", 8);
    std::memcpy(q + 16, &fmt_size, 4);
    std::memcpy(q + 20, &pcm_tag, 2);
    std::memcpy(q + 22, &ch, 2);
    std::memcpy(q + 24, &rate, 4);
    std::memcpy(q + 28, &byte_rate, 4);
    std::memcpy(q + 32, &align, 2);
    std::memcpy(q + 34, &bits, 2);
    std::memcpy(q + 36, "data", 4);
    std::memcpy(q + 40, &bytes, 4);
    std::memcpy(q + 44, pcm.data(), bytes);
    std::lock_guard<std::mutex> lock(g_lock);
    t.wav = std::move(w);
    t.fade_frames = static_cast<unsigned>(fade);
    t.total_frames = static_cast<unsigned>(frames);
    t.sound = nullptr; // a previously created sound (custom reload) is simply left alone
    t.ready = true;
}

void load_selected(int index) {
    if (index <= 0 || index > static_cast<int>(g_tracks.size())) return;
    Track& t = g_tracks[index - 1];
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (t.ready && !t.custom) return;
        t.ready = false;
    }
    std::vector<std::int16_t> pcm;
    bool ok = false;
    if (t.custom) {
        const fs::path file = find_custom_file();
        if (file.empty()) {
            logf("Custom: no .wav or .mp3 found in %s; the default music will play.",
                 g_custom_dir.string().c_str());
            return;
        }
        ok = decode_media(file, pcm);
        if (!ok) logf("Custom: could not decode %s.", file.string().c_str());
        else logf("Custom: loaded %s (%.1f s).", file.string().c_str(),
                  static_cast<double>(pcm.size() / 2) / kRate);
    } else {
        ok = read_bank_pcm(t, pcm);
        if (!ok) logf("Could not read track '%s' from the bank.", t.name.c_str());
    }
    if (!ok) return;
    finish_track(t, pcm);
    logf("Track '%s' is ready.", t.name.c_str());
}

bool create_sound(void* core, Track& t) {
    __try {
        unsigned char info[kExInfoSize] = {};
        *reinterpret_cast<unsigned*>(info) = kExInfoSize;
        *reinterpret_cast<unsigned*>(info + 4) = static_cast<unsigned>(t.wav.size());
        void* sound = nullptr;
        const int r = fn<CreateSoundFn>(kCreateSound)(
            core, t.wav.data(),
            kModeOpenMemory | kModeCreateSample | kMode2D | kModeLoop, info, &sound);
        if (r != 0 || !sound) return false;
        // Loop back past the fade-in so it is not repeated on every pass.
        if (t.total_frames > t.fade_frames + 1)
            fn<LoopPointsFn>(kSetLoopPoints)(sound, t.fade_frames, 2,
                                             t.total_frames - 1, 2);
        t.sound = sound;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool is_menu_sound(void* sound) {
    __try {
        unsigned pcm = 0;
        return fn<GetLengthFn>(kSoundGetLength)(sound, &pcm, 2) == 0 &&
               pcm == kMenuMusicSamples;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* replacement(void* core) {
    const int sel = g_selected.load();
    if (sel <= 0 || sel > static_cast<int>(g_tracks.size())) return nullptr;
    Track& t = g_tracks[sel - 1];
    std::lock_guard<std::mutex> lock(g_lock);
    if (!t.ready) return nullptr;
    if (!t.sound && !create_sound(core, t)) {
        logf("createSound failed for '%s'.", t.name.c_str());
        t.ready = false;
        return nullptr;
    }
    return t.sound;
}

int __fastcall detour_play(void* system, void* sound, void* group, bool paused,
                           void** channel) {
    void* use = sound;
    if (sound && g_selected.load() > 0 && is_menu_sound(sound)) {
        if (void* mine = replacement(system)) {
            use = mine;
            logf("Main-menu music replaced with '%s'.",
                 g_tracks[g_selected.load() - 1].name.c_str());
        } else {
            logf("Selected music is not ready; the default music plays.");
        }
    }
    return g_original_play(system, use, group, paused, channel);
}

void install_hook() {
    if (!g_api->wait_for_code || !g_api->hook_function || !g_api->enable_hooks) {
        logf("The platform lacks the hook API; the mod is inactive.");
        return;
    }
    void* base = g_api->wait_for_code(120000);
    if (!base) {
        logf("Timed out waiting for the game code.");
        return;
    }
    g_base = static_cast<std::uint8_t*>(base);
    FH2Hook* h = g_api->hook_function(
        g_mod, "CustomStartMenu: FMOD System::playSound", g_base + kPlaySound,
        reinterpret_cast<void*>(detour_play),
        reinterpret_cast<void**>(&g_original_play));
    if (!h || !g_api->enable_hooks(g_mod, h))
        logf("Could not hook System::playSound; the mod is inactive.");
    else
        logf("playSound hook active.");
}

void save_selected(const std::string& name) {
    WritePrivateProfileStringW(kIniSection, L"Selected",
                               fs::path(name).wstring().c_str(), g_ini.c_str());
}

void on_choice_changed(void*, const char* key, int value) {
    if (!key || std::strcmp(key, kSettingKey) != 0 || value < 0 ||
        value >= static_cast<int>(g_choices.size())) {
        logf("Ignored invalid MenuMusic setting.");
        return;
    }
    g_selected.store(value);
    save_selected(g_choices[value]);
    logf("Selected: %s (applies the next time the menu music starts).",
         g_choices[value].c_str());
    if (value > 0) std::thread(load_selected, value).detach();
}

} // namespace

FH2MOD_EXPORT int fh2mod_init(const FH2ModApi* api) {
    if (!api || api->api_version < 3 || !api->register_mod || !api->log) return 1;
    FH2ModInfo info{};
    info.size = sizeof(info);
    info.name = "CustomStartMenu";
    info.version = "0.1.2";
    info.github_repo = "suderek22/FH2_CustomMainMenu";
    info.platform_version = "2.8.0";
    info.check_updates = 1;
    info.description = "Choose the main-menu music: Default, bundled tracks or your own file.";
    g_api = api;
    g_mod = api->register_mod(&info);
    if (!g_mod) return 1;

    g_dir = module_directory();
    if (g_dir.empty()) return 1;
    g_ini = g_dir / kIniName;
    g_bank = g_dir / L"CustomStartMenu" / kBankName;
    g_custom_dir = g_dir / L"CustomStartMenu" / L"Custom";
    std::error_code ec;
    fs::create_directories(g_custom_dir, ec);

    wchar_t buf[256] = {};
    GetPrivateProfileStringW(kIniSection, L"Gain", L"", buf, 32, g_ini.c_str());
    if (buf[0]) {
        g_gain = static_cast<float>(_wtof(buf));
        if (!(g_gain >= 0.0f && g_gain <= 4.0f)) g_gain = 1.0f;
    } else {
        WritePrivateProfileStringW(kIniSection, L"Gain", L"1.0", g_ini.c_str());
    }
    g_fade_ms = GetPrivateProfileIntW(kIniSection, L"FadeMs", -1, g_ini.c_str());
    if (g_fade_ms < 0) {
        g_fade_ms = 600;
        WritePrivateProfileStringW(kIniSection, L"FadeMs", L"600", g_ini.c_str());
    }
    g_fade_ms = std::min(g_fade_ms, 5000);
    GetPrivateProfileStringW(kIniSection, L"Selected", L"Default", buf, 256, g_ini.c_str());
    std::string wanted = fs::path(buf).string();
    if (wanted == "Levels") wanted = "Language"; // name used before 0.1.2

    if (!read_bank_index(g_bank))
        logf("Bank %s not found or invalid; only Default and Custom are available.",
             g_bank.string().c_str());
    Track custom;
    custom.name = "Custom";
    custom.custom = true;
    g_tracks.push_back(std::move(custom));
    g_choices.push_back("Default");
    for (const Track& t : g_tracks) g_choices.push_back(t.name);

    int selected = 0;
    for (std::size_t i = 0; i < g_choices.size(); ++i)
        if (g_choices[i] == wanted) selected = static_cast<int>(i);
    g_selected.store(selected);
    logf("Selected at start: %s.", g_choices[selected].c_str());
    if (selected > 0) std::thread(load_selected, selected).detach();

    if (api->api_version >= 9 && api->add_setting) {
        std::vector<const char*> names;
        for (const auto& c : g_choices) names.push_back(c.c_str());
        FH2Setting setting{};
        setting.size = sizeof(setting);
        setting.key = kSettingKey;
        setting.label = "Main menu music";
        setting.description =
            "Default, bundled tracks or Custom (first .wav/.mp3 in "
            "fh2-mods\\CustomStartMenu\\Custom). Applies when the menu music next starts.";
        setting.kind = FH2_SETTING_CHOICE;
        setting.value = selected;
        setting.choices = names.data();
        setting.choice_count = static_cast<uint32_t>(names.size());
        setting.on_change = on_choice_changed;
        if (!api->add_setting(g_mod, &setting))
            logf("Could not add the selector to the Mods menu.");
    } else {
        logf("Platform 2.8.0 is needed for the Mods-menu selector; using CustomStartMenu.ini.");
    }

    std::thread(install_hook).detach();
    return 0;
}