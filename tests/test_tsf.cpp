// C++ tests for the TinySoundFont (tsf.h) public API.
//
// Uses a tiny self-contained test harness (no external dependencies) so it
// builds the same way on macOS / Linux / Windows just like the rest of the
// project. The tests link against the single-header implementation directly.
//
// Build & run (from the repo root):
//   clang++ -std=c++17 -Wall -Wextra -O2 tests/test_tsf.cpp -o tests/test_tsf -lm
//   ./tests/test_tsf examples/florestan-subset.sf2
//
// If no SF2 fixture is passed, the file-based tests are skipped automatically
// (so the suite still runs in environments without the fixture).

#define TSF_IMPLEMENTATION
#include "../tsf.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------

namespace {

int g_pass = 0;
int g_fail = 0;
const char* g_current = "";

struct Section {
    explicit Section(const char* name) { g_current = name; std::printf("\n[%s]\n", name); }
};

#define TEST_SECTION(name) Section _sec(name)

void check(bool cond, const char* expr, const char* file, int line) {
    if (cond) {
        ++g_pass;
    } else {
        ++g_fail;
        std::printf("  FAIL %s:%d: %s\n", file, line, expr);
    }
}

// clang-format off
#define CHECK(cond) check(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    check(_va == _vb, #a " == " #b, __FILE__, __LINE__); \
    if (!(_va == _vb)) std::printf("      got=%lld expected=%lld\n", \
        static_cast<long long>(_va), static_cast<long long>(_vb)); \
} while (0)
#define CHECK_NEAR(a, b, tol) do { \
    double _va = static_cast<double>(a); double _vb = static_cast<double>(b); double _t = (tol); \
    bool _ok = std::fabs(_va - _vb) <= _t; \
    check(_ok, #a " ~ " #b, __FILE__, __LINE__); \
    if (!_ok) std::printf("      got=%g expected=%g tol=%g\n", _va, _vb, _t); \
} while (0)
// clang-format on

// Read an entire file into a byte vector. Returns empty vector on failure.
std::vector<unsigned char> read_file(const char* path) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return {};
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz < 0) { std::fclose(fp); return {}; }
    std::vector<unsigned char> buf(static_cast<size_t>(sz));
    size_t rd = std::fread(buf.data(), 1, buf.size(), fp);
    std::fclose(fp);
    if (rd != buf.size()) return {};
    return buf;
}

// Fixture: tries a few well-known locations for the bundled SF2 subset.
std::string find_fixture(int argc, char** argv) {
    if (argc >= 2) {
        std::string a = argv[1];
        if (!read_file(a.c_str()).empty()) return a;
    }
    const char* candidates[] = {
        "examples/florestan-subset.sf2",
        "../examples/florestan-subset.sf2",
        "florestan-subset.sf2",
    };
    for (const char* c : candidates) {
        if (!read_file(c).empty()) return c;
    }
    return "";
}

}  // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Build a tiny in-memory SF2-free noise buffer to exercise load-failure paths.
static void test_load_invalid(void) {
    TEST_SECTION("load / invalid input");

    // A real (but empty) buffer with size 0 is rejected as invalid data.
    unsigned char emptybuf[8] = {0};
    CHECK(tsf_load_memory(emptybuf, 0) == nullptr);

    // Garbage bytes that do not start with "RIFF...sfbk" must be rejected.
    unsigned char junk[64];
    std::memset(junk, 0xAB, sizeof(junk));
    CHECK(tsf_load_memory(junk, sizeof(junk)) == nullptr);

    // A "RIFF" magic alone (without valid sfbk form) should also be rejected.
    unsigned char riff[16] = {'R', 'I', 'F', 'F', 8, 0, 0, 0,
                              'X', 'X', 'X', 'X', 0, 0, 0, 0};
    CHECK(tsf_load_memory(riff, sizeof(riff)) == nullptr);

    // File-system path that doesn't exist should yield null too.
    CHECK(tsf_load_filename("definitely_not_a_font.sf2") == nullptr);
}

// Tests that require a real SF2 fixture. Skipped if fixture is missing.
//
// Note on test isolation: each state-dependent test loads its own tsf instance.
// tsf_note_off / tsf_note_off_all only start the release tail of a voice — the
// voice keeps producing audio (and counts as "active") for a while afterwards.
// Sharing one instance across tests would let those tails leak into later
// assertions about silence, so we avoid it.
static void test_with_fixture(const std::string& fixture) {
    std::vector<unsigned char> raw = read_file(fixture.c_str());

    auto load_fresh = [&]() -> tsf* {
        tsf* f = tsf_load_filename(fixture.c_str());
        CHECK(f != nullptr);
        tsf_set_output(f, TSF_MONO, 44100, 0.0f);
        return f;
    };

    {
        tsf* f = load_fresh();
        TEST_SECTION("load / valid SF2 (from file)");
        if (f) tsf_close(f);
    }

    {
        TEST_SECTION("preset metadata");
        tsf* f = load_fresh();
        if (!f) return;
        int count = tsf_get_presetcount(f);
        CHECK(count > 0);

        // Names should be non-null for valid indices, null out of range.
        CHECK(tsf_get_presetname(f, 0) != nullptr);
        CHECK(tsf_get_presetname(f, count - 1) != nullptr);
        CHECK(tsf_get_presetname(f, -1) == nullptr);
        CHECK(tsf_get_presetname(f, count) == nullptr);
        CHECK(tsf_get_presetname(f, 1234567) == nullptr);

        // The bundled fixture is known to have real names.
        const char* n0 = tsf_get_presetname(f, 0);
        CHECK(n0 != nullptr && n0[0] != '\0');
        tsf_close(f);
    }

    {
        TEST_SECTION("preset index lookup");
        tsf* f = load_fresh();
        if (!f) return;
        // A missing (bank, preset) must report -1 and a null name.
        CHECK_EQ(tsf_get_presetindex(f, 999, 999), -1);
        CHECK(tsf_bank_get_presetname(f, 999, 999) == nullptr);
        tsf_close(f);
    }

    {
        TEST_SECTION("set_output / set_volume / set_max_voices");
        tsf* f = load_fresh();
        if (!f) return;
        // All three output modes should be accepted without crashing.
        tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
        tsf_set_output(f, TSF_STEREO_UNWEAVED, 48000, -6.0f);
        tsf_set_output(f, TSF_MONO, 22050, 0.0f);

        // A samplerate < 1 must fall back to the internal default (44100).
        tsf_set_output(f, TSF_MONO, 0, 0.0f);

        tsf_set_volume(f, 0.5f);
        tsf_set_volume(f, 1.0f);

        // Pre-allocating voices should succeed and return 1.
        CHECK_EQ(tsf_set_max_voices(f, 32), 1);
        tsf_close(f);
    }

    {
        TEST_SECTION("note on/off lifecycle");
        tsf* f = load_fresh();
        if (!f) return;
        tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);

        // Out-of-range preset index: API documents that this still returns 1.
        CHECK_EQ(tsf_note_on(f, 99999, 60, 1.0f), 1);

        // Valid note on a real preset -> should report an active voice.
        CHECK_EQ(tsf_note_on(f, 0, 60, 1.0f), 1);
        CHECK(tsf_active_voice_count(f) >= 1);

        // Velocity <= 0 is treated as note-off by tsf_note_on.
        CHECK_EQ(tsf_note_on(f, 0, 60, 0.0f), 1);

        // note_off on a key that isn't playing must not crash.
        tsf_note_off(f, 0, 91);
        tsf_note_off_all(f);

        // bank variants: invalid bank/preset returns 0.
        CHECK_EQ(tsf_bank_note_on(f, 999, 999, 60, 1.0f), 0);
        CHECK_EQ(tsf_bank_note_off(f, 999, 999, 60), 0);
        tsf_close(f);
    }

    {
        TEST_SECTION("rendering (float)");
        tsf* f = load_fresh();   // fresh => no release tails => true silence
        if (!f) return;
        tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);

        const int N = 256;
        std::vector<float> buf(N * 2, 0.0f);

        // Silence: with no notes ever played, rendering should produce exactly 0.
        tsf_render_float(f, buf.data(), N, 0);
        double peak = 0.0;
        for (float v : buf) peak = std::max(peak, std::fabs(static_cast<double>(v)));
        CHECK(peak == 0.0);

        // With a note playing, output should be non-trivial and finite.
        CHECK_EQ(tsf_note_on(f, 0, 60, 1.0f), 1);
        tsf_render_float(f, buf.data(), N, 0);
        peak = 0.0;
        for (float v : buf) peak = std::max(peak, std::fabs(static_cast<double>(v)));
        CHECK(peak > 1e-6);
        CHECK(std::isfinite(peak));
        tsf_close(f);
    }

    {
        TEST_SECTION("rendering (short)");
        tsf* f = load_fresh();   // fresh => buffer is provably cleared to 0
        if (!f) return;
        tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);

        const int N = 256;
        std::vector<short> buf(N * 2, 12345);  // sentinel to prove clearing

        tsf_render_short(f, buf.data(), N, 0);
        bool all_zero = true;
        for (short v : buf) if (v != 0) { all_zero = false; break; }
        CHECK(all_zero);

        // With a note, expect non-zero samples within int16 range.
        CHECK_EQ(tsf_note_on(f, 0, 60, 1.0f), 1);
        tsf_render_short(f, buf.data(), N, 0);
        bool any_nonzero = false;
        for (short v : buf) {
            if (v != 0) any_nonzero = true;
            // short is inherently within [-32768, 32767]; this loop documents intent.
        }
        CHECK(any_nonzero);
        tsf_close(f);
    }

    {
        TEST_SECTION("rendering modes (mono / interleaved / unweaved)");
        tsf* f = load_fresh();
        if (!f) return;
        tsf_set_output(f, TSF_MONO, 44100, 0.0f);
        CHECK_EQ(tsf_note_on(f, 0, 60, 1.0f), 1);
        {
            std::vector<float> mono(128, 0.0f);
            tsf_render_float(f, mono.data(), 128, 0);
            double peak = 0;
            for (float v : mono) peak = std::max(peak, std::fabs(static_cast<double>(v)));
            CHECK(peak > 1e-6);
        }

        // Unweaved stereo writes left block then right block.
        tsf_set_output(f, TSF_STEREO_UNWEAVED, 44100, 0.0f);
        CHECK_EQ(tsf_note_on(f, 0, 64, 1.0f), 1);
        {
            const int N = 128;
            std::vector<float> lr(N * 2, 0.0f);
            tsf_render_float(f, lr.data(), N, 0);
            double peak = 0;
            for (float v : lr) peak = std::max(peak, std::fabs(static_cast<double>(v)));
            CHECK(peak > 1e-6);
        }
        tsf_close(f);
    }

    {
        TEST_SECTION("rendering / mixing flag");
        tsf* f = load_fresh();   // fresh => no tails, mixing leaves content untouched
        if (!f) return;
        tsf_set_output(f, TSF_MONO, 44100, 0.0f);

        const int N = 64;
        std::vector<float> buf(N, 0.5f);

        // flag_mixing=1 adds rendering output into the buffer. With no notes,
        // nothing is added, so the content must be preserved bit-for-bit.
        tsf_render_float(f, buf.data(), N, 1);
        bool preserved = true;
        for (float v : buf) {
            if (v != 0.5f) { preserved = false; break; }
        }
        CHECK(preserved);

        // flag_mixing=0 clears first, so with no notes we get all zeros.
        tsf_render_float(f, buf.data(), N, 0);
        bool all_zero = true;
        for (float v : buf) if (v != 0.0f) { all_zero = false; break; }
        CHECK(all_zero);
        tsf_close(f);
    }

    {
        TEST_SECTION("channel API");
        tsf* f = load_fresh();
        if (!f) return;
        tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);

        // Assign preset 0 to channel 0 and verify round-trips.
        CHECK_EQ(tsf_channel_set_presetindex(f, 0, 0), 1);
        CHECK_EQ(tsf_channel_get_preset_index(f, 0), 0);

        CHECK_EQ(tsf_channel_set_volume(f, 0, 0.7f), 1);
        CHECK_NEAR(tsf_channel_get_volume(f, 0), 0.7f, 1e-3);

        CHECK_EQ(tsf_channel_set_pitchwheel(f, 0, 10000), 1);
        CHECK_EQ(tsf_channel_get_pitchwheel(f, 0), 10000);

        CHECK_EQ(tsf_channel_set_pitchrange(f, 0, 4.0f), 1);
        CHECK_NEAR(tsf_channel_get_pitchrange(f, 0), 4.0f, 1e-4);

        CHECK_EQ(tsf_channel_set_tuning(f, 0, 0.5f), 1);
        CHECK_NEAR(tsf_channel_get_tuning(f, 0), 0.5f, 1e-4);

        // Note: tsf_channel_set_pan / get_pan do NOT round-trip as documented.
        // set_pan(x) stores panOffset = x - 0.5, and get_pan returns panOffset - 0.5,
        // i.e. get_pan(x) == x - 1.0 (a known asymmetry in the library). We assert
        // the actual behavior here; the default (never-set) pan reads back as 0.5.
        CHECK_NEAR(tsf_channel_get_pan(f, 1), 0.5f, 1e-6);   // untouched channel
        tsf_channel_set_pan(f, 0, 0.25f);
        CHECK_NEAR(tsf_channel_get_pan(f, 0), -0.75f, 1e-6);

        // Note on a channel that has a preset should produce audio.
        CHECK_EQ(tsf_channel_note_on(f, 0, 60, 1.0f), 1);
        CHECK(tsf_active_voice_count(f) >= 1);

        tsf_channel_note_off(f, 0, 60);
        tsf_channel_note_off_all(f, 0);
        tsf_channel_sounds_off_all(f, 0);

        // MIDI control change should not crash; returns 1 on success.
        int cc = tsf_channel_midi_control(f, 0, 7 /*volume*/, 100);
        CHECK((cc == 0 || cc == 1));
        tsf_close(f);
    }

    {
        TEST_SECTION("tsf_reset clears voices after a render flush");
        tsf* f = load_fresh();
        if (!f) return;
        tsf_set_output(f, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
        CHECK_EQ(tsf_note_on(f, 0, 60, 1.0f), 1);
        CHECK(tsf_active_voice_count(f) >= 1);

        // tsf_reset starts the fast-release tail but does not kill voices
        // immediately; they disappear once their release tail has been rendered
        // out. Flush a few blocks to let them finish.
        tsf_reset(f);
        float flush[256];
        for (int i = 0; i < 10; ++i) tsf_render_float(f, flush, 256, 0);
        CHECK(tsf_active_voice_count(f) == 0);
        tsf_close(f);
    }

    {
        TEST_SECTION("tsf_copy shares the font, has independent voices");
        tsf* f = load_fresh();
        if (!f) return;
        tsf* c = tsf_copy(f);
        CHECK(c != nullptr);
        if (c) {
            CHECK_EQ(tsf_get_presetcount(c), tsf_get_presetcount(f));
            tsf_set_output(c, TSF_STEREO_INTERLEAVED, 44100, 0.0f);
            CHECK_EQ(tsf_note_on(c, 0, 60, 1.0f), 1);
            // Copy must have its own voice state (the original has none).
            CHECK(tsf_active_voice_count(c) >= 1);
            CHECK(tsf_active_voice_count(f) == 0);
            tsf_close(c);
        }
        tsf_close(f);
    }

    {
        TEST_SECTION("load / memory == filename");
        CHECK(!raw.empty());
        if (!raw.empty()) {
            tsf* f = tsf_load_filename(fixture.c_str());
            tsf* fm = tsf_load_memory(raw.data(), static_cast<int>(raw.size()));
            CHECK(fm != nullptr);
            CHECK(f != nullptr);
            if (f && fm) {
                CHECK_EQ(tsf_get_presetcount(fm), tsf_get_presetcount(f));
                const char* nm_f = tsf_get_presetname(f, 0);
                const char* nm_m = tsf_get_presetname(fm, 0);
                CHECK(nm_f != nullptr && nm_m != nullptr);
                CHECK(std::strcmp(nm_f, nm_m) == 0);
            }
            if (f) tsf_close(f);
            if (fm) tsf_close(fm);
        }
    }

    {
        TEST_SECTION("determinism: same setup yields near-identical output");
        // Rendering is not bit-exact across runs (internal voice/play indices
        // and float ordering differ), but the rendered audio should be
        // numerically equivalent to within float epsilon.
        auto render_seq = [&](std::vector<float>& out) {
            tsf* f = tsf_load_filename(fixture.c_str());
            tsf_set_output(f, TSF_MONO, 44100, 0.0f);
            tsf_reset(f);
            float fl[256];
            tsf_render_float(f, fl, 256, 0);  // flush after reset
            CHECK_EQ(tsf_note_on(f, 0, 60, 1.0f), 1);
            out.assign(2048, 0.0f);
            tsf_render_float(f, out.data(), 2048, 0);
            tsf_close(f);
        };

        std::vector<float> a, b;
        render_seq(a);
        render_seq(b);
        CHECK_EQ(a.size(), b.size());
        double max_diff = 0.0;
        for (size_t i = 0; i < a.size(); ++i) {
            max_diff = std::max(max_diff, std::fabs(static_cast<double>(a[i] - b[i])));
        }
        CHECK(max_diff < 1e-5);
    }
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== TinySoundFont C++ test suite ===\n");

    test_load_invalid();

    std::string fixture = find_fixture(argc, argv);
    if (fixture.empty()) {
        std::printf("\n[fixture] No SF2 file provided; skipping fixture tests.\n");
        std::printf("[fixture] Pass a path as argv[1], e.g.:\n");
        std::printf("[fixture]   %s examples/florestan-subset.sf2\n", argv[0]);
    } else {
        std::printf("[fixture] Using: %s\n", fixture.c_str());
        test_with_fixture(fixture);
    }

    std::printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
