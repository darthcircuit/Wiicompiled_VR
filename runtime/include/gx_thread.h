#pragma once
// GX thread: the host side of the GX pipeline (state tracking, FIFO parsing,
// display-list scanning, texture object resolution and every aurora GX call)
// runs on its own thread, fed by an ordered command ring the game thread posts
// to. Each GX HLE override is split into a game-thread front (guest-visible
// side effects: shadow registers, getters, display-list recording) and a
// GX-thread back (the aurora work). When the thread is disabled, Post() runs
// the back inline, so the split is behaviour-preserving in both modes.
//
// Data hazards follow the hardware: anything the GX library copies into the
// FIFO at call time (immediate-mode vertices, matrices, colours, light objects,
// texture object registers) is snapshotted at post time; anything the GP reads
// from memory when it reaches the command (display lists, vertex arrays,
// indexed matrices, texture data) is read when the GX thread executes it.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace GxThread {

// Decides whether posted work runs on the GX thread. Read once, before GXInit.
void Configure(bool enabled);
bool Enabled() noexcept;
// True on the consumer thread.
bool IsGxThread() noexcept;
void Start();
// Executes everything posted so far and joins the thread.
void Stop();
// Game thread: blocks until every posted record has executed (GXDrawDone).
void Drain();
// Publishes the open immediate-mode FIFO chunk. Post() does this itself.
void FlushFifo();
// Invoked at bounded intervals while the game thread blocks in Drain() or on
// a full ring, so guest timing (VI retraces, OS alarms) keeps running.
void SetWaitCallback(void (*callback)());
// Native thread id of the consumer (Android: gettid), 0 until started.
uint32_t NativeThreadId() noexcept;
// One line for the frame-rate log; resets the window counters.
std::string FormatStatsAndReset(double windowSeconds, uint32_t frames);

// Immediate-mode write-gather bytes. Only valid when Enabled().
void PostFifoWord(uint32_t value, uint32_t sizeBytes);
void PostFifoBytes(const uint8_t* data, uint32_t sizeBytes);

namespace detail {
using Invoke = void (*)(const uint8_t* payload, uint32_t payloadBytes);
void PostRecord(Invoke invoke, const void* payload, uint32_t payloadBytes);

template <typename... Ts>
struct Pack;
template <>
struct Pack<> {
    template <typename F, typename... Prev>
    void Call(F f, Prev... prev) const {
        f(prev...);
    }
};
template <typename T, typename... Ts>
struct Pack<T, Ts...> {
    T head;
    Pack<Ts...> tail;
    template <typename F, typename... Prev>
    void Call(F f, Prev... prev) const {
        tail.Call(f, prev..., head);
    }
};
template <typename... Ts>
struct BuildPack;
template <>
struct BuildPack<> {
    static Pack<> Make() { return {}; }
};
template <typename T, typename... Ts>
struct BuildPack<T, Ts...> {
    static Pack<T, Ts...> Make(T head, Ts... tail) {
        return Pack<T, Ts...>{head, BuildPack<Ts...>::Make(tail...)};
    }
};
template <typename R, typename... Params>
struct CallRecord {
    R (*fn)(Params...);
    Pack<Params...> args;
};
template <typename R, typename... Params>
void InvokeCall(const uint8_t* payload, uint32_t) {
    CallRecord<R, Params...> record;
    std::memcpy(&record, payload, sizeof(record));
    record.args.Call(record.fn);
}
} // namespace detail

// Posts fn(args...) to the GX thread, or runs it now when the thread is off.
// Parameters must be trivially copyable values (no pointers into guest memory
// that the game may rewrite before the GX thread reads them).
template <typename R, typename... Params, typename... Args>
inline void Post(R (*fn)(Params...), Args&&... args) {
    if (!Enabled()) {
        fn(static_cast<Params>(args)...);
        return;
    }
    detail::CallRecord<R, Params...> record{fn, detail::BuildPack<Params...>::Make(static_cast<Params>(args)...)};
    detail::PostRecord(&detail::InvokeCall<R, Params...>, &record, sizeof(record));
}

} // namespace GxThread
