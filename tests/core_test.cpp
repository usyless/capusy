#include "check.hpp"

#include <capusy/audio_capture.hpp>
#include <capusy/capture_logic.hpp>
#include <capusy/color_logic.hpp>
#include <capusy/history_store.hpp>
#include <capusy/hotkeys.hpp>
#include <capusy/recording_logic.hpp>
#include <capusy/startup_logic.hpp>
#include <capusy/update_logic.hpp>
#include <capusy/ui/anim.hpp>
#include <capusy/ui/dirty_rects.hpp>
#include <capusy/ui/fit.hpp>
#include <capusy/ui/flow.hpp>

#include <array>
#include <filesystem>

namespace fs = std::filesystem;

static void test_startup_logic() {
    using namespace capusy;
    const std::array<uint8_t, 12> enabled_bytes = {0x02, 0x00, 0x00, 0x00};
    const std::array<uint8_t, 12> disabled_bytes = {0x03, 0x00, 0x00, 0x00};
    CHECK(is_startup_approved_bytes(enabled_bytes));
    CHECK(!is_startup_approved_bytes(disabled_bytes));
    CHECK(is_startup_approved_bytes({}));

    const auto q = extract_startup_command_exe(L"\"C:\\Program Files\\capusy\\capusy.exe\" --minimized");
    CHECK(q.primary == L"C:\\Program Files\\capusy\\capusy.exe");
    CHECK(q.fallback.empty());

    const auto u = extract_startup_command_exe(L"C:\\capusy\\capusy.exe --tray");
    CHECK(u.primary == L"C:\\capusy\\capusy.exe --tray");
    CHECK(u.fallback == L"C:\\capusy\\capusy.exe");

    const auto bad = extract_startup_command_exe(L"\"C:\\unclosed.exe");
    CHECK(bad.primary.empty());
}

static void test_hotkey_collisions() {
    using namespace capusy;
    std::array<HotkeyBinding, HOTKEY_COUNT> bindings = {{
        {.enabled = true, .mods = MOD_ALT | MOD_SHIFT, .vk = '1'},
        {.enabled = true, .mods = MOD_ALT | MOD_SHIFT, .vk = '2'},
        {.enabled = true, .mods = MOD_ALT | MOD_SHIFT, .vk = '3'},
        {.enabled = true, .mods = MOD_ALT | MOD_SHIFT, .vk = 'H'},
        {.enabled = true, .mods = MOD_ALT | MOD_SHIFT, .vk = 'C'},
        {.enabled = true, .mods = MOD_ALT | MOD_SHIFT, .vk = '4'},
    }};

    for (size_t i = 0; i < HOTKEY_COUNT; ++i) {
        CHECK(!find_internal_hotkey_conflict(bindings, i).has_value());
    }

    // Candidate colliding with action 1
    const auto c1 = check_candidate_hotkey_conflict(bindings, 0, MOD_ALT | MOD_SHIFT, '2');
    CHECK(c1.has_value() && *c1 == HotkeyAction::RegionScreenshot);

    // Same key on self is NOT a conflict
    CHECK(!check_candidate_hotkey_conflict(bindings, 0, MOD_ALT | MOD_SHIFT, '1').has_value());

    // Duplicate enabled bindings detected
    bindings[2] = bindings[0];
    const auto c2 = find_internal_hotkey_conflict(bindings, 2);
    CHECK(c2.has_value() && *c2 == HotkeyAction::FullScreenshot);

    // Disabled binding does not collide
    bindings[0].enabled = false;
    CHECK(!find_internal_hotkey_conflict(bindings, 2).has_value());

    // Standalone keys
    CHECK(is_standalone_hotkey_vk(VK_SNAPSHOT));
    CHECK(is_standalone_hotkey_vk(VK_F9));
    CHECK(!is_standalone_hotkey_vk('A'));

    // PrintScreen standalone & modified keybind logic
    CHECK(resolve_recorded_hotkey_mods(0, VK_SNAPSHOT) == 0u);
    CHECK(resolve_recorded_hotkey_mods(MOD_CONTROL, VK_SNAPSHOT) == MOD_CONTROL);
    CHECK(resolve_recorded_hotkey_mods(MOD_SHIFT | MOD_ALT, VK_SNAPSHOT) == (MOD_SHIFT | MOD_ALT));
    CHECK(resolve_recorded_hotkey_mods(MOD_WIN, VK_SNAPSHOT) == MOD_WIN);
    CHECK(resolve_recorded_hotkey_mods(0, 'A') == MOD_ALT);
    CHECK(resolve_recorded_hotkey_mods(MOD_CONTROL, 'A') == MOD_CONTROL);

    CHECK(is_modifier_vk(VK_SHIFT));
    CHECK(is_modifier_vk(VK_LSHIFT));
    CHECK(is_modifier_vk(VK_RCONTROL));
    CHECK(is_modifier_vk(VK_RMENU));
    CHECK(is_modifier_vk(VK_LWIN));
    CHECK(!is_modifier_vk(VK_SNAPSHOT));

    CHECK(format_hotkey(0, VK_SNAPSHOT) == L"PrintScreen");
    CHECK(format_hotkey(MOD_CONTROL | MOD_SHIFT, VK_SNAPSHOT) == L"Ctrl + Shift + PrintScreen");
    CHECK(format_hotkey(MOD_ALT, VK_SNAPSHOT, true) == L"Alt+PrintScreen");
    CHECK(format_hotkey(MOD_WIN, VK_SNAPSHOT, true) == L"Win+PrintScreen");

    // Distinct PrintScreen combinations do not collide with each other
    bindings[0] = {.enabled = true, .mods = 0, .vk = VK_SNAPSHOT};
    bindings[1] = {.enabled = true, .mods = MOD_SHIFT, .vk = VK_SNAPSHOT};
    CHECK(!find_internal_hotkey_conflict(bindings, 0).has_value());
    CHECK(!find_internal_hotkey_conflict(bindings, 1).has_value());
    CHECK(check_candidate_hotkey_conflict(bindings, 2, MOD_SHIFT, VK_SNAPSHOT) == HotkeyAction::RegionScreenshot);
    CHECK(any_printscreen_hotkey(bindings));

    // Windows PrintScreen registry evaluation & fallback policy
    CHECK(is_windows_printscreen_disabled_value(0u));
    CHECK(!is_windows_printscreen_disabled_value(1u));
    CHECK(!is_windows_printscreen_disabled_value(std::nullopt));
    CHECK(is_windows_reserved_printscreen(0, VK_SNAPSHOT));
    CHECK(is_windows_reserved_printscreen(MOD_WIN, VK_SNAPSHOT));
    CHECK(!is_windows_reserved_printscreen(MOD_CONTROL, VK_SNAPSHOT));
    CHECK(!is_windows_reserved_printscreen(0, VK_F9));

    const HotkeyBinding prtscn_hb{.enabled = true, .mods = 0, .vk = VK_SNAPSHOT};
    const HotkeyBinding win_prtscn_hb{.enabled = true, .mods = MOD_WIN, .vk = VK_SNAPSHOT};
    const HotkeyBinding f9_hb{.enabled = true, .mods = 0, .vk = VK_F9};
    CHECK(should_use_printscreen_hook_fallback(prtscn_hb, true));
    CHECK(should_use_printscreen_hook_fallback(win_prtscn_hb, true));
    CHECK(!should_use_printscreen_hook_fallback(prtscn_hb, false));
    CHECK(!should_use_printscreen_hook_fallback(f9_hb, true));
    CHECK(is_hotkey_active(HotkeyState::Registered));
    CHECK(is_hotkey_active(HotkeyState::RegisteredHook));
    CHECK(!is_hotkey_active(HotkeyState::Unavailable));
    CHECK(!is_hotkey_active(HotkeyState::Disabled));
}

static void test_history_store() {
    using namespace capusy;
    const fs::path tmp = fs::temp_directory_path() / L"capusy_test_history.db";
    std::error_code ec;
    fs::remove(tmp, ec);
    fs::remove(tmp.wstring() + L"-wal", ec);
    fs::remove(tmp.wstring() + L"-shm", ec);

    HistoryStore store;
    std::string err;
    CHECK(store.open(tmp, &err));
    CHECK(store.opened());

    CaptureEntry e1{
        .path = "C:\\Captures\\2026-11\\shot1.png",
        .kind = CaptureKind::Image,
        .mode = CaptureMode::Full,
        .created_at = 1000,
        .width = 3840,
        .height = 1080,
        .size_bytes = 204800,
        .duration_ms = 0,
        .window_title = "main.cpp - Visual Studio Code",
        .process_name = "Code.exe",
        .format = "PNG",
    };
    CaptureEntry e2{
        .path = "C:\\Captures\\2026-11\\clip1.mp4",
        .kind = CaptureKind::Video,
        .mode = CaptureMode::Recording,
        .created_at = 2000,
        .width = 1280,
        .height = 720,
        .size_bytes = 1048576,
        .duration_ms = 5400,
        .window_title = "YouTube - Chrome",
        .process_name = "chrome.exe",
        .format = "MP4",
    };
    CHECK(store.insert(e1, &err));
    CHECK(e1.id > 0);
    CHECK(store.insert(e2, &err));
    CHECK(e2.id > 0);

    const auto all = store.list_recent(100);
    CHECK(all.has_value() && all->size() == 2);
    CHECK((*all)[0].path == "C:\\Captures\\2026-11\\clip1.mp4"); // newest first
    CHECK((*all)[0].window_title == "YouTube - Chrome");
    CHECK((*all)[0].process_name == "chrome.exe");
    CHECK((*all)[0].format == "MP4");
    CHECK((*all)[1].path == "C:\\Captures\\2026-11\\shot1.png");
    CHECK((*all)[1].window_title == "main.cpp - Visual Studio Code");
    CHECK((*all)[1].process_name == "Code.exe");
    CHECK((*all)[1].format == "PNG");

    const auto imgs = store.list_recent(100, CaptureKind::Image);
    CHECK(imgs.has_value() && imgs->size() == 1);
    const auto vids = store.list_recent(100, CaptureKind::Video);
    CHECK(vids.has_value() && vids->size() == 1 && (*vids)[0].duration_ms == 5400);

    const auto st = store.stats();
    CHECK(st.has_value() && st->total_count == 2 && st->image_count == 1 && st->video_count == 1);

    CHECK(store.remove(e1.id));
    const auto after = store.list_recent(100);
    CHECK(after.has_value() && after->size() == 1);

    // Verify batch removal in a transaction
    CaptureEntry e3{
        .path = "C:\\Captures\\2026-11\\shot2.png",
        .kind = CaptureKind::Image,
        .mode = CaptureMode::Region,
        .created_at = 3000,
    };
    CaptureEntry e4{
        .path = "C:\\Captures\\2026-11\\shot3.png",
        .kind = CaptureKind::Image,
        .mode = CaptureMode::Region,
        .created_at = 4000,
    };
    CHECK(store.insert(e3, &err));
    CHECK(store.insert(e4, &err));
    const std::array<int64_t, 2> batch_ids = {e2.id, e4.id};
    CHECK(store.remove_many(batch_ids, &err));
    const auto after_batch = store.list_recent(100);
    CHECK(after_batch.has_value() && after_batch->size() == 1 && (*after_batch)[0].id == e3.id);

    store.close();
    fs::remove(tmp, ec);
    fs::remove(tmp.wstring() + L"-wal", ec);
    fs::remove(tmp.wstring() + L"-shm", ec);

    // Verify legacy schema migration (existing DB without window_title / process_name / format)
    sqlite3* legacy = nullptr;
    const auto u8 = tmp.u8string();
    const std::string utf8{reinterpret_cast<const char*>(u8.data()), u8.size()};
    CHECK(sqlite3_open(utf8.c_str(), &legacy) == SQLITE_OK);
    CHECK(sqlite3_exec(legacy,
                       "CREATE TABLE captures ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT NOT NULL UNIQUE, "
                       "kind INTEGER NOT NULL DEFAULT 0, mode INTEGER NOT NULL DEFAULT 0, "
                       "created_at INTEGER NOT NULL, width INTEGER NOT NULL DEFAULT 0, "
                       "height INTEGER NOT NULL DEFAULT 0, size_bytes INTEGER NOT NULL DEFAULT 0, "
                       "duration_ms INTEGER NOT NULL DEFAULT 0);"
                       "INSERT INTO captures(path, created_at) VALUES('C:\\old.png', 500);",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(legacy);

    CHECK(store.open(tmp, &err));
    const auto migrated = store.list_recent(10);
    CHECK(migrated.has_value() && migrated->size() == 1 && (*migrated)[0].path == "C:\\old.png" && (*migrated)[0].window_title.empty());
    CHECK(store.insert(e1, &err));
    store.close();
    fs::remove(tmp, ec);
    fs::remove(tmp.wstring() + L"-wal", ec);
    fs::remove(tmp.wstring() + L"-shm", ec);
}

static void test_capture_paths_and_formats() {
    using namespace capusy;
    CHECK(format_year_month(2026, 3) == L"2026-03");
    CHECK(format_year_month(2026, 11) == L"2026-11");

    const CaptureDateTime dt{.year = 2026, .month = 4, .day = 7, .hour = 9, .minute = 5, .second = 3, .millisecond = 42};
    const fs::path split = make_capture_path_for_time(fs::path{L"C:\\Caps"}, L".png", dt, 1, true);
    CHECK(split == fs::path{L"C:\\Caps"} / L"2026-04" / L"capusy_2026-04-07_09-05-03_042_1.png");

    const fs::path flat = make_capture_path_for_time(fs::path{L"C:\\Caps"}, L".jpg", dt, 2, false);
    CHECK(flat == fs::path{L"C:\\Caps"} / L"capusy_2026-04-07_09-05-03_042_2.jpg");

    CHECK(parse_image_format("png") == ImageFormat::Png);
    CHECK(parse_image_format("jpg") == ImageFormat::Jpeg);
    CHECK(parse_image_format("JPEG") == ImageFormat::Jpeg);
    CHECK(parse_image_format("bmp") == ImageFormat::Bmp);
    CHECK(parse_image_format("tiff") == ImageFormat::Tiff);
    CHECK(parse_image_format("unknown") == ImageFormat::Png);
    CHECK(image_format_ext(ImageFormat::Jpeg) == L".jpg");
    CHECK(image_format_supports_quality(ImageFormat::Jpeg));
    CHECK(!image_format_supports_quality(ImageFormat::Png));
}

static void test_history_selection() {
    using namespace capusy;
    std::vector<CaptureEntry> items;
    for (int64_t id = 10; id <= 50; id += 10) {
        CaptureEntry e;
        e.id = id;
        items.push_back(std::move(e));
    }

    HistorySelection sel;
    sel.reconcile(items, true);
    CHECK(sel.count() == 1 && sel.focus == 0 && sel.anchor == 0 && sel.contains(10));

    // Shift+click selects contiguous range and keeps anchor fixed
    CHECK(!sel.click(items, 3, false, true));
    CHECK(sel.count() == 4 && sel.focus == 3 && sel.anchor == 0);
    CHECK(sel.contains(10) && sel.contains(20) && sel.contains(30) && sel.contains(40) && !sel.contains(50));

    // Second Shift+click shrinks range back toward the same anchor
    CHECK(!sel.click(items, 1, false, true));
    CHECK(sel.count() == 2 && sel.focus == 1 && sel.anchor == 0);
    CHECK(sel.contains(10) && sel.contains(20) && !sel.contains(30));

    // Ctrl+click toggles individual item without clearing existing selection
    CHECK(!sel.click(items, 4, true, false));
    CHECK(sel.count() == 3 && sel.focus == 4 && sel.anchor == 4);
    CHECK(sel.contains(10) && sel.contains(20) && sel.contains(50));

    // Ctrl+click on already-selected focused item removes it and moves focus to closest remaining selected
    CHECK(!sel.click(items, 4, true, false));
    CHECK(sel.count() == 2 && sel.focus == 1 && !sel.contains(50));

    // Plain click on already-selected item in multi-select returns deferred=true (for drag-out)
    CHECK(sel.click(items, 0, false, false));
    CHECK(sel.count() == 2);

    // Ctrl+Shift+click adds range to existing selection without clearing
    CHECK(!sel.click(items, 3, true, false)); // select 40, anchor=3
    CHECK(!sel.click(items, 4, true, true));  // add range 3..4 (40, 50) while keeping 10, 20
    CHECK(sel.count() == 4 && sel.contains(10) && sel.contains(20) && sel.contains(40) && sel.contains(50));

    // Plain click on unselected item collapses immediately
    CHECK(!sel.click(items, 2, false, false));
    CHECK(sel.count() == 1 && sel.focus == 2 && sel.anchor == 2 && sel.contains(30));

    // Keyboard Shift+Down / Shift+Up extends and shrinks range around anchor
    CHECK(sel.move(items, 1, true));
    CHECK(sel.move(items, 1, true));
    CHECK(sel.count() == 3 && sel.focus == 4 && sel.anchor == 2 && sel.contains(30) && sel.contains(40) && sel.contains(50));
    CHECK(sel.move(items, -1, true));
    CHECK(sel.count() == 2 && sel.focus == 3 && sel.anchor == 2 && !sel.contains(50));

    // Reconcile preserves still-visible multi-selection & tracks anchor across index shifts
    std::vector<CaptureEntry> shifted;
    CaptureEntry newest;
    newest.id = 99;
    shifted.push_back(newest);
    shifted.push_back(items[2]); // id 30 (old anchor)
    shifted.push_back(items[3]); // id 40 (old focus)
    sel.reconcile(shifted, true, 40);
    CHECK(sel.count() == 2 && sel.focus == 2 && sel.anchor == 1 && sel.contains(30) && sel.contains(40));

    // Select all
    sel.select_all(shifted);
    CHECK(sel.count() == 3 && sel.selected_indices(shifted).size() == 3);
}

static void test_history_search() {
    using namespace capusy;
    const std::wstring hay = L"capusy_2026-11-26_23-40-10_001_1.jpg main.cpp - visual studio code code.exe jpeg 26 nov 2026 • 23:40 2026-11-26 23:40:10";
    CHECK(matches_history_query_lower(hay, L""));
    CHECK(matches_history_query_lower(hay, L"   "));
    CHECK(matches_history_query_lower(hay, L"visual studio"));
    CHECK(matches_history_query_lower(hay, L"code 2026-11"));
    CHECK(matches_history_query_lower(hay, L"23:40 jpeg"));
    CHECK(matches_history_query_lower(hay, L"nov 2026"));
    CHECK(!matches_history_query_lower(hay, L"code firefox"));
}

static void test_update_logic() {
    using namespace capusy;
    CHECK(is_newer_version("v0.2.0", "0.1.0"));
    CHECK(is_newer_version("1.0.0", "1.0.0-rc1"));
    CHECK(!is_newer_version("0.1.0", "0.1.0"));
    CHECK(!is_newer_version("0.0.9", "0.1.0"));
    CHECK(normalize_version_string(" v0.3.1 ") == "0.3.1");

    CHECK(should_auto_check_updates(true, 0, 1000));
    CHECK(!should_auto_check_updates(false, 0, 1000));
    CHECK(!should_auto_check_updates(true, 1000, 1000 + UPDATE_CHECK_INTERVAL_S - 1));
    CHECK(should_auto_check_updates(true, 1000, 1000 + UPDATE_CHECK_INTERVAL_S));

    CHECK(parse_tar_supports_7z("bsdtar 3.7.7 - libarchive 3.7.7 zlib/1.3.1 liblzma/5.4.5"));
    CHECK(!parse_tar_supports_7z("bsdtar 3.5.2 - libarchive 3.5.2 zlib/1.2.11"));

    const std::string good_sha = "sha256:ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789";
    CHECK(extract_sha256_digest(good_sha).size() == 64);
    CHECK(extract_sha256_digest("sha256:short").empty());

    const std::array<GithubAsset, 2> assets = {{
        {.name = "capusy.zip", .browser_download_url = "https://example.test/capusy.zip", .size = 100},
        {.name = "capusy.7z", .browser_download_url = "https://example.test/capusy.7z", .size = 80},
    }};
    const auto a7 = pick_release_asset(assets, true);
    CHECK(a7.has_value() && a7->name == "capusy.7z");
    const auto az = pick_release_asset(assets, false);
    CHECK(az.has_value() && az->name == "capusy.zip");
}

static void test_ui_primitives() {
    using namespace capusy::ui;
    AnimFloat a;
    a.snap(0.0f);
    a.set(1.0f, true);
    CHECK(a.active());
    for (int i = 0; i < 60; ++i) (void)a.step(0.016f, 22.0f);
    CHECK(!a.active());
    CHECK(a.value == 1.0f);

    const std::array<FlowItem, 3> items = {{{100, 30, false}, {100, 30, false}, {100, 30, true}}};
    std::array<FlowPlacement, 3> out{};
    const float h = flow_layout_into(items, 10.0f, 8.0f, 220.0f, out);
    CHECK(out[0].line == 0 && out[1].line == 0 && out[2].line == 1);
    CHECK(out[2].width == 220.0f);
    CHECK(h == 68.0f);

    FlipDirtyTracker tracker;
    const auto p0 = tracker.plan(false, {}, 800, 600);
    CHECK(p0.full);
    tracker.commit(false, p0);
    const auto p1 = tracker.plan(false, {}, 800, 600);
    CHECK(p1.full);
    tracker.commit(false, p1);
    const auto p2 = tracker.plan(false, {}, 800, 600);
    CHECK(p2.skip);
}

static void test_audio_capture_helpers() {
    using namespace capusy;

    // Sample decoding (PCM16, Float32, PCM24, PCM8)
    const int16_t s16 = 16384;
    CHECK(std::abs(decode_audio_sample(reinterpret_cast<const uint8_t*>(&s16), AudioSampleFormat::Pcm16) - 0.5f) < 0.001f);
    const float sf32 = -0.75f;
    CHECK(std::abs(decode_audio_sample(reinterpret_cast<const uint8_t*>(&sf32), AudioSampleFormat::Float32) + 0.75f) < 0.0001f);
    const uint8_t s8 = 128;
    CHECK(std::abs(decode_audio_sample(&s8, AudioSampleFormat::Pcm8)) < 0.001f);

    // Mono & 5.1 downmix to stereo
    const std::array<float, 1> mono = {0.4f};
    const StereoFrame dm1 = downmix_to_stereo(mono);
    CHECK(std::abs(dm1.l - 0.4f) < 0.0001f && std::abs(dm1.r - 0.4f) < 0.0001f);

    const std::array<float, 6> ch51 = {1.0f, 0.0f, 0.5f, 0.0f, 0.0f, 0.0f};
    const StereoFrame dm51 = downmix_to_stereo(ch51);
    CHECK(dm51.l > dm51.r && dm51.r > 0.1f);

    // Resampler pass-through at 48 kHz
    StereoResampler r48;
    r48.reset(48000, 48000);
    StereoFloatFifo fifo48{256};
    const std::array<StereoFrame, 4> in4 = {{{0.1f, -0.1f}, {0.2f, -0.2f}, {0.3f, -0.3f}, {0.4f, -0.4f}}};
    r48.push_stereo(in4, fifo48);
    CHECK(fifo48.frames_available() == 4);
    std::array<float, 8> out4{};
    CHECK(fifo48.pop_frames(out4.data(), 4) == 4);
    CHECK(std::abs(out4[0] - 0.1f) < 0.0001f && std::abs(out4[7] + 0.4f) < 0.0001f);
    CHECK(fifo48.frames_available() == 0);

    // Resampler 44.1 kHz -> 48 kHz preserves duration within 1 frame
    StereoResampler r44;
    r44.reset(44100, 48000);
    StereoFloatFifo fifo44{2048};
    std::vector<StereoFrame> chunk44(441, {.l = 0.5f, .r = -0.5f}); // 10 ms at 44.1 kHz
    for (int i = 0; i < 10; ++i) r44.push_stereo(chunk44, fifo44);   // 100 ms total -> ~4800 frames at 48 kHz
    CHECK(fifo44.frames_available() >= 4795 && fifo44.frames_available() <= 4805);

    // FIFO trim_excess keeps latency bounded
    fifo44.trim_excess(2000, 1000);
    CHECK(fifo44.frames_available() == 1000);

    // Mixing + soft-limiting prevents int16 overflow wrap
    const std::array<float, 4> a = {0.8f, -0.8f, 0.25f, 0.0f};
    const std::array<float, 4> b = {0.8f, -0.8f, 0.25f, -0.5f};
    std::array<int16_t, 4> mixed{};
    mix_stereo_to_pcm16(a, b, mixed);
    CHECK(mixed[0] > 28000 && mixed[0] <= 32767);
    CHECK(mixed[1] < -28000 && mixed[1] >= -32767);
    CHECK(std::abs(mixed[2] - 16384) <= 2);
    CHECK(std::abs(mixed[3] + 16384) <= 2);

    // Mono mix downmixes stereo L/R and applies soft-limiting
    const std::array<float, 4> ma = {0.5f, -0.5f, 0.9f, 0.9f};
    const std::array<float, 4> mb = {0.0f, 0.0f, 0.9f, 0.9f};
    std::array<int16_t, 2> mono_out{};
    mix_stereo_to_mono_pcm16(ma, mb, mono_out);
    CHECK(std::abs(mono_out[0]) <= 1);
    CHECK(mono_out[1] > 28000 && mono_out[1] <= 32767);

    // Device label resolution
    const std::vector<AudioDeviceInfo> devs = {
        {.id = "id-spk", .name = L"Speakers (Realtek)", .is_default = true},
        {.id = "id-hp", .name = L"Headphones (USB)", .is_default = false},
    };
    CHECK(resolve_audio_device_label("", devs, L"Default Output") == L"Default Output (Speakers (Realtek))");
    CHECK(resolve_audio_device_label("id-hp", devs, L"Default Output") == L"Headphones (USB)");
    CHECK(resolve_audio_device_label("missing-id", devs, L"Default Output") == L"Default Output (Speakers (Realtek))");
    CHECK(resolve_audio_device_label("", {}, L"Default Output") == L"No device found");
}

static void test_recording_logic() {
    using namespace capusy;

    CHECK(parse_record_format("mp4") == RecordFormat::Mp4);
    CHECK(parse_record_format("WMV") == RecordFormat::Wmv);
    CHECK(parse_record_format("gif") == RecordFormat::Gif);
    CHECK(parse_record_format("unknown") == RecordFormat::Mp4);
    CHECK(record_format_ext(RecordFormat::Mp4) == L".mp4");
    CHECK(record_format_ext(RecordFormat::Wmv) == L".wmv");
    CHECK(record_format_ext(RecordFormat::Gif) == L".gif");
    CHECK(record_format_supports_audio(RecordFormat::Mp4));
    CHECK(record_format_supports_audio(RecordFormat::Wmv));
    CHECK(!record_format_supports_audio(RecordFormat::Gif));

    CHECK(parse_video_codec("h264") == VideoCodec::H264);
    CHECK(parse_video_codec("hevc") == VideoCodec::Hevc);
    CHECK(parse_video_codec("H.265") == VideoCodec::Hevc);
    CHECK(parse_video_codec("invalid") == VideoCodec::H264);

    CHECK(parse_h264_profile("baseline") == H264Profile::Baseline);
    CHECK(parse_h264_profile("main") == H264Profile::Main);
    CHECK(parse_h264_profile("high") == H264Profile::High);
    CHECK(parse_h264_profile("other") == H264Profile::High);

    CHECK(parse_rate_control("vbr") == RateControlMode::Vbr);
    CHECK(parse_rate_control("quality") == RateControlMode::Quality);
    CHECK(parse_rate_control("cbr") == RateControlMode::Cbr);
    CHECK(parse_rate_control("unknown") == RateControlMode::Vbr);

    CHECK(clamp_record_fps(1) == RECORD_FPS_MIN);
    CHECK(clamp_record_fps(24) == 24u);
    CHECK(clamp_record_fps(120) == RECORD_FPS_MAX);
    CHECK(effective_record_fps(60, RecordFormat::Mp4) == 60u);
    CHECK(effective_record_fps(60, RecordFormat::Gif) == RECORD_GIF_FPS_MAX);
    CHECK(effective_record_fps(15, RecordFormat::Gif) == 15u);

    CHECK(clamp_record_bitrate_mbps(0) == 1u);
    CHECK(clamp_record_bitrate_mbps(15) == 15u);
    CHECK(clamp_record_bitrate_mbps(99) == 50u);
    CHECK(clamp_record_quality(0) == 10u);
    CHECK(clamp_record_quality(85) == 85u);
    CHECK(clamp_record_quality(120) == 100u);
    CHECK(clamp_record_scale_pct(10) == 25u);
    CHECK(clamp_record_scale_pct(75) == 75u);
    CHECK(clamp_record_scale_pct(200) == 100u);
    CHECK(clamp_record_keyframe_s(0) == 1u);
    CHECK(clamp_record_keyframe_s(5) == 5u);
    CHECK(clamp_record_keyframe_s(30) == 10u);

    CHECK(normalize_audio_bitrate_kbps(64) == 96u);
    CHECK(normalize_audio_bitrate_kbps(128) == 128u);
    CHECK(normalize_audio_bitrate_kbps(160) == 160u);
    CHECK(normalize_audio_bitrate_kbps(320) == 192u);
    CHECK(normalize_audio_sample_rate(44100) == 44100u);
    CHECK(normalize_audio_sample_rate(48000) == 48000u);
    CHECK(normalize_audio_channels(1) == 1u);
    CHECK(normalize_audio_channels(2) == 2u);
    CHECK(normalize_audio_channels(6) == 2u);
    CHECK(normalize_gif_colors(32) == 64u);
    CHECK(normalize_gif_colors(128) == 128u);
    CHECK(normalize_gif_colors(256) == 256u);

    // Scaling preserves even alignment & minimum 16x16
    const auto [w1, h1] = compute_scaled_video_dimensions(1920, 1080, 50, true);
    CHECK(w1 == 960u && h1 == 540u);
    const auto [w2, h2] = compute_scaled_video_dimensions(1921, 1081, 100, true);
    CHECK(w2 == 1920u && h2 == 1080u);
    const auto [w3, h3] = compute_scaled_video_dimensions(20, 20, 25, true);
    CHECK(w3 == 16u && h3 == 16u);
    const auto [w4, h4] = compute_scaled_video_dimensions(301, 201, 100, false);
    CHECK(w4 == 301u && h4 == 201u);

    // GIF frame delay centiseconds
    CHECK(gif_frame_delay_cs(10) == 10u);
    CHECK(gif_frame_delay_cs(20) == 5u);
    CHECK(gif_frame_delay_cs(30) == 3u);

    // GIF detection helper
    CHECK(is_gif_capture("GIF", "C:\\clip.mp4"));
    CHECK(is_gif_capture("", "C:\\clip.gif"));
    CHECK(!is_gif_capture("MP4", "C:\\clip.mp4"));

    // Spec summary formatting
    CHECK(format_recording_spec_summary(RecordFormat::Mp4, VideoCodec::H264, RateControlMode::Vbr, 30, 12, 80, 100, 256) == L"MP4 • H.264 • 30 FPS • 12M");
    CHECK(format_recording_spec_summary(RecordFormat::Mp4, VideoCodec::Hevc, RateControlMode::Quality, 60, 12, 85, 75, 256) == L"MP4 • HEVC • 60 FPS • Q85 • 75%");
    CHECK(format_recording_spec_summary(RecordFormat::Wmv, VideoCodec::H264, RateControlMode::Cbr, 24, 8, 80, 100, 256) == L"WMV • 24 FPS • 8M");
    CHECK(format_recording_spec_summary(RecordFormat::Gif, VideoCodec::H264, RateControlMode::Vbr, 60, 12, 80, 50, 128) == L"GIF • 30 FPS • 128c • 50%");

    // Full-screen / tray-only recording detection
    const RectI work{.left = 0, .top = 0, .right = 1920, .bottom = 1040};
    const RectI full_mon{.left = 0, .top = 0, .right = 1920, .bottom = 1080};
    const RectI mid_region{.left = 200, .top = 150, .right = 1200, .bottom = 750};
    const RectI bottom_region{.left = 200, .top = 300, .right = 1200, .bottom = 1030};
    const RectI top_region{.left = 200, .top = 10, .right = 1200, .bottom = 800};
    const RectI tall_region{.left = 100, .top = 20, .right = 1800, .bottom = 1010};
    CHECK(should_use_tray_recording_mode(full_mon, work, 40));
    CHECK(should_use_tray_recording_mode(work, work, 40));
    CHECK(!should_use_tray_recording_mode(mid_region, work, 40));
    CHECK(!should_use_tray_recording_mode(bottom_region, work, 40)); // fits above
    CHECK(!should_use_tray_recording_mode(top_region, work, 40));    // fits below
    CHECK(should_use_tray_recording_mode(tall_region, work, 40));    // neither fits
}

static void test_color_logic() {
    using namespace capusy;

    const Rgb8 coral{.r = 255, .g = 92, .b = 92};
    CHECK(rgb_to_u32(coral) == 0xFF5C5Cu);
    CHECK(u32_to_rgb(0xFF5C5Cu) == coral);
    CHECK(format_hex_rgb(coral) == L"#FF5C5C");
    CHECK(format_rgb_label(coral) == L"RGB(255, 92, 92)");
    const Rgb8 dark_blue{0, 15, 160};
    const Rgb8 white{255, 255, 255};
    const Rgb8 yellow{255, 212, 59};
    const Rgb8 dark_ink{20, 18, 24};
    CHECK(format_hex_rgb(dark_blue) == L"#000FA0");
    CHECK(format_rgb_label(dark_blue) == L"RGB(0, 15, 160)");

    CHECK(is_light_rgb(white));
    CHECK(is_light_rgb(yellow));
    CHECK(!is_light_rgb(dark_ink));

    // HSV <-> RGB round-trips across primaries & neutral tones
    const std::array<Rgb8, 8> samples = {{
        {255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0},
        {0, 255, 255}, {255, 0, 255}, {255, 255, 255}, {0, 0, 0},
    }};
    for (const Rgb8 s : samples) {
        const Hsv h = rgb_to_hsv(s);
        const Rgb8 back = hsv_to_rgb(h.h, h.s, h.v);
        CHECK(back == s);
    }
    const Rgb8 mid{.r = 77, .g = 171, .b = 247};
    const Hsv mh = rgb_to_hsv(mid);
    const Rgb8 mback = hsv_to_rgb(mh.h, mh.s, mh.v);
    CHECK(std::abs(static_cast<int>(mback.r) - static_cast<int>(mid.r)) <= 1);
    CHECK(std::abs(static_cast<int>(mback.g) - static_cast<int>(mid.g)) <= 1);
    CHECK(std::abs(static_cast<int>(mback.b) - static_cast<int>(mid.b)) <= 1);

    // BGRA pixel sampling + bounds checks
    const std::array<uint8_t, 16> bgra = {
        0x10, 0x20, 0x30, 0xFF,  0x40, 0x50, 0x60, 0xFF,
        0x70, 0x80, 0x90, 0xFF,  0xAA, 0xBB, 0xCC, 0xFF,
    };
    const Rgb8 exp00{0x30, 0x20, 0x10};
    const Rgb8 exp11{0xCC, 0xBB, 0xAA};
    CHECK(sample_bgra_pixel(bgra, 2, 2, 0, 0) == exp00);
    CHECK(sample_bgra_pixel(bgra, 2, 2, 1, 1) == exp11);
    CHECK(!sample_bgra_pixel(bgra, 2, 2, -1, 0).has_value());
    CHECK(!sample_bgra_pixel(bgra, 2, 2, 2, 0).has_value());
    CHECK(!sample_bgra_pixel(bgra, 2, 2, 0, 2).has_value());
}

int main() {
    test_startup_logic();
    test_hotkey_collisions();
    test_history_store();
    test_history_selection();
    test_capture_paths_and_formats();
    test_history_search();
    test_update_logic();
    test_ui_primitives();
    test_audio_capture_helpers();
    test_recording_logic();
    test_color_logic();
    return capusy::test::failures == 0 ? 0 : 1;
}
