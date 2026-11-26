#include "check.hpp"

#include <capusy/history_store.hpp>
#include <capusy/hotkeys.hpp>
#include <capusy/startup_logic.hpp>
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
        .path = "C:\\Captures\\shot1.png",
        .kind = CaptureKind::Image,
        .mode = CaptureMode::Full,
        .created_at = 1000,
        .width = 3840,
        .height = 1080,
        .size_bytes = 204800,
        .duration_ms = 0,
    };
    CaptureEntry e2{
        .path = "C:\\Captures\\clip1.mp4",
        .kind = CaptureKind::Video,
        .mode = CaptureMode::Recording,
        .created_at = 2000,
        .width = 1280,
        .height = 720,
        .size_bytes = 1048576,
        .duration_ms = 5400,
    };
    CHECK(store.insert(e1, &err));
    CHECK(e1.id > 0);
    CHECK(store.insert(e2, &err));
    CHECK(e2.id > 0);

    const auto all = store.list_recent(100);
    CHECK(all.has_value() && all->size() == 2);
    CHECK((*all)[0].path == "C:\\Captures\\clip1.mp4"); // newest first
    CHECK((*all)[1].path == "C:\\Captures\\shot1.png");

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
    test_ui_primitives();
    return capusy::test::failures == 0 ? 0 : 1;
}
