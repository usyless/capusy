#include "check.hpp"

#include <capusy/capture_logic.hpp>
#include <capusy/history_store.hpp>
#include <capusy/hotkeys.hpp>
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

int main() {
    test_startup_logic();
    test_hotkey_collisions();
    test_history_store();
    test_capture_paths_and_formats();
    test_history_search();
    test_update_logic();
    test_ui_primitives();
    return capusy::test::failures == 0 ? 0 : 1;
}
