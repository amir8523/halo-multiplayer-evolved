// SPDX-License-Identifier: MIT
// MultiplayerEvolved: Unreal/GameThread.cpp
#define MPE_LOG_CATEGORY "Unreal.GameThread"

#include "Unreal/GameThread.h"

#include "Core/Log.h"
#include "Debug/AccessTrap.h"
#include "Unreal/FNameTrampoline.h"
#include "Unreal/ProcessMemory.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace mpe::unreal {
namespace {

/// ProcessEvent is a substantial function. Anything far outside this range is something
/// else that happens to be shared, such as a destructor or a trivial accessor.
constexpr std::size_t kMinProcessEventSize = 600;
constexpr std::size_t kMaxProcessEventSize = 16384;

/// How many virtual table slots to compare. UObject's own portion is well inside this.
constexpr int kSlotsToCompare = 96;

std::mutex g_mutex;
CallLayout g_layout;
bool       g_detected = false;

// --- The pending game thread job -------------------------------------------
//
// The job is owned here rather than pointed at on a caller's stack.
//
// An earlier version stored a pointer to a std::function local to the calling frame. That
// frame is alive while the caller blocks, but not after a timeout: the caller returned, the
// frame died, and a game thread already inside the handler then called through the dangling
// pointer and faulted on a stack address. Owning the job removes the dangling pointer, and
// the active count below removes the race that let a handler still be running when it was
// replaced.
std::function<void()> g_job;
std::atomic<bool>     g_job_waiting{false};
std::atomic<bool>     g_job_done{false};
std::atomic<int>      g_handlers_active{0};

/// Blocks until no exception handler is inside the job.
///
/// Disarming is not instantaneous: it walks every thread, and one can already be past the
/// check by then. Nothing may replace or destroy the job until they have all left.
void DrainHandlers() {
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (g_handlers_active.load(std::memory_order_acquire) == 0) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

/// Reads the size of the function containing an address, from the exception directory.
///
/// Every x64 image describes its functions there, so this gives exact bounds instead of
/// inferring them from padding.
[[nodiscard]] std::size_t FunctionSize(std::uintptr_t base, std::uintptr_t rva) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (directory.VirtualAddress == 0 || directory.Size == 0) {
        return 0;
    }

    const auto* functions =
        reinterpret_cast<const RUNTIME_FUNCTION*>(base + directory.VirtualAddress);
    const std::size_t count = directory.Size / sizeof(RUNTIME_FUNCTION);

    std::size_t low = 0;
    std::size_t high = count;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        RUNTIME_FUNCTION entry{};
        if (!memory::GuardedRead(reinterpret_cast<std::uintptr_t>(&functions[middle]), &entry,
                                 sizeof(entry))) {
            return 0;
        }
        if (rva < entry.BeginAddress) {
            high = middle;
        } else if (rva >= entry.EndAddress) {
            low = middle + 1;
        } else {
            return entry.EndAddress - entry.BeginAddress;
        }
    }
    return 0;
}

/// Reads an object's virtual table pointer.
[[nodiscard]] std::uintptr_t VTableOf(std::uintptr_t object) {
    std::uintptr_t table = 0;
    if (!memory::GuardedRead(object, &table, sizeof(table))) {
        return 0;
    }
    return memory::IsPlausiblePointer(table) ? table : 0;
}

/// Picks live instances of several distinct classes.
///
/// Distinct classes matter: a slot holding the same function in unrelated classes was
/// inherited rather than overridden, and only inherited slots can be UObject's own.
[[nodiscard]] std::vector<std::uintptr_t> SampleInstances(const ObjectArray& objects,
                                                          std::size_t wanted) {
    std::vector<std::uintptr_t> chosen;
    std::vector<std::string>    seen_classes;

    objects.ForEach([&](const ObjectInfo& object) {
        if (chosen.size() >= wanted) {
            return false;
        }
        // Classes and defaults do not share instance layout, so they are poor samples.
        if (object.class_name == "Class" || object.class_name == "ScriptStruct" ||
            object.class_name == "Function" || object.class_name == "Package" ||
            object.name.rfind("Default__", 0) == 0) {
            return true;
        }
        for (const std::string& already : seen_classes) {
            if (already == object.class_name) {
                return true;
            }
        }
        if (VTableOf(object.address) == 0) {
            return true;
        }
        seen_classes.push_back(object.class_name);
        chosen.push_back(object.address);
        return true;
    });

    return chosen;
}

/// The trampoline the exception handler calls. Runs on a game thread.
bool OnGameThreadHit(std::uintptr_t) {
    // The count is taken before the check and released at the end, so a waiter can tell
    // whether any thread is still inside.
    g_handlers_active.fetch_add(1, std::memory_order_acq_rel);

    // Claim the job so a second thread hitting the same breakpoint cannot run it twice.
    if (g_job_waiting.exchange(false, std::memory_order_acq_rel)) {
        if (g_job) {
            g_job();
        }
        g_job_done.store(true, std::memory_order_release);
    }

    g_handlers_active.fetch_sub(1, std::memory_order_acq_rel);

    // Once the job is claimed there is nothing left for any thread to do here, so each one
    // disarms itself as it arrives rather than continuing to trap on every call.
    return false;
}

} // namespace

Result DetectCallLayout(const ObjectArray& objects, CallLayout& out_layout) {
    std::lock_guard lock(g_mutex);
    if (g_detected) {
        out_layout = g_layout;
        return Result::Success();
    }

    const auto base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));

    // Enough unrelated classes that an override cannot masquerade as an inherited slot.
    //
    // Three was too few. Detection ran earlier than before as an optimisation, at a point
    // where only four distinct classes existed, and with that little to compare it accepted
    // a class override in slot 49 rather than the real UObject virtual in slot 79. The
    // dispatch then armed on a function nothing ever calls, so every game thread job timed
    // out and the menu entry silently never appeared.
    //
    // The cost of waiting is a moment; the cost of guessing is a feature that does nothing.
    constexpr std::size_t kRequiredSamples = 6;

    const std::vector<std::uintptr_t> samples = SampleInstances(objects, kRequiredSamples);
    if (samples.size() < kRequiredSamples) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("only {} distinct class(es) available; detection "
                                        "needs {} to tell an inherited virtual from an "
                                        "override",
                                        samples.size(), kRequiredSamples));
    }

    // Collect each slot's pointer for every sample.
    std::vector<std::vector<std::uintptr_t>> tables;
    tables.reserve(samples.size());
    for (const std::uintptr_t instance : samples) {
        const std::uintptr_t table = VTableOf(instance);
        std::vector<std::uintptr_t> entries(kSlotsToCompare, 0);
        for (int slot = 0; slot < kSlotsToCompare; ++slot) {
            std::uintptr_t entry = 0;
            if (!memory::GuardedRead(table + static_cast<std::uintptr_t>(slot) * 8, &entry,
                                     sizeof(entry))) {
                break;
            }
            entries[static_cast<std::size_t>(slot)] = entry;
        }
        tables.push_back(std::move(entries));
    }

    // A slot is inherited when every sample agrees on it.
    int            best_slot = -1;
    std::uintptr_t best_address = 0;
    std::size_t    best_size = 0;

    for (int slot = 0; slot < kSlotsToCompare; ++slot) {
        const std::uintptr_t candidate = tables.front()[static_cast<std::size_t>(slot)];
        if (candidate == 0 || !memory::IsPlausiblePointer(candidate)) {
            continue;
        }
        bool shared = true;
        for (const std::vector<std::uintptr_t>& table : tables) {
            if (table[static_cast<std::size_t>(slot)] != candidate) {
                shared = false;
                break;
            }
        }
        if (!shared) {
            continue;
        }

        const std::size_t size = FunctionSize(base, candidate - base);
        if (size < kMinProcessEventSize || size > kMaxProcessEventSize) {
            continue;
        }
        if (size > best_size) {
            best_size    = size;
            best_slot    = slot;
            best_address = candidate;
        }
    }

    if (best_slot < 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "no shared virtual table slot held a function of a plausible "
                            "size for ProcessEvent");
    }

    g_layout.process_event     = best_address;
    g_layout.process_event_rva = best_address - base;
    g_layout.process_event_size = best_size;
    g_layout.vtable_slot       = best_slot;
    g_layout.dispatch_anchor   = best_address;
    g_detected                 = true;
    out_layout                 = g_layout;

    MPE_LOG_INFO("ProcessEvent detected in slot {} at 0x{:X} (RVA 0x{:X}, {} bytes), shared "
                "across {} unrelated class(es)",
                best_slot, best_address, g_layout.process_event_rva, best_size,
                samples.size());
    return Result::Success();
}

Result RunOnGameThread(const std::function<void()>& job, unsigned timeout_milliseconds) {
    if (!job) {
        return Result::Fail(ErrorCode::InvalidArgument, "no job given");
    }

    std::uintptr_t anchor = 0;
    {
        std::lock_guard lock(g_mutex);
        if (!g_detected) {
            return Result::Fail(ErrorCode::InvalidState,
                                "the call layout has not been detected yet");
        }
        anchor = g_layout.dispatch_anchor;
    }

    // Nothing may replace the job while a handler could still be running the previous one.
    DrainHandlers();

    // One job at a time. The handler reads these without a lock, so publishing order
    // matters: everything it will read is in place before the job can be picked up.
    g_job = job;
    g_job_done.store(false, std::memory_order_release);
    g_job_waiting.store(true, std::memory_order_release);

    // With a pump installed the job runs on the widget's next event, which for anything on
    // screen is the next frame. No threads are suspended and no exceptions are raised, so
    // the whole thing costs a frame rather than several seconds.
    if (GameThreadPumpActive()) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeout_milliseconds);
        while (!g_job_done.load(std::memory_order_acquire)) {
            if (std::chrono::steady_clock::now() > deadline) {
                g_job_waiting.store(false, std::memory_order_release);
                DrainHandlers();
                return Result::Fail(ErrorCode::Timeout,
                                    "the pump widget stopped receiving events");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        DrainHandlers();
        return Result::Success();
    }

    debugtrap::SetHitCallback(&OnGameThreadHit);

    const Result armed = debugtrap::Arm(anchor, 1, debugtrap::Condition::Execute,
                                        "game thread dispatch");
    if (!armed.ok()) {
        g_job_waiting.store(false, std::memory_order_release);
        debugtrap::SetHitCallback(nullptr);
        DrainHandlers();
        return armed;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_milliseconds);
    while (!g_job_done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > deadline) {
            g_job_waiting.store(false, std::memory_order_release);
            debugtrap::DisarmAll();
            debugtrap::SetHitCallback(nullptr);
            // Draining before returning is what makes it safe for a caller to capture its
            // own locals by reference: once this returns, no handler is inside the job.
            DrainHandlers();
            return Result::Fail(ErrorCode::Timeout,
                                "the game thread never reached the dispatch point; the game "
                                "may be paused, loading, or without a world");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    debugtrap::DisarmAll();
    debugtrap::SetHitCallback(nullptr);
    DrainHandlers();
    return Result::Success();
}

std::uintptr_t FindFunction(const ObjectArray& objects, std::string_view name,
                            std::string_view owner_class) {
    std::uintptr_t found = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.class_name != "Function" || object.name != name) {
            return true;
        }
        if (!owner_class.empty()) {
            // The owner is the object's outer, so the path carries it.
            const std::string path = objects.BuildPath(object);
            if (path.find(owner_class) == std::string::npos) {
                return true;
            }
        }
        found = object.address;
        return false;
    });
    return found;
}

Result CallFunction(std::uintptr_t object, std::uintptr_t function, void* parameters) {
    std::uintptr_t process_event = 0;
    {
        std::lock_guard lock(g_mutex);
        if (!g_detected) {
            return Result::Fail(ErrorCode::InvalidState, "call layout not detected");
        }
        process_event = g_layout.process_event;
    }
    if (object == 0 || function == 0) {
        return Result::Fail(ErrorCode::InvalidArgument, "object or function is null");
    }

    using ProcessEventFn = void(__fastcall*)(void*, void*, void*);
    const auto call = reinterpret_cast<ProcessEventFn>(process_event);
    call(reinterpret_cast<void*>(object), reinterpret_cast<void*>(function), parameters);
    return Result::Success();
}

namespace {

/// An FString as Unreal lays it out: a buffer, how many characters are used including the
/// terminator, and how many it can hold.
struct FStringLayout {
    wchar_t*     data;
    std::int32_t count;
    std::int32_t capacity;
};

/// Finds a live player controller, which is what owns the travel functions.
///
/// Matching on the name containing "PlayerController" is not enough, and getting this wrong
/// is fatal rather than merely useless: an earlier version selected
/// BlamNetworkPlayerControllerComponent, a component that merely has the word in its type,
/// and calling travel on it aborted the process with
/// "Failed to find function ClientTravelInternal".
///
/// Two rules follow from that. Components are excluded outright. And a gameplay controller
/// is preferred over the frontend one, because the frontend controller exists at the menu
/// where travel has nowhere to go.
///
/// allow_frontend distinguishes the two uses. Travel must refuse the frontend controller,
/// because travelling with no world loaded is what crashes. Creating a widget is the
/// opposite case: at the menu the frontend controller is the only one there is, and it is
/// the correct owner for a menu widget.
[[nodiscard]] std::uintptr_t FindPlayerController(const ObjectArray& objects,
                                                  bool allow_frontend = false) {
    std::uintptr_t gameplay = 0;
    std::uintptr_t frontend = 0;
    std::string    gameplay_class;
    std::string    frontend_class;

    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name.rfind("Default__", 0) == 0) {
            return true;
        }
        // A component is never a controller, whatever its type is called.
        if (object.class_name.find("Component") != std::string::npos ||
            object.name.find("_GEN_VARIABLE") != std::string::npos) {
            return true;
        }
        if (object.class_name.find("PlayerController") == std::string::npos) {
            return true;
        }
        if (object.class_name.find("Frontend") != std::string::npos ||
            object.name.find("Frontend") != std::string::npos) {
            if (frontend == 0) {
                frontend       = object.address;
                frontend_class = object.class_name;
            }
            return true;
        }
        gameplay       = object.address;
        gameplay_class = object.class_name;
        return false;
    });

    if (gameplay != 0) {
        MPE_LOG_INFO("using gameplay player controller {} at 0x{:X}", gameplay_class,
                    gameplay);
        return gameplay;
    }
    if (frontend != 0 && allow_frontend) {
        MPE_LOG_INFO("using frontend controller {} at 0x{:X}", frontend_class, frontend);
        return frontend;
    }
    if (frontend != 0) {
        MPE_LOG_WARN("only the frontend controller {} at 0x{:X} exists, which means no world "
                    "is loaded; travelling from here is what crashes",
                    frontend_class, frontend);
    }
    return 0;
}

} // namespace

Result Travel(const ObjectArray& objects, std::string_view url, std::uint8_t travel_type) {
    const std::uintptr_t controller = FindPlayerController(objects);
    if (controller == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "no live player controller; a world has to exist first");
    }

    const std::uintptr_t function = FindFunction(objects, "ClientTravel");
    if (function == 0) {
        return Result::Fail(ErrorCode::InvalidState, "ClientTravel was not found");
    }

    // Layout read from the function's own reflected properties rather than assumed:
    //   +0x00 URL (FString)  +0x10 TravelType (byte)  +0x11 bSeamless  +0x14 MapPackageGuid
    struct Parameters {
        FStringLayout url;             // +0x00
        std::uint8_t  travel_type;     // +0x10
        std::uint8_t  seamless;        // +0x11
        std::uint8_t  padding[2];      // +0x12
        std::uint8_t  map_package_guid[16]; // +0x14
    };
    static_assert(offsetof(Parameters, travel_type) == 0x10, "URL must occupy 0x00..0x0F");
    static_assert(offsetof(Parameters, map_package_guid) == 0x14, "guid must sit at 0x14");

    std::wstring text;
    text.reserve(url.size() + 1);
    for (const char character : url) {
        text.push_back(static_cast<wchar_t>(character));
    }

    Parameters parameters{};
    parameters.url.data     = text.data();
    parameters.url.count    = static_cast<std::int32_t>(text.size() + 1);
    parameters.url.capacity = parameters.url.count;
    parameters.travel_type  = travel_type;
    parameters.seamless     = 0;

    MPE_LOG_INFO("travelling to '{}' (type {}) via controller 0x{:X}", url, travel_type,
                controller);
    const Result called = CallFunction(controller, function, &parameters);
    if (!called.ok()) {
        return called;
    }
    MPE_LOG_INFO("ClientTravel returned");
    return Result::Success();
}

namespace {

/// Finds a live instance of a class by exact name.
[[nodiscard]] std::uintptr_t FindInstanceOfClass(const ObjectArray& objects,
                                                 std::string_view class_name) {
    std::uintptr_t found = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name.rfind("Default__", 0) == 0 || object.class_name != class_name) {
            return true;
        }
        found = object.address;
        return false;
    });
    return found;
}

} // namespace

Result ShowWidget(const ObjectArray& objects, std::string_view widget_class,
                  std::uintptr_t& out_widget) {
    // The class object to instantiate. Widget blueprint classes are objects in their own
    // right, so this is a lookup rather than anything exotic.
    std::uintptr_t widget_type = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name != widget_class) {
            return true;
        }
        if (object.class_name.find("Class") == std::string::npos) {
            return true;
        }
        widget_type = object.address;
        return false;
    });
    if (widget_type == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("no widget class named '{}'", widget_class));
    }

    const std::uintptr_t controller = FindPlayerController(objects, /*allow_frontend=*/true);
    if (controller == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "no player controller to own the widget");
    }

    // Create is a static on UWidgetBlueprintLibrary. A static still needs an object to
    // dispatch through, and the class default object is the conventional choice.
    const std::uintptr_t create = FindFunction(objects, "Create");
    if (create == 0) {
        return Result::Fail(ErrorCode::InvalidState, "Create was not found");
    }
    std::uintptr_t library = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name != "Default__WidgetBlueprintLibrary") {
            return true;
        }
        library = object.address;
        return false;
    });
    if (library == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "Default__WidgetBlueprintLibrary was not found");
    }

    //   +0x00 WorldContextObject  +0x08 WidgetType  +0x10 OwningPlayer  +0x18 ReturnValue
    struct CreateParameters {
        std::uintptr_t world_context;
        std::uintptr_t widget_type;
        std::uintptr_t owning_player;
        std::uintptr_t return_value;
    };
    CreateParameters create_parameters{};
    create_parameters.world_context = controller;
    create_parameters.widget_type   = widget_type;
    create_parameters.owning_player = controller;

    MPE_LOG_INFO("creating widget '{}' (class 0x{:X}) for controller 0x{:X}", widget_class,
                widget_type, controller);
    if (const Result called = CallFunction(library, create, &create_parameters);
        !called.ok()) {
        return called;
    }
    if (create_parameters.return_value == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "Create returned nothing; the class is probably not a user "
                            "widget");
    }

    const std::uintptr_t add = FindFunction(objects, "AddToViewport");
    if (add == 0) {
        return Result::Fail(ErrorCode::InvalidState, "AddToViewport was not found");
    }
    struct AddParameters {
        std::int32_t z_order;
        std::int32_t padding;
    };
    AddParameters add_parameters{};
    if (const Result called = CallFunction(create_parameters.return_value, add,
                                           &add_parameters);
        !called.ok()) {
        return called;
    }

    out_widget = create_parameters.return_value;
    MPE_LOG_INFO("widget '{}' created at 0x{:X} and added to the viewport", widget_class,
                out_widget);
    return Result::Success();
}

namespace {

/// Finds a live object by its class name.
[[nodiscard]] std::uintptr_t FindByClass(const ObjectArray& objects,
                                         std::string_view class_name) {
    std::uintptr_t found = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name.rfind("Default__", 0) == 0 || object.class_name != class_name) {
            return true;
        }
        found = object.address;
        return false;
    });
    return found;
}

/// Finds a class object by name, for passing to Create.
[[nodiscard]] std::uintptr_t FindClassObject(const ObjectArray& objects,
                                             std::string_view name) {
    std::uintptr_t found = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name != name || object.class_name.find("Class") == std::string::npos) {
            return true;
        }
        found = object.address;
        return false;
    });
    return found;
}

} // namespace

namespace {

/// How many virtual table entries to copy. Comfortably past ProcessEvent's slot.
constexpr std::size_t kVTableEntries = 200;

std::uintptr_t              g_watched_widget = 0;
std::uintptr_t              g_original_vtable = 0;
std::vector<std::uintptr_t> g_vtable_copy;
std::atomic<bool>           g_widget_clicked{false};

/// Every widget being watched for clicks, and which one was clicked last.
///
/// One table rather than one watch, because the lobby needs every button on it to be
/// live at once, not just a single entry. They are all the same class, so they all share
/// one virtual table and therefore one copy of it: pointing each of them at that copy is
/// enough, and the handler then only has to work out which of them the event arrived on.
constexpr std::size_t       kMaxWatchedWidgets = 64;
std::atomic<std::uintptr_t> g_watched[kMaxWatchedWidgets];
std::atomic<std::uintptr_t> g_clicked_widget{0};
/// The UFunction that counts as a click, once identified.
std::atomic<std::uintptr_t> g_click_event{0};

using ProcessEventSignature = void(__fastcall*)(void*, void*, void*);
ProcessEventSignature g_real_process_event = nullptr;

/// Every distinct event seen on the watched widget, as UFunction addresses.
///
/// Which event a click produces is not something to assume: these buttons come from a UI
/// framework whose naming varies, and picking the wrong one gives a menu that either never
/// responds or fires constantly. Recording what actually arrives, then reading the names
/// back outside the hot path, settles it by observation.
constexpr std::size_t              kEventSlots = 32;
std::atomic<std::uintptr_t>        g_seen_events[kEventSlots];
std::atomic<std::uintptr_t>        g_last_event{0};

// --- Game thread pump ------------------------------------------------------
//
// A second widget, chosen for being long lived, whose events are used purely as a place to
// run queued work.
std::uintptr_t              g_pump_widget = 0;
std::uintptr_t              g_pump_original_vtable = 0;
std::vector<std::uintptr_t> g_pump_vtable_copy;
ProcessEventSignature       g_pump_real_process_event = nullptr;

/// Runs any queued job. Called from the pump's event path, on the game thread.
void DrainQueuedJob() {
    if (!g_job_waiting.load(std::memory_order_acquire)) {
        return;
    }
    g_handlers_active.fetch_add(1, std::memory_order_acq_rel);
    if (g_job_waiting.exchange(false, std::memory_order_acq_rel)) {
        if (g_job) {
            g_job();
        }
        g_job_done.store(true, std::memory_order_release);
    }
    g_handlers_active.fetch_sub(1, std::memory_order_acq_rel);
}

void __fastcall PumpProcessEvent(void* self, void* function, void* parameters) {
    DrainQueuedJob();
    if (g_pump_real_process_event != nullptr) {
        g_pump_real_process_event(self, function, parameters);
    }
}

/// Stands in for ProcessEvent on the watched widget only.
///
/// Runs on the game thread for every event that widget receives, so it does the least
/// possible: an address compare, a bounded scan of a small table, and the real call.
void __fastcall WidgetProcessEvent(void* self, void* function, void* parameters) {
    bool watched = reinterpret_cast<std::uintptr_t>(self) == g_watched_widget;
    if (!watched && self != nullptr) {
        const auto address = reinterpret_cast<std::uintptr_t>(self);
        for (std::size_t slot = 0; slot < kMaxWatchedWidgets; ++slot) {
            const std::uintptr_t entry = g_watched[slot].load(std::memory_order_acquire);
            if (entry == 0) {
                break;
            }
            if (entry == address) {
                watched = true;
                break;
            }
        }
    }
    if (watched && function != nullptr) {
        const auto value = reinterpret_cast<std::uintptr_t>(function);
        g_last_event.store(value, std::memory_order_release);

        for (std::size_t slot = 0; slot < kEventSlots; ++slot) {
            const std::uintptr_t seen = g_seen_events[slot].load(std::memory_order_acquire);
            if (seen == value) {
                break;
            }
            if (seen == 0) {
                std::uintptr_t expected = 0;
                if (g_seen_events[slot].compare_exchange_strong(expected, value,
                                                                std::memory_order_acq_rel)) {
                    break;
                }
            }
        }

        if (value == g_click_event.load(std::memory_order_acquire)) {
            g_widget_clicked.store(true, std::memory_order_release);
            // Which button, not just that one was pressed. The lobby has several and they
            // do different things.
            g_clicked_widget.store(reinterpret_cast<std::uintptr_t>(self),
                                   std::memory_order_release);
        }
    }
    if (g_real_process_event != nullptr) {
        g_real_process_event(self, function, parameters);
    }
}

} // namespace

Result InstallGameThreadPump(std::uintptr_t widget) {
    if (widget == 0) {
        return Result::Fail(ErrorCode::InvalidArgument, "no widget given");
    }
    if (g_pump_widget == widget) {
        return Result::Success();
    }
    RemoveGameThreadPump();

    std::uintptr_t process_event = 0;
    int            slot          = -1;
    {
        std::lock_guard lock(g_mutex);
        if (!g_detected) {
            return Result::Fail(ErrorCode::InvalidState, "call layout not detected");
        }
        process_event = g_layout.process_event;
        slot          = g_layout.vtable_slot;
    }

    std::uintptr_t table = 0;
    if (!memory::GuardedRead(widget, &table, sizeof(table)) || table == 0) {
        return Result::Fail(ErrorCode::InvalidState, "could not read the pump vtable");
    }

    g_pump_vtable_copy.assign(kVTableEntries, 0);
    for (std::size_t entry = 0; entry < kVTableEntries; ++entry) {
        std::uintptr_t value = 0;
        if (!memory::GuardedRead(table + entry * sizeof(value), &value, sizeof(value))) {
            break;
        }
        g_pump_vtable_copy[entry] = value;
    }

    g_pump_real_process_event = reinterpret_cast<ProcessEventSignature>(process_event);
    g_pump_vtable_copy[static_cast<std::size_t>(slot)] =
        reinterpret_cast<std::uintptr_t>(&PumpProcessEvent);

    const auto copy_address = reinterpret_cast<std::uintptr_t>(g_pump_vtable_copy.data());
    if (!memory::GuardedWrite(widget, &copy_address, sizeof(copy_address))) {
        return Result::Fail(ErrorCode::InvalidState, "could not repoint the pump vtable");
    }

    g_pump_original_vtable = table;
    g_pump_widget          = widget;
    MPE_LOG_INFO("game thread pump installed on widget 0x{:X}; jobs now run on the next "
                "frame instead of behind a breakpoint",
                widget);
    return Result::Success();
}

void RemoveGameThreadPump() {
    if (g_pump_widget != 0 && g_pump_original_vtable != 0) {
        (void)memory::GuardedWrite(g_pump_widget, &g_pump_original_vtable,
                                   sizeof(g_pump_original_vtable));
    }
    g_pump_widget          = 0;
    g_pump_original_vtable = 0;
}

bool GameThreadPumpActive() {
    return g_pump_widget != 0;
}

Result WatchWidgetEvents(std::uintptr_t widget) {
    if (widget == 0) {
        return Result::Fail(ErrorCode::InvalidArgument, "no widget given");
    }

    std::uintptr_t process_event = 0;
    int            slot          = -1;
    {
        std::lock_guard lock(g_mutex);
        if (!g_detected) {
            return Result::Fail(ErrorCode::InvalidState, "call layout not detected");
        }
        process_event = g_layout.process_event;
        slot          = g_layout.vtable_slot;
    }
    if (slot < 0) {
        return Result::Fail(ErrorCode::InvalidState, "no ProcessEvent slot");
    }

    std::uintptr_t table = 0;
    if (!memory::GuardedRead(widget, &table, sizeof(table)) || table == 0) {
        return Result::Fail(ErrorCode::InvalidState, "could not read the widget vtable");
    }

    // Copy the table so the original is never written to. Other widgets of the same class
    // continue to use it untouched.
    g_vtable_copy.assign(kVTableEntries, 0);
    for (std::size_t entry = 0; entry < kVTableEntries; ++entry) {
        std::uintptr_t value = 0;
        if (!memory::GuardedRead(table + entry * sizeof(value), &value, sizeof(value))) {
            break;
        }
        g_vtable_copy[entry] = value;
    }

    g_real_process_event  = reinterpret_cast<ProcessEventSignature>(process_event);
    g_vtable_copy[static_cast<std::size_t>(slot)] =
        reinterpret_cast<std::uintptr_t>(&WidgetProcessEvent);

    const auto copy_address = reinterpret_cast<std::uintptr_t>(g_vtable_copy.data());
    if (!memory::GuardedWrite(widget, &copy_address, sizeof(copy_address))) {
        return Result::Fail(ErrorCode::InvalidState, "could not repoint the widget vtable");
    }

    g_original_vtable = table;
    g_watched_widget  = widget;
    MPE_LOG_INFO("watching events on widget 0x{:X} (vtable 0x{:X} -> copy 0x{:X}, slot {})",
                widget, table, copy_address, slot);
    return Result::Success();
}

bool ConsumeWidgetClick() {
    return g_widget_clicked.exchange(false, std::memory_order_acq_rel);
}

Result AlsoWatchWidget(std::uintptr_t widget) {
    if (widget == 0) {
        return Result::Fail(ErrorCode::InvalidArgument, "no widget given");
    }
    if (g_original_vtable == 0 || g_vtable_copy.empty()) {
        return Result::Fail(ErrorCode::InvalidState,
                            "no watch is established to share a vtable with");
    }

    // Only widgets of the class the copy was taken from. A different class has a different
    // table, and pointing it at this one would call the wrong functions for every entry in
    // it, not just the one that was replaced.
    std::uintptr_t table = 0;
    if (!memory::GuardedRead(widget, &table, sizeof(table))) {
        return Result::Fail(ErrorCode::InvalidState, "could not read the widget vtable");
    }
    const auto copy_address = reinterpret_cast<std::uintptr_t>(g_vtable_copy.data());
    if (table != g_original_vtable && table != copy_address) {
        return Result::Fail(ErrorCode::InvalidState,
                            "the widget is not the class the watch was built for");
    }

    for (std::size_t slot = 0; slot < kMaxWatchedWidgets; ++slot) {
        std::uintptr_t expected = 0;
        const std::uintptr_t entry = g_watched[slot].load(std::memory_order_acquire);
        if (entry == widget) {
            return Result::Success();
        }
        if (entry == 0 &&
            g_watched[slot].compare_exchange_strong(expected, widget,
                                                    std::memory_order_acq_rel)) {
            if (table != copy_address &&
                !memory::GuardedWrite(widget, &copy_address, sizeof(copy_address))) {
                return Result::Fail(ErrorCode::InvalidState,
                                    "could not repoint the widget vtable");
            }
            return Result::Success();
        }
    }
    return Result::Fail(ErrorCode::InvalidState, "no room left to watch another widget");
}

std::uintptr_t ConsumeClickedWidget() {
    return g_clicked_widget.exchange(0, std::memory_order_acq_rel);
}

void ForgetExtraWatchedWidgets() {
    // The originals are restored by the caller's own bookkeeping where it matters; what
    // must not survive is a stale address, because a freed widget's memory can be reused
    // and the next object at that address would be treated as a button.
    for (std::size_t slot = 0; slot < kMaxWatchedWidgets; ++slot) {
        g_watched[slot].store(0, std::memory_order_release);
    }
    g_clicked_widget.store(0, std::memory_order_release);
}

std::vector<std::uintptr_t> SeenWidgetEvents() {
    std::vector<std::uintptr_t> out;
    for (std::size_t slot = 0; slot < kEventSlots; ++slot) {
        const std::uintptr_t seen = g_seen_events[slot].load(std::memory_order_acquire);
        if (seen == 0) {
            break;
        }
        out.push_back(seen);
    }
    return out;
}

std::uintptr_t LastWidgetEvent() {
    return g_last_event.load(std::memory_order_acquire);
}

void SetWidgetClickEvent(std::uintptr_t function) {
    g_click_event.store(function, std::memory_order_release);
}

void StopWatchingWidgetEvents() {
    if (g_watched_widget != 0 && g_original_vtable != 0) {
        (void)memory::GuardedWrite(g_watched_widget, &g_original_vtable,
                                   sizeof(g_original_vtable));
    }
    g_watched_widget  = 0;
    g_original_vtable = 0;
}

Result ResolveMenuButtonPlan(const ObjectArray& objects, std::uintptr_t known_menu,
                             MenuButtonPlan& out_plan) {
    constexpr std::uintptr_t kContainerOffset = 0x560;
    static constexpr std::uintptr_t kMenuButtonOffsets[] = {
        0x580, 0x508, 0x578, 0x568, 0x518, 0x510, 0x570,
    };

    // Everything except the menu itself is resolved once and kept.
    //
    // The button class, the two libraries, the four functions and the player controller do
    // not change for the life of the process, and this scanned the whole object array for
    // them every time a menu appeared. That is the worst possible moment to spend a scan:
    // the menu appearing is exactly when the entry needs to be on it, and the player is
    // looking at the screen waiting. Measured on this build it was most of the delay
    // between the menu being drawn and MULTIPLAYER showing up on it.
    //
    // The caller already knows the menu address, because it found it in the pass that
    // decided there was work to do, so with the rest cached a new menu costs no scan at
    // all: a few guarded reads and the game thread call.
    static std::mutex     s_plan_mutex;
    static MenuButtonPlan s_static_plan;
    static bool           s_static_resolved = false;

    std::lock_guard plan_lock(s_plan_mutex);

    MenuButtonPlan plan;
    if (s_static_resolved) {
        plan      = s_static_plan;
        plan.menu = known_menu;
    }

    if (!s_static_resolved || plan.menu == 0) {
        // One pass over the object array instead of one pass per lookup. The array is around
        // fifty thousand entries and every read is guarded, so the difference between one scan
        // and eight is the difference between instant and a visible stall.
        objects.ForEach([&](const ObjectInfo& object) {
        const bool is_default = object.name.rfind("Default__", 0) == 0;

        if (!is_default && plan.menu == 0 && object.class_name == "WBP_MainMenu_C") {
            plan.menu = object.address;
        } else if (plan.button_class == 0 &&
                   object.name == "WBP_MeteoriteStandaloneButtonDefault_C" &&
                   object.class_name.find("Class") != std::string::npos) {
            plan.button_class = object.address;
        } else if (plan.widget_library == 0 &&
                   object.name == "Default__WidgetBlueprintLibrary") {
            plan.widget_library = object.address;
        } else if (plan.text_library == 0 && object.name == "Default__KismetTextLibrary") {
            plan.text_library = object.address;
        } else if (object.class_name == "Function") {
            if (plan.create_function == 0 && object.name == "Create") {
                plan.create_function = object.address;
            } else if (plan.add_child_function == 0 && object.name == "AddChild") {
                plan.add_child_function = object.address;
            } else if (plan.remove_child_function == 0 && object.name == "RemoveChild") {
                plan.remove_child_function = object.address;
            } else if (plan.convert_function == 0 && object.name == "Conv_StringToText") {
                plan.convert_function = object.address;
            }
        } else if (!is_default && plan.controller == 0 &&
                   object.class_name.find("PlayerController") != std::string::npos &&
                   object.class_name.find("Component") == std::string::npos &&
                   object.name.find("_GEN_VARIABLE") == std::string::npos) {
            plan.controller = object.address;
        }
        return true;
        });
    }

    if (plan.menu == 0) {
        return Result::Fail(ErrorCode::InvalidState, "no live main menu");
    }
    if (plan.button_class == 0 || plan.create_function == 0 || plan.widget_library == 0 ||
        plan.add_child_function == 0 || plan.controller == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "the menu button pieces could not all be resolved");
    }

    // Kept without the per menu fields, which are filled in from the caller's address and
    // from reads against whichever menu is live at the time.
    if (!s_static_resolved) {
        s_static_plan                = plan;
        s_static_plan.menu           = 0;
        s_static_plan.container      = 0;
        s_static_plan.existing_count = 0;
        s_static_resolved            = true;
    }

    if (!memory::GuardedRead(plan.menu + kContainerOffset, &plan.container,
                             sizeof(plan.container)) ||
        plan.container == 0) {
        return Result::Fail(ErrorCode::InvalidState, "could not read MainButtonContainer");
    }

    for (const std::uintptr_t offset : kMenuButtonOffsets) {
        std::uintptr_t existing = 0;
        if (memory::GuardedRead(plan.menu + offset, &existing, sizeof(existing)) &&
            existing != 0) {
            plan.existing[plan.existing_count++] = existing;
        }
    }

    out_plan = plan;
    return Result::Success();
}

namespace {

/// Creates one button, labels it, and parents it into the container.
[[nodiscard]] std::uintptr_t MakeLabelledButton(const MenuButtonPlan& plan,
                                                std::string_view label) {
    constexpr std::uintptr_t kLabelOffset = 0x15B0;
    constexpr std::size_t    kTextSize    = 0x10;

    struct CreateParameters {
        std::uintptr_t world_context;
        std::uintptr_t widget_type;
        std::uintptr_t owning_player;
        std::uintptr_t return_value;
    };
    CreateParameters create_parameters{};
    create_parameters.world_context = plan.controller;
    create_parameters.widget_type   = plan.button_class;
    create_parameters.owning_player = plan.controller;

    if (!CallFunction(plan.widget_library, plan.create_function, &create_parameters).ok() ||
        create_parameters.return_value == 0) {
        return 0;
    }
    const std::uintptr_t button = create_parameters.return_value;

    if (plan.convert_function != 0 && plan.text_library != 0) {
        std::wstring wide;
        wide.reserve(label.size() + 1);
        for (const char character : label) {
            wide.push_back(static_cast<wchar_t>(character));
        }
        struct ConvertParameters {
            FStringLayout input;
            std::uint8_t  result[kTextSize];
        };
        ConvertParameters convert_parameters{};
        convert_parameters.input.data     = wide.data();
        convert_parameters.input.count    = static_cast<std::int32_t>(wide.size() + 1);
        convert_parameters.input.capacity = convert_parameters.input.count;
        if (CallFunction(plan.text_library, plan.convert_function, &convert_parameters).ok()) {
            (void)memory::GuardedWrite(button + kLabelOffset, convert_parameters.result,
                                       kTextSize);
        }
    }

    struct ChildParameters {
        std::uintptr_t content;
        std::uintptr_t return_value;
    };
    ChildParameters add_parameters{};
    add_parameters.content = button;
    if (!CallFunction(plan.container, plan.add_child_function, &add_parameters).ok()) {
        return 0;
    }
    return button;
}

} // namespace

Result BuildMenuRows(const MenuButtonPlan& plan, const std::vector<MenuRow>& rows,
                     std::vector<std::uintptr_t>& out_buttons) {
    // Offsets from the button's own reflected properties.
    constexpr std::uintptr_t kDescriptionOffset = 0x15C0;
    constexpr std::uintptr_t kShowLeftBracket   = 0x15EC;
    constexpr std::uintptr_t kShowRightBracket  = 0x15ED;
    constexpr std::uintptr_t kShowTopBracket    = 0x15EE;
    constexpr std::uintptr_t kShowBottomBracket = 0x15EF;
    constexpr std::size_t    kTextSize          = 0x10;

    if (plan.container == 0 || plan.button_class == 0) {
        return Result::Fail(ErrorCode::InvalidState, "the menu plan is incomplete");
    }

    if (plan.remove_child_function != 0) {
        for (std::size_t index = 0; index < plan.existing_count; ++index) {
            struct ChildParameters {
                std::uintptr_t content;
                std::uintptr_t return_value;
            };
            ChildParameters remove_parameters{};
            remove_parameters.content = plan.existing[index];
            (void)CallFunction(plan.container, plan.remove_child_function, &remove_parameters);
        }
    }

    out_buttons.clear();
    out_buttons.reserve(rows.size());

    for (const MenuRow& row : rows) {
        const std::uintptr_t button = MakeLabelledButton(plan, row.label);
        if (button == 0) {
            return Result::Fail(ErrorCode::InvalidState,
                                std::format("could not create the '{}' row", row.label));
        }

        if (!row.description.empty() && plan.convert_function != 0 &&
            plan.text_library != 0) {
            std::wstring wide;
            wide.reserve(row.description.size() + 1);
            for (const char character : row.description) {
                wide.push_back(static_cast<wchar_t>(character));
            }
            struct ConvertParameters {
                FStringLayout input;
                std::uint8_t  result[kTextSize];
            };
            ConvertParameters convert_parameters{};
            convert_parameters.input.data     = wide.data();
            convert_parameters.input.count    = static_cast<std::int32_t>(wide.size() + 1);
            convert_parameters.input.capacity = convert_parameters.input.count;
            if (CallFunction(plan.text_library, plan.convert_function, &convert_parameters)
                    .ok()) {
                (void)memory::GuardedWrite(button + kDescriptionOffset,
                                           convert_parameters.result, kTextSize);
            }
        }

        // Headings are framed top and bottom, which separates sections without needing a
        // divider widget that would have to be constructed.
        const std::uint8_t framed = row.heading ? 1 : 0;
        (void)memory::GuardedWrite(button + kShowTopBracket, &framed, sizeof(framed));
        (void)memory::GuardedWrite(button + kShowBottomBracket, &framed, sizeof(framed));
        const std::uint8_t sides = row.heading ? 0 : 1;
        (void)memory::GuardedWrite(button + kShowLeftBracket, &sides, sizeof(sides));
        (void)memory::GuardedWrite(button + kShowRightBracket, &sides, sizeof(sides));

        out_buttons.push_back(button);
    }

    MPE_LOG_INFO("built a {} row lobby in container 0x{:X}", out_buttons.size(),
                plan.container);
    return Result::Success();
}

Result BuildMenuList(const MenuButtonPlan& plan, const std::vector<std::string>& labels,
                     std::vector<std::uintptr_t>& out_buttons) {
    if (plan.container == 0 || plan.button_class == 0) {
        return Result::Fail(ErrorCode::InvalidState, "the menu plan is incomplete");
    }

    // Clear the shipped entries first. They are removed rather than hidden so the container
    // lays the new list out from the top and navigation does not walk through invisible
    // items.
    if (plan.remove_child_function != 0) {
        for (std::size_t index = 0; index < plan.existing_count; ++index) {
            struct ChildParameters {
                std::uintptr_t content;
                std::uintptr_t return_value;
            };
            ChildParameters remove_parameters{};
            remove_parameters.content = plan.existing[index];
            (void)CallFunction(plan.container, plan.remove_child_function, &remove_parameters);
        }
    }

    out_buttons.clear();
    out_buttons.reserve(labels.size());
    for (const std::string& label : labels) {
        const std::uintptr_t button = MakeLabelledButton(plan, label);
        if (button == 0) {
            return Result::Fail(ErrorCode::InvalidState,
                                std::format("could not create the '{}' entry", label));
        }
        out_buttons.push_back(button);
    }

    MPE_LOG_INFO("built a {} entry list in container 0x{:X}", out_buttons.size(),
                plan.container);
    return Result::Success();
}

Result ApplyMenuButtonPlan(const MenuButtonPlan& plan, std::string_view label,
                           std::uintptr_t& out_button) {
    constexpr std::uintptr_t kLabelOffset = 0x15B0;
    constexpr std::size_t    kTextSize    = 0x10;

    struct CreateParameters {
        std::uintptr_t world_context;
        std::uintptr_t widget_type;
        std::uintptr_t owning_player;
        std::uintptr_t return_value;
    };
    CreateParameters create_parameters{};
    create_parameters.world_context = plan.controller;
    create_parameters.widget_type   = plan.button_class;
    create_parameters.owning_player = plan.controller;

    if (const Result called =
            CallFunction(plan.widget_library, plan.create_function, &create_parameters);
        !called.ok()) {
        return called;
    }
    if (create_parameters.return_value == 0) {
        return Result::Fail(ErrorCode::InvalidState, "Create returned no button");
    }
    const std::uintptr_t button = create_parameters.return_value;

    if (plan.convert_function != 0 && plan.text_library != 0) {
        std::wstring wide;
        wide.reserve(label.size() + 1);
        for (const char character : label) {
            wide.push_back(static_cast<wchar_t>(character));
        }
        struct ConvertParameters {
            FStringLayout input;
            std::uint8_t  result[kTextSize];
        };
        ConvertParameters convert_parameters{};
        convert_parameters.input.data     = wide.data();
        convert_parameters.input.count    = static_cast<std::int32_t>(wide.size() + 1);
        convert_parameters.input.capacity = convert_parameters.input.count;
        if (CallFunction(plan.text_library, plan.convert_function, &convert_parameters).ok()) {
            (void)memory::GuardedWrite(button + kLabelOffset, convert_parameters.result,
                                       kTextSize);
        }
    }

    struct ChildParameters {
        std::uintptr_t content;
        std::uintptr_t return_value;
    };
    ChildParameters add_parameters{};
    add_parameters.content = button;
    if (const Result called =
            CallFunction(plan.container, plan.add_child_function, &add_parameters);
        !called.ok()) {
        return called;
    }

    // Rotate the shipped entries below the new one, since the panel has no insert at index.
    if (plan.remove_child_function != 0) {
        for (std::size_t index = 0; index < plan.existing_count; ++index) {
            ChildParameters remove_parameters{};
            remove_parameters.content = plan.existing[index];
            if (!CallFunction(plan.container, plan.remove_child_function, &remove_parameters)
                     .ok()) {
                continue;
            }
            ChildParameters add_back{};
            add_back.content = plan.existing[index];
            (void)CallFunction(plan.container, plan.add_child_function, &add_back);
        }
    }

    out_button = button;
    return Result::Success();
}

Result AddMainMenuButton(const ObjectArray& objects, std::string_view label,
                         std::uintptr_t& out_button) {
    // Offsets read from the menu's own reflected properties:
    //   WBP_MainMenu_C                        +0x560  MainButtonContainer
    //   WBP_MeteoriteStandaloneButtonDefault_C +0x15B0 ButtonLabelText (FText, 16 bytes)
    constexpr std::uintptr_t kContainerOffset = 0x560;
    constexpr std::uintptr_t kLabelOffset     = 0x15B0;
    constexpr std::size_t    kTextSize        = 0x10;

    const std::uintptr_t menu = FindByClass(objects, "WBP_MainMenu_C");
    if (menu == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "no live main menu; this only works at the main menu");
    }

    std::uintptr_t container = 0;
    if (!memory::GuardedRead(menu + kContainerOffset, &container, sizeof(container)) ||
        container == 0) {
        return Result::Fail(ErrorCode::InvalidState, "could not read MainButtonContainer");
    }

    const std::uintptr_t button_class =
        FindClassObject(objects, "WBP_MeteoriteStandaloneButtonDefault_C");
    if (button_class == 0) {
        return Result::Fail(ErrorCode::InvalidState, "the menu button class was not found");
    }

    const std::uintptr_t controller = FindPlayerController(objects, /*allow_frontend=*/true);
    const std::uintptr_t create     = FindFunction(objects, "Create");
    const std::uintptr_t library    = FindByClass(objects, "WidgetBlueprintLibrary");
    std::uintptr_t       library_cdo = library;
    if (library_cdo == 0) {
        objects.ForEach([&](const ObjectInfo& object) {
            if (object.name != "Default__WidgetBlueprintLibrary") {
                return true;
            }
            library_cdo = object.address;
            return false;
        });
    }
    if (controller == 0 || create == 0 || library_cdo == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "missing a player controller, Create, or the widget library");
    }

    struct CreateParameters {
        std::uintptr_t world_context;
        std::uintptr_t widget_type;
        std::uintptr_t owning_player;
        std::uintptr_t return_value;
    };
    CreateParameters create_parameters{};
    create_parameters.world_context = controller;
    create_parameters.widget_type   = button_class;
    create_parameters.owning_player = controller;

    if (const Result called = CallFunction(library_cdo, create, &create_parameters);
        !called.ok()) {
        return called;
    }
    if (create_parameters.return_value == 0) {
        return Result::Fail(ErrorCode::InvalidState, "Create returned no button");
    }
    const std::uintptr_t button = create_parameters.return_value;

    // The label is an FText, which cannot be built by hand: it owns shared text data. The
    // engine's own string to text conversion is used instead, and the result copied into
    // the property.
    if (const std::uintptr_t convert = FindFunction(objects, "Conv_StringToText");
        convert != 0) {
        std::uintptr_t text_library = 0;
        objects.ForEach([&](const ObjectInfo& object) {
            if (object.name != "Default__KismetTextLibrary") {
                return true;
            }
            text_library = object.address;
            return false;
        });

        if (text_library != 0) {
            std::wstring wide;
            wide.reserve(label.size() + 1);
            for (const char character : label) {
                wide.push_back(static_cast<wchar_t>(character));
            }

            struct ConvertParameters {
                FStringLayout input;          // +0x00
                std::uint8_t  result[kTextSize]; // +0x10
            };
            ConvertParameters convert_parameters{};
            convert_parameters.input.data     = wide.data();
            convert_parameters.input.count    = static_cast<std::int32_t>(wide.size() + 1);
            convert_parameters.input.capacity = convert_parameters.input.count;

            if (CallFunction(text_library, convert, &convert_parameters).ok()) {
                if (memory::GuardedWrite(button + kLabelOffset, convert_parameters.result,
                                         kTextSize)) {
                    MPE_LOG_INFO("button label set to '{}'", label);
                } else {
                    MPE_LOG_WARN("could not write the button label");
                }
            }
        } else {
            MPE_LOG_WARN("Default__KismetTextLibrary not found; button will be unlabelled");
        }
    }

    // Parent it into the menu's own container so the game lays it out and navigates to it.
    const std::uintptr_t add_child = FindFunction(objects, "AddChild");
    if (add_child == 0) {
        return Result::Fail(ErrorCode::InvalidState, "AddChild was not found");
    }
    struct AddChildParameters {
        std::uintptr_t content;
        std::uintptr_t return_value;
    };
    AddChildParameters add_parameters{};
    add_parameters.content = button;

    if (const Result called = CallFunction(container, add_child, &add_parameters);
        !called.ok()) {
        return called;
    }

    // Move it to the top.
    //
    // The panel offers no insert at index and no shift, only add and remove, so the order
    // is rotated instead: every original entry is removed and appended again, in the order
    // the menu already shows them, which leaves the new button ahead of all of them.
    // The menu names each button, so the sequence is read from properties rather than
    // guessed from the panel's child list.
    static constexpr std::uintptr_t kMenuButtonOffsets[] = {
        0x580, // ResumeCampaignButton
        0x508, // CampaignMenuButton
        0x578, // RemixButton
        0x568, // PlayCoop
        0x518, // CustomizationButton
        0x510, // CollectiblesButton
        0x570, // QuitButton
    };

    const std::uintptr_t remove_child = FindFunction(objects, "RemoveChild");
    if (remove_child != 0) {
        int moved = 0;
        for (const std::uintptr_t offset : kMenuButtonOffsets) {
            std::uintptr_t existing = 0;
            if (!memory::GuardedRead(menu + offset, &existing, sizeof(existing)) ||
                existing == 0) {
                continue;
            }
            struct ChildParameters {
                std::uintptr_t content;
                std::uintptr_t return_value;
            };
            ChildParameters remove_parameters{};
            remove_parameters.content = existing;
            if (!CallFunction(container, remove_child, &remove_parameters).ok()) {
                continue;
            }
            ChildParameters add_back{};
            add_back.content = existing;
            if (CallFunction(container, add_child, &add_back).ok()) {
                ++moved;
            }
        }
        MPE_LOG_INFO("rotated {} existing entries below '{}'", moved, label);
    } else {
        MPE_LOG_WARN("RemoveChild not found; '{}' stays at the bottom of the menu", label);
    }

    out_button = button;
    MPE_LOG_INFO("'{}' button 0x{:X} added to the main menu container 0x{:X}", label, button,
                container);
    return Result::Success();
}

Result HideWidget(const ObjectArray& objects, std::uintptr_t widget) {
    if (widget == 0) {
        return Result::Fail(ErrorCode::InvalidArgument, "no widget given");
    }
    const std::uintptr_t remove = FindFunction(objects, "RemoveFromParent");
    if (remove == 0) {
        return Result::Fail(ErrorCode::InvalidState, "RemoveFromParent was not found");
    }
    return CallFunction(widget, remove, nullptr);
}

Result CallSimple(const ObjectArray& objects, std::string_view class_name,
                  std::string_view function_name) {
    const std::uintptr_t instance = FindInstanceOfClass(objects, class_name);
    if (instance == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("no live instance of {}", class_name));
    }
    const std::uintptr_t function = FindFunction(objects, function_name);
    if (function == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("{} was not found", function_name));
    }
    MPE_LOG_INFO("calling {}::{} on 0x{:X}", class_name, function_name, instance);
    return CallFunction(instance, function, nullptr);
}

Result CallReturningInt(const ObjectArray& objects, std::string_view class_name,
                        std::string_view function_name, int& out_value) {
    const std::uintptr_t instance = FindInstanceOfClass(objects, class_name);
    if (instance == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("no live instance of {}", class_name));
    }
    const std::uintptr_t function = FindFunction(objects, function_name);
    if (function == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("{} was not found", function_name));
    }
    struct Parameters {
        std::int32_t return_value;
        std::int32_t padding;
    };
    Parameters parameters{};
    if (const Result called = CallFunction(instance, function, &parameters); !called.ok()) {
        return called;
    }
    out_value = parameters.return_value;
    MPE_LOG_INFO("{}::{} returned {}", class_name, function_name, out_value);
    return Result::Success();
}

Result BeginCampaign(const ObjectArray& objects, const Reflection& reflection,
                     std::string_view scenario, std::string_view campaign_asset,
                     bool friendly_fire, int difficulty) {
    // The subsystem instance, not the class. Only a live one carries the state that makes
    // the call meaningful.
    std::uintptr_t subsystem = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name.rfind("Default__", 0) == 0 || object.class_name == "Class") {
            return true;
        }
        if (object.class_name != "BlamCampaignFlowGameSubsystem") {
            return true;
        }
        subsystem = object.address;
        return false;
    });
    if (subsystem == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "no live BlamCampaignFlowGameSubsystem");
    }

    // The campaign data asset. BeginCampaign alone returned false because nothing had set
    // an active campaign; SetAndBeginCampaign takes it directly.
    std::uintptr_t campaign = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.class_name != "BlamCampaignDataAsset" || object.name != campaign_asset) {
            return true;
        }
        campaign = object.address;
        return false;
    });
    if (campaign == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            std::format("no BlamCampaignDataAsset named '{}'",
                                        campaign_asset));
    }

    const std::uintptr_t function = FindFunction(objects, "SetAndBeginCampaign");
    if (function == 0) {
        return Result::Fail(ErrorCode::InvalidState, "SetAndBeginCampaign was not found");
    }

    std::wstring wide;
    wide.reserve(scenario.size() + 1);
    for (const char character : scenario) {
        wide.push_back(static_cast<wchar_t>(character));
    }

    std::uint64_t scenario_name = 0;
    if (const Result made = MakeFName(wide.c_str(), scenario_name); !made.ok()) {
        return made;
    }
    if (scenario_name == 0) {
        return Result::Fail(ErrorCode::InvalidArgument,
                            std::format("'{}' is not a name the game knows; a scenario has "
                                        "to already exist in the name pool",
                                        scenario));
    }

    // Parameters, laid out from the function's own reflected properties:
    //   +0x00 Campaign (object)  +0x08 StartingScenarioName (FName)
    //   +0x10 Options (0x88)     +0x98 ReturnValue
    struct Parameters {
        std::uintptr_t campaign;
        std::uint64_t  scenario_name;
        std::uint8_t   options[0x88];
        bool           return_value;
        std::uint8_t   padding[7];
    };
    static_assert(offsetof(Parameters, scenario_name) == 0x08, "name must sit at 0x08");
    static_assert(offsetof(Parameters, options) == 0x10, "options must sit at 0x10");
    static_assert(offsetof(Parameters, return_value) == 0x98, "return value must sit at 0x98");

    Parameters parameters{};
    parameters.campaign      = campaign;
    parameters.scenario_name = scenario_name;

    // The options are copied from a live save record rather than zeroed. The struct is the
    // same BlamScenarioGameOptions the save game carries, and a real one is guaranteed to
    // hold values the engine considers valid, which a block of zeroes is not.
    std::uintptr_t source = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name.rfind("Default__", 0) == 0) {
            return true;
        }
        if (object.class_name != "BlamMetaDataSaveGame") {
            return true;
        }
        for (const PropertyInfo& property : reflection.ReadProperties(
                 objects.ClassOf(object.address))) {
            if (property.name == "SavedScenarioGameOptions") {
                source = object.address + static_cast<std::uintptr_t>(property.offset);
                return false;
            }
        }
        return true;
    });

    if (source != 0 && memory::GuardedRead(source, parameters.options,
                                           sizeof(parameters.options))) {
        MPE_LOG_INFO("options copied from a live save record at 0x{:X}", source);
    } else {
        MPE_LOG_WARN("no live save options found; sending a zeroed options struct, which the "
                    "engine may reject");
    }

    // Host settings are written into the options the match actually starts with, which is
    // the reliable place for them: setting the flag on some other copy afterwards is what
    // made friendly fire unreliable before.
    //   +0x18 CampaignDifficultyLevel   +0x70 bFriendlyFireEnabled
    parameters.options[0x70] = friendly_fire ? 1 : 0;
    if (difficulty >= 0 && difficulty <= 3) {
        parameters.options[0x18] = static_cast<std::uint8_t>(difficulty);
    }

    MPE_LOG_INFO("SetAndBeginCampaign(campaign '{}' 0x{:X}, scenario '{}', friendly fire {}, "
                "difficulty {}) on subsystem 0x{:X}",
                campaign_asset, campaign, scenario, friendly_fire ? "on" : "off",
                difficulty < 0 ? -1 : difficulty, subsystem);
    if (const Result called = CallFunction(subsystem, function, &parameters); !called.ok()) {
        return called;
    }
    MPE_LOG_INFO("SetAndBeginCampaign returned {}",
                parameters.return_value ? "true" : "false");

    if (!parameters.return_value) {
        return Result::Fail(ErrorCode::InvalidState,
                            "SetAndBeginCampaign refused the request; the scenario is "
                            "probably not part of that campaign asset");
    }
    return Result::Success();
}

Result ExecuteConsoleCommand(const ObjectArray& objects, std::string_view command) {
    // A player controller is what owns ConsoleCommand, and one only exists once a world is
    // up, which is why this fails cleanly at the main menu rather than faulting.
    std::uintptr_t controller = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name.rfind("Default__", 0) == 0) {
            return true;
        }
        if (object.class_name.find("PlayerController") == std::string::npos &&
            object.class_name.find("_C") == std::string::npos) {
            return true;
        }
        if (object.class_name.find("PlayerController") == std::string::npos) {
            return true;
        }
        controller = object.address;
        return false;
    });
    if (controller == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "no live player controller; load a level first");
    }

    const std::uintptr_t function = FindFunction(objects, "ConsoleCommand");
    if (function == 0) {
        return Result::Fail(ErrorCode::InvalidState, "ConsoleCommand was not found");
    }

    // The parameter block mirrors what the function declares:
    //     FString Command; bool bWriteToLog; FString ReturnValue;
    // An FString is a pointer plus a used count and a capacity, both counting characters
    // including the terminator.
    struct FStringLayout {
        wchar_t*     data;
        std::int32_t count;
        std::int32_t capacity;
    };
    struct Parameters {
        FStringLayout command;
        bool          write_to_log;
        char          padding[7];
        FStringLayout result;
    };

    std::wstring text;
    text.reserve(command.size() + 1);
    for (const char character : command) {
        text.push_back(static_cast<wchar_t>(character));
    }

    Parameters parameters{};
    parameters.command.data     = text.data();
    parameters.command.count    = static_cast<std::int32_t>(text.size() + 1);
    parameters.command.capacity = parameters.command.count;
    parameters.write_to_log     = true;

    MPE_LOG_INFO("executing console command: {}", command);
    const Result called = CallFunction(controller, function, &parameters);
    if (!called.ok()) {
        return called;
    }
    MPE_LOG_INFO("console command returned");
    return Result::Success();
}

} // namespace mpe::unreal
