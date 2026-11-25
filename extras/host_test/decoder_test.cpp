// Host-side regression test for src/USBPacketDecoder.cpp. No hardware needed.
//
// The PIO DPLL delivers one clock-recovered 2-bit [D+, D-] symbol per USB bit,
// 16 per 32-bit word, first symbol in bits 1:0. This test synthesizes that stream
// from spec-level packets (NRZI, bit stuffing, CRC5/CRC16 computed here, not with
// the decoder's tables), cuts it into separately allocated DMA-sized buffers (so an
// out-of-bounds read trips AddressSanitizer) and checks that:
//
//   1. single-core feedSamples() returns every packet exactly once, in bus order;
//   2. dual-core feedSamplesStriped(), driven like USBSnifferPIO::task() and
//      taskCore0() (even buffers -> one decoder, odd -> the other, each given the
//      previous and the next buffer), returns the same sequence;
//   3. sliding one IN/DATA/ACK transaction across a buffer boundary, one symbol
//      at a time, never loses or duplicates a packet;
//   4. negative control: with margin_words = 0 the dual-core path loses packets,
//      which proves this test can see a loss;
//   5. single-core output on a corrupted stream matches the v1.2.0 decoder
//      (golden digest), so changes to the shared loop keep single-core behaviour;
//   6. with no previous buffer and the capture starting inside a packet, decoding
//      resumes at that packet's EOP without inventing or losing packets;
//   7. a bad packet just before a boundary is counted once, as in single-core;
//   8. a stuffing violation (a run of 8 or 9 J) before a boundary, with noise filling the
//      entry window, gives the same packets and counters as single-core;
//   9. margin_words larger than the buffers does not read past them;
//  10. a bare SE0 right before a boundary leaves the decoder waiting for the gap, as in
//      single-core.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "USBPacketDecoder.h"

namespace {

// Digest of test 5, computed with the v1.2.0 decoder (commit 28f453d). Change it only
// on purpose, when single-core decoding semantics are meant to change.
constexpr uint32_t kGoldenSingleCoreNoisy = 0x36EB1F19u;

constexpr uint16_t kMargin = 48;  // SNIFFER_DUALCORE_TAIL_WORDS default

/* ── Spec-level helpers (independent of the decoder's tables) ─────────────────── */

uint8_t crc5_usb(uint16_t d11) {
    uint8_t c = 0x1F;
    for (int i = 0; i < 11; i++) {
        const uint8_t fb = (c ^ (d11 >> i)) & 1u;
        c >>= 1;
        if (fb) c ^= 0x14;
    }
    return c ^ 0x1F;
}

uint16_t crc16_usb(const uint8_t* d, size_t len) {
    uint16_t c = 0xFFFF;
    for (size_t k = 0; k < len; k++)
        for (int i = 0; i < 8; i++) {
            const uint16_t fb = (c ^ (d[k] >> i)) & 1u;
            c >>= 1;
            if (fb) c ^= 0xA001;
        }
    return c ^ 0xFFFF;
}

uint32_t fnv1a(const void* p, size_t n, uint32_t h = 2166136261u) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

uint8_t pid_byte(uint8_t pid) { return uint8_t((pid & 0x0F) | ((~pid & 0x0F) << 4)); }

/* ── What a decoded packet must look like ─────────────────────────────────────── */

struct Rec {
    uint8_t  pid;
    uint8_t  len;
    bool     crc;
    uint32_t key;  // token: 11-bit payload; data: FNV-1a of the payload; handshake: 0
    bool operator==(const Rec& o) const { return pid == o.pid && len == o.len && crc == o.crc && key == o.key; }
    bool operator<(const Rec& o) const {
        return std::tie(pid, len, crc, key) < std::tie(o.pid, o.len, o.crc, o.key);
    }
};

Rec rec_from(const USBPacket& p) {
    Rec r{ p.pid, p.data_length, p.crc_valid, 0 };
    if (p.type == USBPacketType::TOKEN)
        r.key = (p.pid == USBPID::SOF) ? p.frame_number : uint32_t(p.addr) | (uint32_t(p.endp) << 7);
    else if (p.type == USBPacketType::DATA)
        r.key = fnv1a(p.data, p.data_length);
    return r;
}

/* ── Bus model: emits symbols and records the packets a decoder must return ───── */

struct Buffers {
    uint16_t wc = 0;
    std::vector<std::unique_ptr<uint32_t[]>> buf;
    size_t size() const { return buf.size(); }
    const uint32_t* at(size_t i) const { return buf[i].get(); }
};

class Bus {
public:
    // skip: bus symbols before the capture starts; packets that start there are not expected.
    explicit Bus(bool low_speed, uint64_t skip = 0)
        : ls_(low_speed), J_(low_speed ? 0b10 : 0b01), K_(low_speed ? 0b01 : 0b10), skip_(skip) {}

    bool low_speed() const { return ls_; }
    uint64_t symbols() const { return nsym_; }
    const std::vector<Rec>& expected() const { return expect_; }

    void idle(uint64_t n) { for (uint64_t i = 0; i < n; i++) put(J_); }
    void eop() { put(0); put(0); put(J_); }
    // Raw line symbols, for corrupted streams: 'J', 'K' or '0' (SE0). Nothing is expected.
    void raw(const std::string& s) {
        for (char c : s) put(c == 'J' ? J_ : c == 'K' ? K_ : 0);
    }

    void token(uint8_t pid, uint16_t p11) {
        p11 &= 0x7FF;
        const uint16_t raw = uint16_t(p11 | (uint16_t(crc5_usb(p11)) << 11));
        const uint8_t b[3] = { pid_byte(pid), uint8_t(raw & 0xFF), uint8_t(raw >> 8) };
        if (packet(b, 3)) expect_.push_back({ pid, 0, true, p11 });
    }

    void data(uint8_t pid, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> b{ pid_byte(pid) };
        b.insert(b.end(), payload.begin(), payload.end());
        const uint16_t c = crc16_usb(payload.data(), payload.size());
        b.push_back(uint8_t(c & 0xFF));
        b.push_back(uint8_t(c >> 8));
        if (packet(b.data(), b.size()))
            expect_.push_back({ pid, uint8_t(payload.size()), true, fnv1a(payload.data(), payload.size()) });
    }

    void handshake(uint8_t pid) {
        const uint8_t b = pid_byte(pid);
        if (packet(&b, 1)) expect_.push_back({ pid, 0, true, 0 });
    }

    // Pads with idle to whole buffers, plus one idle buffer so the last real one has a successor.
    Buffers cut(uint16_t wc) {
        while (n_ != 0) put(J_);
        const uint32_t all_j = ls_ ? 0xAAAAAAAAu : 0x55555555u;
        Buffers b;
        b.wc = wc;
        const size_t nb = (words_.size() + wc - 1) / wc + 1;
        for (size_t i = 0; i < nb; i++) {
            std::unique_ptr<uint32_t[]> p(new uint32_t[wc]);
            for (size_t w = 0; w < wc; w++) {
                const size_t idx = i * wc + w;
                p[w] = idx < words_.size() ? words_[idx] : all_j;
            }
            b.buf.push_back(std::move(p));
        }
        return b;
    }

private:
    void put(uint8_t s) {
        if (nsym_++ < skip_) return;
        cur_ |= uint32_t(s) << (2 * n_);
        if (++n_ == 16) { words_.push_back(cur_); cur_ = 0; n_ = 0; }
    }

    // NRZI + bit stuffing (counted from the SYNC's last bit, USB 2.0 §7.1.9) + EOP.
    // Returns whether the packet starts inside the capture (and so must be decoded).
    bool packet(const uint8_t* bytes, size_t len) {
        const bool captured = nsym_ >= skip_;
        uint8_t level = J_;
        int ones = 0;
        auto bit = [&](int b) {
            if (b == 0) { level = (level == J_) ? K_ : J_; ones = 0; } else { ones++; }
            put(level);
            if (ones == 6) { level = (level == J_) ? K_ : J_; put(level); ones = 0; }
        };
        for (int i = 0; i < 7; i++) bit(0);
        bit(1);
        for (size_t k = 0; k < len; k++)
            for (int i = 0; i < 8; i++) bit((bytes[k] >> i) & 1);
        eop();
        return captured;
    }

    bool ls_;
    uint8_t J_, K_;
    uint64_t skip_;
    std::vector<uint32_t> words_;
    uint32_t cur_ = 0;
    int n_ = 0;
    uint64_t nsym_ = 0;
    std::vector<Rec> expect_;
};

// Host polling: IN answered by NAK or by DATA + ACK, SOF (FS) or keep-alive EOP (LS) each 1 ms.
void gen_traffic(Bus& bus, uint32_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    bus.idle(2000);
    const uint64_t frame = bus.low_speed() ? 1500 : 12000;
    uint64_t next_frame = frame;
    uint16_t frame_no = 0;
    for (uint32_t t = 0; t < n; t++) {
        if (bus.symbols() >= next_frame) {
            if (bus.low_speed()) bus.eop();
            else bus.token(USBPID::SOF, frame_no++);
            bus.idle(10 + rng() % 30);
            next_frame += frame;
        }
        bus.token(USBPID::IN, uint16_t((t * 37u) & 0x7FFu));
        bus.idle(1 + rng() % 6);  // turnaround: 2..7 J symbols, the EOP's J included
        if (rng() % 2) {
            bus.handshake(USBPID::NAK);
        } else {
            const size_t max_len = bus.low_speed() ? 8 : 64;
            const size_t len = (rng() % 3 == 0) ? max_len : rng() % (max_len + 1);
            std::vector<uint8_t> p(len);
            const unsigned pattern = rng() % 4;
            for (size_t i = 0; i < len; i++)
                p[i] = pattern == 0 ? uint8_t(i) : pattern == 1 ? 0xFF : pattern == 2 ? 0x00 : uint8_t(rng());
            for (size_t i = 0; i < len && i < 4; i++) p[i] = uint8_t(t >> (8 * i));
            bus.data((t & 1) ? USBPID::DATA1 : USBPID::DATA0, p);
            bus.idle(1 + rng() % 6);
            bus.handshake(USBPID::ACK);
        }
        bus.idle(bus.low_speed() ? 20 + rng() % 200 : 20 + rng() % 580);
    }
    bus.idle(4000);
}

/* ── Decoding the way USBSnifferPIO does ──────────────────────────────────────── */

std::vector<Rec> g_out;
void on_packet(const USBPacket& p) { g_out.push_back(rec_from(p)); }

struct Stats { uint32_t decoded, sync, crc, stuff, ovf; };

Stats stats_of(const USBPacketDecoder& d) {
    return { d.packets_decoded, d.sync_errors, d.crc_errors, d.stuffing_errors, d.overflow_errors };
}

// The only dual-core call site in this file.
void striped(USBPacketDecoder& d, const uint32_t* prev, const uint32_t* buf, uint16_t wc,
             const uint32_t* next, uint16_t margin) {
    d.feedSamplesStriped(buf, wc, next, margin, prev);
}

Stats run_single(const Buffers& b, USBSpeed sp) {
    g_out.clear();
    USBPacketDecoder d(sp);
    d.onPacket(on_packet);
    for (size_t i = 0; i < b.size(); i++) d.feedSamples(b.at(i), b.wc);
    return stats_of(d);
}

Stats run_dual(const Buffers& b, USBSpeed sp, uint16_t margin) {
    g_out.clear();
    USBPacketDecoder core1(sp), core0(sp);
    core1.onPacket(on_packet);
    core0.onPacket(on_packet);
    for (size_t i = 0; i < b.size(); i++) {
        const uint32_t* prev = (i > 0) ? b.at(i - 1) : nullptr;
        const uint32_t* next = (i + 1 < b.size()) ? b.at(i + 1) : nullptr;
        striped((i & 1) ? core0 : core1, prev, b.at(i), b.wc, next, margin);
    }
    const Stats a = stats_of(core1), z = stats_of(core0);
    return { a.decoded + z.decoded, a.sync + z.sync, a.crc + z.crc, a.stuff + z.stuff, a.ovf + z.ovf };
}

/* ── Reporting ────────────────────────────────────────────────────────────────── */

int g_failures = 0;

// Missing = expected but not returned; extra = returned but not expected (multiset difference).
void diff_counts(const std::vector<Rec>& want, const std::vector<Rec>& got, size_t* missing, size_t* extra) {
    std::vector<Rec> a(want), b(got);
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    size_t i = 0, j = 0, both = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) i++;
        else if (b[j] < a[i]) j++;
        else { both++; i++; j++; }
    }
    *missing = a.size() - both;
    *extra = b.size() - both;
}

// should_match = false is the negative control: it passes only if packets went missing.
void report(const char* name, const std::vector<Rec>& want, const Stats& s, bool should_match = true) {
    const bool same = (g_out == want);
    const bool clean = s.sync == 0 && s.crc == 0 && s.stuff == 0 && s.ovf == 0 && s.decoded == want.size();
    const bool ok = should_match ? (same && clean) : !same;
    if (!ok) g_failures++;
    printf("%s  %-46s expected %zu, got %zu", ok ? "PASS" : "FAIL", name, want.size(), g_out.size());
    if (!same) {
        size_t first = 0;
        while (first < want.size() && first < g_out.size() && g_out[first] == want[first]) first++;
        size_t missing = 0, extra = 0;
        diff_counts(want, g_out, &missing, &extra);
        printf(" (missing %zu, extra %zu, first difference at #%zu)", missing, extra, first);
    }
    printf(" | decoded=%u sync=%u crc=%u stuff=%u ovf=%u\n", s.decoded, s.sync, s.crc, s.stuff, s.ovf);
}

void print_ranges(const std::vector<long>& v) {
    size_t i = 0;
    while (i < v.size()) {
        size_t j = i;
        while (j + 1 < v.size() && v[j + 1] == v[j] + 1) j++;
        if (i == j) printf(" %ld", v[i]); else printf(" %ld..%ld", v[i], v[j]);
        i = j + 1;
    }
}

// Test 3: slide IN + DATA0(16 B) + ACK so the DATA SYNC starts at boundary + offset.
void sweep(int gap) {
    const uint16_t wc = 2048;
    std::vector<uint8_t> payload(16);
    for (size_t i = 0; i < payload.size(); i++) payload[i] = uint8_t(i);
    auto emit = [&](Bus& bus) -> uint64_t {
        bus.token(USBPID::IN, 0x123);
        bus.idle(gap - 1);
        const uint64_t data_at = bus.symbols();
        bus.data(USBPID::DATA0, payload);
        bus.idle(gap - 1);
        bus.handshake(USBPID::ACK);
        return data_at;
    };
    Bus probe(false);
    const long data_off = long(emit(probe));
    const long boundary = long(wc) * 16;

    std::vector<long> bad_single, bad_dual;
    for (long off = -800; off <= 160; off++) {
        Bus bus(false);
        bus.idle(uint64_t(boundary + off - data_off));
        emit(bus);
        const Buffers b = bus.cut(wc);
        Stats s = run_single(b, USBSpeed::FULL_SPEED);
        if (!(g_out == bus.expected()) || s.sync || s.crc || s.stuff || s.ovf) bad_single.push_back(off);
        s = run_dual(b, USBSpeed::FULL_SPEED, kMargin);
        if (!(g_out == bus.expected()) || s.sync || s.crc || s.stuff || s.ovf) bad_dual.push_back(off);
    }
    const bool ok = bad_single.empty() && bad_dual.empty();
    if (!ok) g_failures++;
    printf("%s  boundary sweep, gap of %d J, offsets -800..160", ok ? "PASS" : "FAIL", gap);
    if (!bad_single.empty()) { printf(" | single-core wrong at"); print_ranges(bad_single); }
    if (!bad_dual.empty()) { printf(" | dual-core wrong at"); print_ranges(bad_dual); }
    printf("\n");
}

// Test 6: no previous buffer, and the capture starts inside a 64 x 0xFF DATA packet (full of
// 7-symbol runs, one short of the 8 that prove the bus is idle). Decoding must resume at that
// packet's EOP: nothing invented before it, nothing lost after it.
void start_inside_packet() {
    const std::vector<uint8_t> ff(64, 0xFF);
    auto emit = [&](Bus& bus, uint64_t* data_end) -> uint64_t {
        bus.idle(100);
        bus.token(USBPID::IN, 0x055);
        bus.idle(2);
        const uint64_t data_at = bus.symbols();
        bus.data(USBPID::DATA1, ff);
        *data_end = bus.symbols();
        bus.idle(2);
        bus.handshake(USBPID::ACK);
        bus.idle(300);
        bus.token(USBPID::IN, 0x055);
        bus.idle(3);
        bus.handshake(USBPID::NAK);
        bus.idle(500);
        return data_at;
    };
    uint64_t data_end = 0;
    Bus probe(false);
    const uint64_t data_at = emit(probe, &data_end);

    // From the second symbol of the SYNC to the second SE0 of the EOP.
    std::vector<long> bad;
    for (uint64_t start = data_at + 1; start + 1 < data_end; start++) {
        Bus bus(false, start);
        uint64_t unused = 0;
        emit(bus, &unused);
        const Buffers b = bus.cut(2048);
        const Stats s = run_dual(b, USBSpeed::FULL_SPEED, kMargin);
        if (!(g_out == bus.expected()) || s.sync || s.crc || s.stuff || s.ovf) bad.push_back(long(start - data_at));
    }
    const bool ok = bad.empty();
    if (!ok) g_failures++;
    printf("%s  capture starting inside a 64 x 0xFF packet, no previous buffer (%llu offsets)",
           ok ? "PASS" : "FAIL", (unsigned long long)(data_end - data_at - 2));
    if (!ok) { printf(" | wrong at offsets"); print_ranges(bad); }
    printf("\n");
}

// Test 7: a packet with a bit-stuffing violation ends just before a boundary, inside the words
// the next buffer re-reads on entry. Its errors belong to the buffer it is in: dual-core must
// count exactly what single-core counts. (v1.2.0 passes too: this guards the counter restore
// of the entry phase, it does not reproduce a v1.2.0 defect.)
void error_before_boundary() {
    const uint16_t wc = 2048;
    const std::vector<uint8_t> ff(64, 0xFF);
    const uint64_t boundary = uint64_t(wc) * 16;
    Bus bus(false);
    bus.idle(boundary - 700);
    const uint64_t data_at = bus.symbols();
    bus.data(USBPID::DATA1, ff);
    bus.idle(2);
    bus.handshake(USBPID::ACK);
    bus.idle(3000);
    Buffers b = bus.cut(wc);

    // SYNC (8) + PID 0x4B (8) + six 1s: symbol 22 is the stuffed transition. Repeating the
    // previous level there turns it into a seventh 1, which the decoder must reject.
    const uint64_t pos = data_at + 22;
    uint32_t& word = b.buf[size_t(pos / 16 / wc)][size_t((pos / 16) % wc)];
    const unsigned s = unsigned(pos % 16);
    const uint32_t prev = (s > 0) ? (word >> (2 * (s - 1))) & 3u
                                  : (b.buf[size_t((pos - 1) / 16 / wc)][size_t(((pos - 1) / 16) % wc)] >> 30) & 3u;
    word = (word & ~(3u << (2 * s))) | (prev << (2 * s));

    const Stats single = run_single(b, USBSpeed::FULL_SPEED);
    const std::vector<Rec> single_out = g_out;
    const Stats dual = run_dual(b, USBSpeed::FULL_SPEED, kMargin);
    const bool counted = single.stuff >= 1;  // the corruption took effect
    const bool same = g_out == single_out && dual.decoded == single.decoded && dual.sync == single.sync
                   && dual.crc == single.crc && dual.stuff == single.stuff && dual.ovf == single.ovf;
    const bool ok = counted && same;
    if (!ok) g_failures++;
    printf("%s  error just before a boundary counted once: single stuff=%u sync=%u | dual stuff=%u sync=%u\n",
           ok ? "PASS" : "FAIL", single.stuff, single.sync, dual.stuff, dual.sync);
}

// Test 8: noise fills the whole entry window (no SE0, no long J run), then a packet whose
// bit-stuffing violation is a run of 8 J (or 9 when its SYNC ends in JJ), then a valid packet
// that starts 2 symbols after the violation and contains the boundary. A run that short is not a
// point where every state has converged: after the violation the decoder still waits for the
// inter-packet gap. Dual-core must match single-core, packets and counters.
void violation_before_boundary(bool sync_ends_jj) {
    const uint16_t wc = 2048;
    const uint64_t boundary = uint64_t(wc) * 16;
    const std::vector<uint8_t> zeros(64, 0x00);
    std::string noise;
    for (int i = 0; i < 400; i++) noise += "KJ";  // 800 symbols: ends in IDLE with the gap flag set
    std::string body;
    for (int i = 0; i < 40; i++) body += "JK";    // 10 bytes of 0x00
    auto emit = [&](Bus& bus, uint64_t lead_idle) -> uint64_t {
        bus.idle(lead_idle);
        bus.raw(noise);
        bus.raw("JJJ");
        if (sync_ends_jj) {
            bus.raw("KJKJKJJ");    // SYNC accepted at its 7th symbol
            bus.raw("JJJJJJJ");    // six 1s, then the 9th J of the run is a stuffing error
        } else {
            bus.raw("KJKJKJKK");
            bus.raw(body);
            bus.raw("JJJJJJJJ");   // transition + six 1s, then the 8th J is a stuffing error
        }
        const uint64_t after = bus.symbols();
        bus.raw("KJ");
        bus.data(USBPID::DATA0, zeros);
        bus.idle(300);
        bus.token(USBPID::IN, 0x055);
        bus.idle(3);
        bus.handshake(USBPID::NAK);
        bus.idle(2000);
        return after;
    };
    Bus probe(false);
    const uint64_t after_probe = emit(probe, 1000);
    // Put the valid packet's SYNC (after + 2) 100 symbols before the boundary.
    const uint64_t lead_idle = 1000 + (boundary - 102) - after_probe;
    Bus bus(false);
    emit(bus, lead_idle);
    const Buffers b = bus.cut(wc);
    const Stats single = run_single(b, USBSpeed::FULL_SPEED);
    const std::vector<Rec> single_out = g_out;
    const Stats dual = run_dual(b, USBSpeed::FULL_SPEED, kMargin);
    // The scenario is what it claims: single-core sees the violation, the noise and every valid packet.
    const bool scenario = single.stuff == 1 && single.sync > 0 && single_out == bus.expected();
    const bool same = g_out == single_out && dual.decoded == single.decoded && dual.sync == single.sync
                   && dual.crc == single.crc && dual.stuff == single.stuff && dual.ovf == single.ovf;
    const bool ok = scenario && same;
    if (!ok) g_failures++;
    printf("%s  stuffing violation (%s J run) before a boundary, noise-filled entry: single sync=%u stuff=%u"
           " pkts=%zu | dual sync=%u stuff=%u pkts=%zu\n",
           ok ? "PASS" : "FAIL", sync_ends_jj ? "9" : "8", single.sync, single.stuff, single_out.size(),
           dual.sync, dual.stuff, g_out.size());
}

// Test 9: margin_words larger than the buffers is allowed by the decoder API (the library
// rules it out with a static_assert); decoding must stay inside every buffer (ASan checks).
void margin_larger_than_buffer() {
    Bus bus(false);
    gen_traffic(bus, 50, 31);
    const Buffers b = bus.cut(16);
    run_dual(b, USBSpeed::FULL_SPEED, kMargin);
    printf("PASS  margin_words larger than 16-word buffers stays inside them\n");
}

// Test 10: a bare SE0 (no J after it) right before a boundary, with noise filling the entry
// window, then a valid packet whose SYNC starts 3 symbols after the SE0. After an SE0 the decoder
// waits for a J before it accepts a SYNC, so the K right after the SE0 starts nothing, and the
// packet belongs to the buffer after the boundary. Dual-core must match single-core.
void bare_se0_before_boundary() {
    const uint16_t wc = 2048;
    const uint64_t boundary = uint64_t(wc) * 16;
    const std::vector<uint8_t> zeros(16, 0x00);
    std::string noise;
    for (int i = 0; i < 400; i++) noise += "KJ";  // ends in IDLE with the gap flag set
    auto emit = [&](Bus& bus, uint64_t lead_idle) -> uint64_t {
        bus.idle(lead_idle);
        bus.raw(noise);
        const uint64_t se0_at = bus.symbols();
        bus.raw("0KJ");                  // SE0, a K that starts nothing, then the J gap
        bus.data(USBPID::DATA0, zeros);  // SYNC starts right after that J
        bus.idle(300);
        bus.token(USBPID::IN, 0x055);
        bus.idle(3);
        bus.handshake(USBPID::NAK);
        bus.idle(2000);
        return se0_at;
    };
    Bus probe(false);
    const uint64_t se0_probe = emit(probe, 1000);
    // SE0 at boundary - 2, K at boundary - 1, J at the boundary, SYNC from boundary + 1.
    const uint64_t lead_idle = 1000 + (boundary - 2) - se0_probe;
    Bus bus(false);
    emit(bus, lead_idle);
    const Buffers b = bus.cut(wc);
    const Stats single = run_single(b, USBSpeed::FULL_SPEED);
    const std::vector<Rec> single_out = g_out;
    const Stats dual = run_dual(b, USBSpeed::FULL_SPEED, kMargin);
    const bool scenario = single.sync > 0 && single_out == bus.expected();
    const bool same = g_out == single_out && dual.decoded == single.decoded && dual.sync == single.sync
                   && dual.crc == single.crc && dual.stuff == single.stuff && dual.ovf == single.ovf;
    const bool ok = scenario && same;
    if (!ok) g_failures++;
    printf("%s  bare SE0 right before a boundary, noise-filled entry: single pkts=%zu sync=%u | dual pkts=%zu sync=%u\n",
           ok ? "PASS" : "FAIL", single_out.size(), single.sync, g_out.size(), dual.sync);
}

// Test 5: single-core decoding of a corrupted stream, reduced to one digest.
uint32_t noisy_single_core_digest() {
    Bus bus(false);
    gen_traffic(bus, 8000, 99);
    Buffers b = bus.cut(2048);
    std::mt19937 rng(4242);
    for (size_t i = 0; i < b.size(); i++)
        for (size_t w = 0; w < b.wc; w++)
            if (rng() % 32 == 0) {
                const unsigned s = rng() % 16;
                b.buf[i][w] ^= (1u + rng() % 3) << (2 * s);
            }
    const Stats s = run_single(b, USBSpeed::FULL_SPEED);
    uint32_t h = 2166136261u;
    for (const Rec& r : g_out) {
        h = fnv1a(&r.pid, 1, h);
        h = fnv1a(&r.len, 1, h);
        const uint8_t crc = r.crc ? 1 : 0;
        h = fnv1a(&crc, 1, h);
        h = fnv1a(&r.key, sizeof r.key, h);
    }
    const uint32_t counters[5] = { s.decoded, s.sync, s.crc, s.stuff, s.ovf };
    return fnv1a(counters, sizeof counters, h);
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);  // keep every line if a sanitizer aborts mid-run
    printf("USBPacketDecoder host test (margin %u words)\n", unsigned(kMargin));
    {
        Bus bus(false);
        gen_traffic(bus, 30000, 20261125);
        const Buffers b = bus.cut(2048);
        report("full-speed, single-core, 2048-word buffers", bus.expected(), run_single(b, USBSpeed::FULL_SPEED));
        report("full-speed, dual-core, 2048-word buffers", bus.expected(), run_dual(b, USBSpeed::FULL_SPEED, kMargin));
        report("negative control: dual-core with margin 0", bus.expected(), run_dual(b, USBSpeed::FULL_SPEED, 0), false);
    }
    {
        Bus bus(false);
        gen_traffic(bus, 30000, 777);
        const Buffers b = bus.cut(1024);
        report("full-speed, single-core, 1024-word buffers", bus.expected(), run_single(b, USBSpeed::FULL_SPEED));
        report("full-speed, dual-core, 1024-word buffers", bus.expected(), run_dual(b, USBSpeed::FULL_SPEED, kMargin));
    }
    {
        Bus bus(true);
        gen_traffic(bus, 30000, 4711);
        const Buffers b = bus.cut(2048);
        report("low-speed, single-core, 2048-word buffers", bus.expected(), run_single(b, USBSpeed::LOW_SPEED));
        report("low-speed, dual-core, 2048-word buffers", bus.expected(), run_dual(b, USBSpeed::LOW_SPEED, kMargin));
    }
    for (int gap : { 2, 4, 7 }) sweep(gap);
    start_inside_packet();
    error_before_boundary();
    violation_before_boundary(false);
    violation_before_boundary(true);
    margin_larger_than_buffer();
    bare_se0_before_boundary();
    {
        // The four-argument form from v1.2.0 must still compile and run.
        Bus bus(false);
        gen_traffic(bus, 200, 5);
        const Buffers b = bus.cut(2048);
        USBPacketDecoder d(USBSpeed::FULL_SPEED);
        d.feedSamplesStriped(b.at(0), b.wc, b.size() > 1 ? b.at(1) : nullptr, kMargin);
        printf("PASS  four-argument feedSamplesStriped() still compiles and runs\n");
    }
    {
        const uint32_t digest = noisy_single_core_digest();
        const bool ok = digest == kGoldenSingleCoreNoisy;
        if (!ok) g_failures++;
        printf("%s  single-core on a corrupted stream matches v1.2.0: digest 0x%08X, golden 0x%08X\n",
               ok ? "PASS" : "FAIL", digest, kGoldenSingleCoreNoisy);
    }
    printf("%s: %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
