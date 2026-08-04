// SPDX-License-Identifier: MIT
// ForgeEvolved: Unreal/LobbyUI.cpp
#define FE_LOG_CATEGORY "Unreal.LobbyUI"

#include "Unreal/LobbyUI.h"

#include "Core/Log.h"
#include "Unreal/GameThread.h"
#include "Unreal/ProcessMemory.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace fe::unreal {
namespace {

/// The design is laid out against this size and scaled by the canvas, so it holds its
/// proportions on any resolution rather than needing per resolution numbers.
constexpr float kDesignWidth  = 1920.0F;
constexpr float kDesignHeight = 1080.0F;

/// FVector2D as the engine actually lays it out.
///
/// Double precision, not float. Unreal 5 widened the core vector types, so FVector2D is
/// sixteen bytes. Passing a pair of floats to a function that takes one is not a rounding
/// difference, it is a parameter block half the expected size: the engine reads the four
/// bytes of x and the four of y as the first double, and reads whatever follows on the
/// stack as the second.
///
/// This is what made every earlier lobby build without a single error and draw nothing.
/// The widgets were created, parented and given slots correctly, then placed at a garbage
/// position with a garbage size, which put them off screen or collapsed them to nothing.
struct Vector2 {
    double x{0};
    double y{0};
};

struct LinearColour {
    float r{0};
    float g{0};
    float b{0};
    float a{1};
};

/// The palette from the reference: dark slate panels with a cyan accent.
constexpr LinearColour kPanel      = {0.043F, 0.063F, 0.078F, 0.92F};
constexpr LinearColour kPanelLight = {0.086F, 0.114F, 0.133F, 0.95F};
constexpr LinearColour kAccent     = {0.294F, 0.780F, 0.886F, 1.0F};
constexpr LinearColour kAccentDim  = {0.294F, 0.780F, 0.886F, 0.25F};
constexpr LinearColour kRed        = {0.898F, 0.286F, 0.286F, 1.0F};
constexpr LinearColour kBlue       = {0.361F, 0.643F, 0.898F, 1.0F};
constexpr LinearColour kText       = {0.902F, 0.933F, 0.949F, 1.0F};
constexpr LinearColour kTextDim    = {0.600F, 0.655F, 0.690F, 1.0F};
constexpr LinearColour kSlot       = {0.180F, 0.216F, 0.239F, 0.45F};
/// The plus on an empty slot. Deliberately dim: an empty slot is an absence, and it should
/// read as quieter than a card with somebody in it rather than competing with one.
constexpr LinearColour kSlotMark   = {0.294F, 0.780F, 0.886F, 0.35F};
constexpr LinearColour kGood       = {0.400F, 0.850F, 0.450F, 1.0F};
constexpr LinearColour kBad        = {0.910F, 0.380F, 0.350F, 1.0F};
constexpr LinearColour kWarn       = {0.960F, 0.760F, 0.300F, 1.0F};

/// The bars drawn behind the two mode buttons. Selection is shown by making one visible
/// and the other not, so choosing a mode costs a visibility change rather than a rebuild.
std::uintptr_t g_mode_marker[2] = {0, 0};

/// The same idea for the map list: one marker per map, only the chosen one visible.
std::uintptr_t g_map_marker[4] = {0, 0, 0, 0};

/// The server name field, so its contents can be read back when hosting.
std::uintptr_t g_server_name_field = 0;

/// ESlateVisibility values used throughout.
constexpr std::uint8_t kVisibleValue          = 0;
constexpr std::uint8_t kCollapsedValue        = 1;
constexpr std::uint8_t kHitTestInvisible      = 3;
/// EStretch::Fill. Stretches a widget to its whole box rather than fitting it centred.
constexpr std::uint8_t kStretchFill           = 1;
constexpr std::uint8_t kSelfHitTestInvisibleValue = 4;

/// The server table, built once and then only rewritten.
///
/// Rebuilding the browser to apply a filter would mean creating the whole screen again,
/// which is exactly the cost this design exists to avoid. The rows are permanent and their
/// text is replaced, so filtering is a few string writes.
struct ServerRowWidgets {
    std::uintptr_t highlight{0};
    std::uintptr_t button{0};
    std::uintptr_t name{0};
    std::uintptr_t mode{0};
    std::uintptr_t map{0};
    std::uintptr_t players{0};
    std::uintptr_t ping{0};
};
constexpr std::size_t kServerRows = 8;
ServerRowWidgets g_server_row[kServerRows];

/// The details panel's five value lines, and the filter markers.
std::uintptr_t g_detail_line[5]   = {0, 0, 0, 0, 0};
std::uintptr_t g_filter_mode[3]   = {0, 0, 0};
std::uintptr_t g_filter_slots[3]  = {0, 0, 0};
std::uintptr_t g_filter_ping[3]   = {0, 0, 0};
std::uintptr_t g_empty_notice     = 0;

/// The status panel's three lines, top right of the screen.
std::uintptr_t g_status_line[3] = {0, 0, 0};

/// Builds widgets and places them on a canvas.
///
/// Every call here is an engine call on the game thread, so the builder keeps them to the
/// minimum: create, parent, position, and set the one or two properties that matter.
class Builder {
public:
    explicit Builder(const LobbyUIContext& context) noexcept : context_(context) {}

    [[nodiscard]] const LobbyUIContext& Context() const noexcept { return context_; }

    /// Creates a widget of a class.
    [[nodiscard]] std::uintptr_t Spawn(std::uintptr_t widget_class) const {
        struct Parameters {
            std::uintptr_t object_class;
            std::uintptr_t outer;
            std::uintptr_t return_value;
        };
        Parameters parameters{};
        parameters.object_class = widget_class;
        parameters.outer        = context_.outer;
        if (!CallFunction(context_.gameplay_statics, context_.spawn_object, &parameters).ok()) {
            return 0;
        }
        return parameters.return_value;
    }

    /// Parents a widget to a canvas and places it.
    ///
    /// Anchors are left at the default top left, and the position and size are given in
    /// design space; the canvas scales the whole thing to the real viewport.
    [[nodiscard]] bool Place(std::uintptr_t canvas, std::uintptr_t widget, float x, float y,
                             float width, float height) const {
        struct AddParameters {
            std::uintptr_t content;
            std::uintptr_t return_value;
        };
        AddParameters add{};
        add.content = widget;
        if (!CallFunction(canvas, context_.add_to_canvas, &add).ok() ||
            add.return_value == 0) {
            return false;
        }
        const std::uintptr_t slot = add.return_value;

        struct VectorParameters {
            Vector2 value;
        };
        VectorParameters position{};
        position.value = {x, y};
        (void)CallFunction(slot, context_.set_position, &position);

        VectorParameters size{};
        size.value = {width, height};
        (void)CallFunction(slot, context_.set_size, &size);
        return true;
    }

    /// A filled rectangle, used for panels, bars and slots.
    [[nodiscard]] std::uintptr_t Panel(std::uintptr_t canvas, float x, float y, float width,
                                       float height, LinearColour colour) const {
        const std::uintptr_t border = Spawn(context_.border_class);
        if (border == 0) {
            return 0;
        }
        // Border's brush colour lives in its style rather than behind a setter that takes a
        // plain colour, so it is written directly. BrushColor sits at the start of the
        // background brush, which is the first property on the class.
        SetBorderColour(border, colour);
        if (!Place(canvas, border, x, y, width, height)) {
            return 0;
        }
        return border;
    }

    /// A line of text.
    [[nodiscard]] std::uintptr_t Text(std::uintptr_t canvas, float x, float y, float width,
                                      float height, std::string_view value,
                                      LinearColour colour, float size = 22.0F) const {
        const std::uintptr_t block = Spawn(context_.text_class);
        if (block == 0) {
            return 0;
        }
        SetText(block, value);
        SetTextAppearance(block, colour, size);
        if (!Place(canvas, block, x, y, width, height)) {
            return 0;
        }
        return block;
    }

    /// One of the game's own menu buttons, placed on the canvas.
    ///
    /// Created through the same path the main menu uses for its entries, so it arrives
    /// fully styled: the frontend's typeface, its hover and pressed states, its sounds and
    /// its focus handling. A border with text on it can look similar in a screenshot and is
    /// none of those things, and in particular it cannot be clicked.
    ///
    ///   the button's label is an FText at +0x15B0
    /// stretch is an EStretch: ScaleToFit keeps the button's proportions and centres it,
    /// which means its clickable area is only the shrunken art, not the box. Fill stretches
    /// it to the whole box instead, so the entire area responds. Anything showing a label
    /// wants ScaleToFit; anything that is purely a hit target wants Fill.
    [[nodiscard]] std::uintptr_t Button(std::uintptr_t canvas, float x, float y, float width,
                                        float height, std::string_view label,
                                        std::uint8_t stretch = 2) const {
        constexpr std::uintptr_t kLabelOffset = 0x15B0;
        constexpr std::size_t    kTextSize    = 0x10;

        if (context_.button_class == 0 || context_.create_widget == 0 ||
            context_.widget_library == 0) {
            return 0;
        }
        struct CreateParameters {
            std::uintptr_t world_context;
            std::uintptr_t widget_type;
            std::uintptr_t owning_player;
            std::uintptr_t return_value;
        };
        CreateParameters create{};
        create.world_context = context_.outer;
        create.widget_type   = context_.button_class;
        if (!CallFunction(context_.widget_library, context_.create_widget, &create).ok() ||
            create.return_value == 0) {
            return 0;
        }

        if (context_.convert_function != 0 && context_.text_library != 0) {
            std::wstring wide;
            wide.reserve(label.size() + 1);
            for (const char character : label) {
                wide.push_back(static_cast<wchar_t>(character));
            }
            wide.push_back(L'\0');
            struct ConvertParameters {
                struct {
                    wchar_t*     data;
                    std::int32_t count;
                    std::int32_t capacity;
                } input;
                std::uint8_t result[kTextSize];
            };
            ConvertParameters convert{};
            convert.input.data     = wide.data();
            convert.input.count    = static_cast<std::int32_t>(wide.size());
            convert.input.capacity = convert.input.count;
            if (CallFunction(context_.text_library, context_.convert_function, &convert)
                    .ok()) {
                (void)memory::GuardedWrite(create.return_value + kLabelOffset, convert.result,
                                           kTextSize);
            }
        }

        // Scaled to the box rather than dropped into it.
        //
        // The frontend's button is authored for the main menu's list and renders at that
        // size wherever it is put: placed straight into a canvas slot it does not shrink,
        // it clips. That is why the two mode buttons came out as a pair of bracket marks
        // with no label between them, and why START MATCH ran off the screen reading
        // "T MATCH". A scale box gives the button all the room it wants and then fits the
        // result to the space the layout actually has.
        const std::uintptr_t fitted = Spawn(context_.scalebox_class);
        if (fitted == 0) {
            return 0;
        }
        struct AddParameters {
            std::uintptr_t content;
            std::uintptr_t return_value;
        };
        AddParameters inside{};
        inside.content = create.return_value;
        if (!CallFunction(fitted, context_.add_child, &inside).ok() ||
            inside.return_value == 0) {
            return 0;
        }
        struct StretchParameters {
            std::uint8_t stretch;
        };
        StretchParameters mode{stretch};
        (void)CallFunction(fitted, context_.set_stretch, &mode);

        if (!Place(canvas, fitted, x, y, width, height)) {
            return 0;
        }
        return create.return_value;
    }

    /// A heading or a value drawn with the frontend's own button art.
    ///
    /// The same widget the lobby's buttons are made from, made non interactive: it is
    /// visible and it draws exactly as the game draws its menu text, but it does not answer
    /// the mouse, so it neither highlights nor swallows clicks meant for anything behind
    /// it. This is what makes a heading match the buttons instead of merely sitting near
    /// them in a similar colour.
    [[nodiscard]] std::uintptr_t Label(std::uintptr_t canvas, float x, float y, float width,
                                       float height, std::string_view text) const {
        const std::uintptr_t label = Button(canvas, x, y, width, height, text);
        if (label == 0) {
            return 0;
        }
        SetVisibilityOf(label, kHitTestInvisible);
        return label;
    }

    /// Replaces the text on a label or button built earlier.
    void Relabel(std::uintptr_t button, std::string_view text) const {
        constexpr std::uintptr_t kLabelOffset = 0x15B0;
        constexpr std::size_t    kTextSize    = 0x10;
        if (button == 0 || context_.convert_function == 0 || context_.text_library == 0) {
            return;
        }
        std::wstring wide;
        wide.reserve(text.size() + 1);
        for (const char character : text) {
            wide.push_back(static_cast<wchar_t>(character));
        }
        wide.push_back(L'\0');
        struct ConvertParameters {
            struct {
                wchar_t*     data;
                std::int32_t count;
                std::int32_t capacity;
            } input;
            std::uint8_t result[kTextSize];
        };
        ConvertParameters convert{};
        convert.input.data     = wide.data();
        convert.input.count    = static_cast<std::int32_t>(wide.size());
        convert.input.capacity = convert.input.count;
        if (CallFunction(context_.text_library, context_.convert_function, &convert).ok()) {
            (void)memory::GuardedWrite(button + kLabelOffset, convert.result, kTextSize);
        }
    }

    void SetVisibilityOf(std::uintptr_t widget, std::uint8_t visibility) const {
        if (context_.set_visibility == 0 || widget == 0) {
            return;
        }
        struct Parameters {
            std::uint8_t visibility;
        };
        Parameters parameters{visibility};
        (void)CallFunction(widget, context_.set_visibility, &parameters);
    }

    /// A field the player can type into.
    ///
    ///   EditableTextBox +0x2E8 Text (FText, written directly)
    ///
    /// Written rather than passed to SetText, for the same reason the text block's caption
    /// is: SetText exists on several unrelated classes and calling the wrong one is a
    /// crash, not a wrong result.
    [[nodiscard]] std::uintptr_t Field(std::uintptr_t canvas, float x, float y, float width,
                                       float height, std::string_view initial) const {
        if (context_.editable_class == 0) {
            return 0;
        }
        const std::uintptr_t field = Spawn(context_.editable_class);
        if (field == 0) {
            return 0;
        }
        if (context_.has_font && context_.editable_font_offset != 0) {
            (void)memory::GuardedWrite(field + context_.editable_font_offset,
                                       context_.font_template.data(),
                                       context_.font_template.size());
            const float size = 20.0F;
            (void)memory::GuardedWrite(field + context_.editable_font_offset + 0x48, &size,
                                       sizeof(size));
        }
        if (context_.editable_colour_offset != 0) {
            constexpr std::uint8_t kUseSpecified = 0;
            (void)memory::GuardedWrite(field + context_.editable_colour_offset, &kText,
                                       sizeof(kText));
            (void)memory::GuardedWrite(field + context_.editable_colour_offset + 0x10,
                                       &kUseSpecified, sizeof(kUseSpecified));
        }
        if (context_.editable_hint_offset != 0) {
            SetFieldTextAt(field, context_.editable_hint_offset, "ENTER SERVER NAME");
        }
        if (!initial.empty()) {
            SetFieldText(field, initial);
        }
        if (!Place(canvas, field, x, y, width, height)) {
            return 0;
        }
        return field;
    }

    void SetFieldText(std::uintptr_t field, std::string_view value) const {
        SetFieldTextAt(field, 0x2E8, value);
    }

    void SetFieldTextAt(std::uintptr_t field, std::uintptr_t offset,
                        std::string_view value) const {
        const std::uintptr_t kFieldText = offset;
        std::wstring wide;
        wide.reserve(value.size() + 1);
        for (const char character : value) {
            wide.push_back(static_cast<wchar_t>(character));
        }
        wide.push_back(L'\0');
        struct ConvertParameters {
            struct {
                wchar_t*     data;
                std::int32_t count;
                std::int32_t capacity;
            } input;
            std::uint8_t result[0x10];
        };
        ConvertParameters convert{};
        convert.input.data     = wide.data();
        convert.input.count    = static_cast<std::int32_t>(wide.size());
        convert.input.capacity = convert.input.count;
        if (CallFunction(context_.text_library, context_.convert_function, &convert).ok()) {
            (void)memory::GuardedWrite(field + kFieldText, convert.result, sizeof(convert.result));
        }
    }

    /// The game's own panel backing, filling a rectangle.
    ///
    /// Falls back to a plain filled border when the frontend's backer is not available, so
    /// a panel is always drawn rather than a hole appearing in the layout.
    [[nodiscard]] std::uintptr_t Backer(std::uintptr_t canvas, float x, float y, float width,
                                        float height, LinearColour fallback) const {
        if (context_.backer_class == 0 || context_.create_widget == 0 ||
            context_.widget_library == 0) {
            return Panel(canvas, x, y, width, height, fallback);
        }
        struct CreateParameters {
            std::uintptr_t world_context;
            std::uintptr_t widget_type;
            std::uintptr_t owning_player;
            std::uintptr_t return_value;
        };
        CreateParameters create{};
        create.world_context = context_.outer;
        create.widget_type   = context_.backer_class;
        if (!CallFunction(context_.widget_library, context_.create_widget, &create).ok() ||
            create.return_value == 0) {
            return Panel(canvas, x, y, width, height, fallback);
        }
        if (!Place(canvas, create.return_value, x, y, width, height)) {
            return 0;
        }
        return create.return_value;
    }

    /// A nested canvas, so a section can be positioned as a unit.
    [[nodiscard]] std::uintptr_t Canvas(std::uintptr_t parent, float x, float y, float width,
                                        float height) const {
        const std::uintptr_t canvas = Spawn(context_.canvas_class);
        if (canvas == 0 || !Place(parent, canvas, x, y, width, height)) {
            return 0;
        }
        return canvas;
    }

    void SetText(std::uintptr_t block, std::string_view value) const {
        // The FText itself still comes from the engine, because it owns shared text data
        // that cannot be built by hand. Where it goes is different: it is written straight
        // into the text block's Text property rather than passed to SetText.
        //
        // SetText exists on several unrelated classes, and calling the wrong one on a text
        // block faulted reading 0xffffffffffffffff. Writing the property has no such
        // ambiguity, and the offset comes from the class's own reflection.
        constexpr std::uintptr_t kTextOffset = 0x188;

        std::wstring wide;
        wide.reserve(value.size() + 1);
        for (const char character : value) {
            wide.push_back(static_cast<wchar_t>(character));
        }

        struct FStringLayout {
            wchar_t*     data;
            std::int32_t count;
            std::int32_t capacity;
        };
        struct ConvertParameters {
            FStringLayout input;
            std::uint8_t  result[0x10];
        };
        ConvertParameters convert{};
        convert.input.data     = wide.data();
        convert.input.count    = static_cast<std::int32_t>(wide.size() + 1);
        convert.input.capacity = convert.input.count;
        if (!CallFunction(context_.text_library, context_.convert_function, &convert).ok()) {
            return;
        }
        (void)memory::GuardedWrite(block + kTextOffset, convert.result, sizeof(convert.result));
    }

    /// Writes a text block's colour and size.
    ///
    /// Two fields matter, not one. FSlateColor carries the colour and a rule saying whether
    /// to use it: the rule defaults to taking the foreground colour instead, so writing only
    /// the colour changes nothing. The font size is set explicitly because a size of zero
    /// draws nothing at all.
    ///
    ///   TextBlock +0x1A8 ColorAndOpacity  ( +0x00 SpecifiedColor, +0x10 ColorUseRule )
    ///   TextBlock +0x1D0 Font             ( +0x48 Size )
    void SetTextAppearance(std::uintptr_t block, LinearColour colour, float size) const {
        constexpr std::uintptr_t kColourOffset = 0x1A8;
        constexpr std::uintptr_t kRuleOffset   = 0x1A8 + 0x10;
        constexpr std::uintptr_t kFontSize     = 0x1D0 + 0x48;
        constexpr std::uint8_t   kUseSpecified = 0;

        (void)memory::GuardedWrite(block + kColourOffset, &colour, sizeof(colour));
        (void)memory::GuardedWrite(block + kRuleOffset, &kUseSpecified, sizeof(kUseSpecified));

        // The game's font first, then the size, in that order: the template carries the
        // size the block it was taken from happened to use, so writing it afterwards is
        // what keeps each line the size this layout asked for.
        if (context_.has_font) {
            (void)memory::GuardedWrite(block + 0x1D0, context_.font_template.data(),
                                       context_.font_template.size());
        }
        (void)memory::GuardedWrite(block + kFontSize, &size, sizeof(size));
    }

private:
    /// Makes a border draw a solid rectangle in a colour.
    ///
    /// Colour alone is not enough. A newly created border's background brush has DrawAs set
    /// to nothing, so it renders no pixels whatever colour it is told to be. That is why an
    /// entire lobby could be built, attached and sized correctly and still show only the
    /// backdrop: every panel was present and drawing nothing.
    ///
    ///   Border +0x1C0 Background (FSlateBrush)
    ///     brush +0x08 TintColor ( +0x00 SpecifiedColor, +0x10 ColorUseRule )
    ///     brush +0x1C DrawAs
    ///   Border +0x280 BrushColor
    void SetBorderColour(std::uintptr_t border, LinearColour colour) const {
        constexpr std::uintptr_t kBackground   = 0x1C0;
        constexpr std::uintptr_t kTintColour   = kBackground + 0x08;
        constexpr std::uintptr_t kTintRule     = kBackground + 0x08 + 0x10;
        constexpr std::uintptr_t kDrawAs       = kBackground + 0x1C;
        constexpr std::uintptr_t kBrushColour  = 0x280;
        constexpr std::uint8_t   kDrawAsBox    = 1; ///< A filled rectangle.
        constexpr std::uint8_t   kUseSpecified = 0;

        (void)memory::GuardedWrite(border + kDrawAs, &kDrawAsBox, sizeof(kDrawAsBox));
        (void)memory::GuardedWrite(border + kTintColour, &colour, sizeof(colour));
        (void)memory::GuardedWrite(border + kTintRule, &kUseSpecified, sizeof(kUseSpecified));
        (void)memory::GuardedWrite(border + kBrushColour, &colour, sizeof(colour));
    }

    const LobbyUIContext& context_;
};

/// Draws one player card, filled or empty.
void DrawPlayerCard(const Builder& builder, std::uintptr_t canvas, float x, float y,
                    std::string_view name, bool occupied, bool host, LinearColour team,
                    std::vector<LobbyControl>* controls = nullptr,
                    LobbyAction invite = LobbyAction::None) {
    constexpr float kCardWidth  = 132.0F;
    constexpr float kCardHeight = 168.0F;

    (void)builder.Panel(canvas, x, y, kCardWidth, kCardHeight,
                        occupied ? kAccentDim : kSlot);

    if (occupied) {
        // A filled card carries the name and the role, with a team coloured strip so the
        // side is readable at a glance rather than only from the column heading.
        (void)builder.Panel(canvas, x, y, kCardWidth, 4.0F, team);
        (void)builder.Text(canvas, x + 8.0F, y + kCardHeight - 46.0F, kCardWidth - 16.0F,
                           22.0F, name, kText);
        (void)builder.Text(canvas, x + 8.0F, y + kCardHeight - 24.0F, kCardWidth - 16.0F,
                           18.0F, host ? "Owner" : "Player", kTextDim);
    } else if (controls != nullptr && invite != LobbyAction::None) {
        // An empty slot is a real button, so pressing it invites somebody into this
        // session rather than merely drawing a plus sign that does nothing.
        // The whole card is the target, but the plus is drawn rather than being the
        // button's own label.
        //
        // A button is scaled to fit its box, and the frontend's button art is wide, so a
        // card this narrow crushes it to about a fifth of its size and the label with it.
        // Drawing the plus as text and putting the button behind it, non interactive, gives
        // a glyph as large as the card allows while the whole card stays pressable.
        controls->push_back(
            {builder.Button(canvas, x, y, kCardWidth, kCardHeight, " ", kStretchFill),
             invite, 0});
        const std::uintptr_t plus =
            builder.Text(canvas, x + kCardWidth * 0.30F, y + kCardHeight * 0.28F,
                         kCardWidth, 80.0F, "+", kSlotMark, 72.0F);
        builder.SetVisibilityOf(plus, kHitTestInvisible);
    } else {
        (void)builder.Text(canvas, x + 8.0F, y + kCardHeight - 32.0F, kCardWidth - 16.0F,
                           20.0F, "+", kAccent);
    }
}

/// The HOST tab.
void DrawHostTab(const Builder& builder, std::uintptr_t canvas, const LobbyView& view,
                 std::vector<LobbyControl>& controls) {
    // Left: mode selection.
    (void)builder.Label(canvas, 56.0F, 180.0F, 400.0F, 72.0F, "GAME MODE SELECTION");
    const std::array<const char*, 2> modes = {"CAPTURE THE FLAG", "SLAYER"};
    float                            mode_y = 252.0F;
    for (std::size_t index = 0; index < modes.size(); ++index) {
        const char* mode = modes[index];
        // Behind the button, so the frontend's own button art still reads normally and the
        // bar only marks which one is chosen.
        g_mode_marker[index] = builder.Panel(canvas, 52.0F, mode_y - 3.0F, 376.0F, 72.0F,
                                             kAccentDim);
        controls.push_back(
            {builder.Button(canvas, 60.0F, mode_y, 360.0F, 66.0F, mode),
             std::string_view(mode) == "SLAYER" ? LobbyAction::SelectSlayer
                                                : LobbyAction::SelectCaptureTheFlag});
        mode_y += 82.0F;
    }


    // Left, below the modes: the map.
    (void)builder.Label(canvas, 56.0F, 424.0F, 400.0F, 72.0F, "MAP SELECTION");
    for (std::size_t index = 0; index < std::size(kLobbyMaps); ++index) {
        const float y = 490.0F + static_cast<float>(index) * 62.0F;
        g_map_marker[index] =
            builder.Panel(canvas, 56.0F, y - 3.0F, 308.0F, 60.0F, kAccentDim);
        controls.push_back({builder.Button(canvas, 60.0F, y, 300.0F, 54.0F,
                                           kLobbyMaps[index].label),
                            LobbyAction::SelectMap, static_cast<int>(index)});
    }

    // Middle: the two team columns, five slots each in two rows.
    struct Column {
        const char*                     title;
        LinearColour                    colour;
        const std::vector<std::string>* players;
        float                           x;
    };
    const std::array<Column, 2> columns = {
        Column{"BLUE TEAM", kBlue, &view.blue, 470.0F},
        Column{"RED TEAM", kRed, &view.red, 940.0F},
    };

    for (const Column& column : columns) {
        (void)builder.Label(canvas, column.x - 4.0F, 180.0F, 400.0F, 72.0F,
                            std::format("{} ({}/5)", column.title,
                                        column.players->size()));
        for (int slot = 0; slot < 5; ++slot) {
            const float x = column.x + static_cast<float>(slot % 3) * 144.0F;
            const float y = 250.0F + static_cast<float>(slot / 3) * 180.0F;
            const bool  occupied = slot < static_cast<int>(column.players->size());
            const std::string name = occupied ? (*column.players)[static_cast<std::size_t>(slot)]
                                              : std::string{};
            DrawPlayerCard(builder, canvas, x, y, name, occupied,
                           occupied && name == view.host_name, column.colour, &controls,
                           column.colour.r > column.colour.b ? LobbyAction::InviteRed
                                                             : LobbyAction::InviteBlue);
        }
    }

    // Right: settings and server name.
    (void)builder.Backer(canvas, 1440.0F, 250.0F, 440.0F, 190.0F, kPanelLight);
    (void)builder.Label(canvas, 1444.0F, 172.0F, 440.0F, 74.0F, "LOBBY SETTINGS");
    (void)builder.Label(canvas, 1456.0F, 262.0F, 408.0F, 54.0F,
                        std::format("GAME TIME: {}min", view.game_time_minutes));
    (void)builder.Label(canvas, 1456.0F, 320.0F, 408.0F, 54.0F,
                        std::format("FRIENDLY FIRE: {}", view.friendly_fire ? "ON" : "OFF"));
    (void)builder.Label(canvas, 1456.0F, 378.0F, 408.0F, 54.0F,
                        std::format("RESPAWN TIME: {}s", view.respawn_seconds));

    (void)builder.Panel(canvas, 1452.0F, 498.0F, 416.0F, 68.0F, kPanelLight);
    (void)builder.Text(canvas, 1456.0F, 462.0F, 400.0F, 28.0F, "SERVER NAME", kAccent,
                       18.0F);
    g_server_name_field = builder.Field(canvas, 1468.0F, 512.0F, 384.0F, 40.0F,
                                        view.server_name);

    // Bottom right action.
    controls.push_back({builder.Button(canvas, 1380.0F, 916.0F, 500.0F, 104.0F, "START MATCH"),
                        LobbyAction::StartMatch});
    controls.push_back({builder.Button(canvas, 60.0F, 946.0F, 500.0F, 104.0F, "BACK"),
                        LobbyAction::Back, 0});
}

/// The BROWSE tab.
///
/// Everything here is built once. Filtering and refreshing rewrite the rows in place
/// through SetLobbyServers rather than building the screen again, so the browser stays as
/// instant as the rest of the lobby no matter how often the filters are changed.
void DrawBrowseTab(const Builder& builder, std::uintptr_t canvas, const LobbyView& view,
                   std::vector<LobbyControl>& controls) {
    // Left: filters. Each option is a real button, so the filter is a control rather than
    // a picture of one, with a marker behind the active choice.
    (void)builder.Label(canvas, 56.0F, 176.0F, 380.0F, 72.0F, "SERVER FILTERS");

    struct FilterRow {
        const char*                      caption;
        float                            y;
        std::array<const char*, 3>       options;
        std::array<LobbyAction, 3>       actions;
        std::uintptr_t*                  markers;
    };
    const std::array<FilterRow, 3> rows = {
        FilterRow{"MODE", 232.0F, {"ANY", "CTF", "SLAYER"},
                  {LobbyAction::FilterModeAny, LobbyAction::FilterModeCaptureTheFlag,
                   LobbyAction::FilterModeSlayer},
                  g_filter_mode},
        FilterRow{"SLOTS", 468.0F, {"ANY", "OPEN", "FULL"},
                  {LobbyAction::FilterSlotsAny, LobbyAction::FilterSlotsOpen,
                   LobbyAction::FilterSlotsFull},
                  g_filter_slots},
        FilterRow{"PING", 704.0F, {"ANY", "<50ms", "<100ms"},
                  {LobbyAction::FilterPingAny, LobbyAction::FilterPingUnder50,
                   LobbyAction::FilterPingUnder100},
                  g_filter_ping},
    };

    // Stacked, not in a row.
    //
    // Three options side by side left each about a hundred wide, and a button scaled into
    // a box that narrow renders its label unreadably small. Down the column each one gets
    // the full width, so the text is as large as the panel allows.
    for (const FilterRow& row : rows) {
        // Ruled, not merely larger.
        //
        // Size alone did not separate a heading from the buttons under it. A line above and
        // below makes the grouping structural: everything between two rules belongs to the
        // heading at the top of them.
        (void)builder.Panel(canvas, 56.0F, row.y - 8.0F, 308.0F, 2.0F, kAccent);
        (void)builder.Text(canvas, 64.0F, row.y + 10.0F, 300.0F, 30.0F, row.caption,
                           kAccent, 20.0F);
        (void)builder.Panel(canvas, 56.0F, row.y + 52.0F, 308.0F, 2.0F, kAccentDim);
        for (std::size_t option = 0; option < row.options.size(); ++option) {
            const float y = row.y + 66.0F + static_cast<float>(option) * 52.0F;
            row.markers[option] =
                builder.Panel(canvas, 56.0F, y - 3.0F, 308.0F, 52.0F, kAccentDim);
            controls.push_back({builder.Button(canvas, 60.0F, y, 300.0F, 46.0F,
                                               row.options[option]),
                                row.actions[option], 0});
        }
    }

    // Middle: the table. Headings in the frontend's own art, then eight permanent rows.
    const std::array<const char*, 5> headings = {"SERVER", "MODE", "MAP", "PLAYERS", "PING"};
    // Spaced so the widest heading fits its own column. PLAYERS is the long one, so the
    // gap after it is the one that matters; at thirty point it ran straight into PING.
    const std::array<float, 5> columns = {400.0F, 800.0F, 980.0F, 1120.0F, 1290.0F};
    const std::array<float, 5> widths  = {390.0F, 170.0F, 130.0F, 160.0F, 110.0F};

    // Text, not the button art.
    //
    // A label is a button scaled to fit its box, and a column is under two hundred wide, so
    // whatever size is asked for it comes out at about a third of it. A heading is not a
    // button, so it is drawn as text in the game's own font, where the size is the size.
    for (std::size_t index = 0; index < headings.size(); ++index) {
        (void)builder.Text(canvas, columns[index], 206.0F, widths[index], 26.0F,
                           headings[index], kAccent, 17.0F);
    }
    (void)builder.Panel(canvas, 390.0F, 244.0F, 1000.0F, 2.0F, kAccentDim);

    float row_y = 258.0F;
    for (std::size_t index = 0; index < kServerRows; ++index) {
        ServerRowWidgets& row = g_server_row[index];
        row.highlight = builder.Panel(canvas, 390.0F, row_y, 1000.0F, 62.0F, kAccentDim);

        // The pressable area goes down before the text, so the text draws over it rather
        // than being hidden behind it, and the text is made non interactive so the click
        // still reaches the row underneath. The whole row is the target: a server is
        // chosen by clicking it, not by a separate control that would need explaining.
        row.button = builder.Button(canvas, 390.0F, row_y, 1000.0F, 62.0F, " ",
                                    kStretchFill);
        controls.push_back({row.button, LobbyAction::SelectServer, static_cast<int>(index)});

        row.name    = builder.Text(canvas, columns[0] + 10.0F, row_y + 18.0F, widths[0],
                                   26.0F, "", kText, 24.0F);
        row.mode    = builder.Text(canvas, columns[1], row_y + 18.0F, widths[1], 26.0F, "",
                                   kTextDim, 22.0F);
        row.map     = builder.Text(canvas, columns[2], row_y + 18.0F, widths[2], 26.0F, "",
                                   kTextDim, 22.0F);
        row.players = builder.Text(canvas, columns[3], row_y + 18.0F, widths[3], 26.0F, "",
                                   kTextDim, 22.0F);
        row.ping    = builder.Text(canvas, columns[4], row_y + 18.0F, widths[4], 26.0F, "",
                                   kTextDim, 22.0F);
        for (const std::uintptr_t block : {row.name, row.mode, row.map, row.players,
                                           row.ping}) {
            builder.SetVisibilityOf(block, kHitTestInvisible);
        }
        row_y += 70.0F;
    }

    g_empty_notice = builder.Text(canvas, 400.0F, 300.0F, 900.0F, 40.0F,
                                  "No games found. Host one, or refresh.", kTextDim, 26.0F);

    // Right: details of the highlighted server.
    (void)builder.Backer(canvas, 1440.0F, 250.0F, 440.0F, 330.0F, kPanelLight);
    (void)builder.Label(canvas, 1444.0F, 172.0F, 440.0F, 74.0F, "SERVER DETAILS");
    for (std::size_t line = 0; line < std::size(g_detail_line); ++line) {
        g_detail_line[line] =
            builder.Text(canvas, 1462.0F, 266.0F + static_cast<float>(line) * 40.0F, 400.0F,
                         30.0F, "", kTextDim, 24.0F);
    }

    controls.push_back({builder.Button(canvas, 1380.0F, 916.0F, 500.0F, 104.0F, "JOIN MATCH"),
                        LobbyAction::JoinMatch, 0});
    controls.push_back({builder.Button(canvas, 60.0F, 946.0F, 500.0F, 104.0F, "BACK"),
                        LobbyAction::Back, 0});

    SetLobbyServers(builder.Context(), view.servers, view.selected_server);
}

} // namespace

namespace {
/// The lobby currently on screen, so it can be replaced or closed.
std::uintptr_t g_open_lobby_root = 0;
/// The user widget carrying it, which is what the viewport actually holds.
std::uintptr_t g_open_lobby_widget = 0;

/// The host user widget in the viewport. Removing this takes the whole lobby with it.
std::uintptr_t g_open_host_widget = 0;

/// The two tab canvases. Both exist at once; visibility decides which is on screen.
std::uintptr_t g_host_tab   = 0;
std::uintptr_t g_browse_tab = 0;


/// Creates a user widget, gives it a canvas as its content, and shows it.
///
/// The canvas is installed as the widget's tree root before the widget is added, because
/// adding it is what builds the underlying Slate representation: set the root afterwards
/// and the widget is already built around whatever it had before.
///
///   UserWidget +0x268 WidgetTree, WidgetTree +0x30 RootWidget
/// Attaches a correctly sized canvas into the game's own menu.
///
/// Native throughout: the canvas becomes a child of the main menu's existing root, so the
/// game draws, scales and owns it like any other part of that screen. No separate window,
/// no borrowed developer widget, and nothing added to the viewport on its own.
///
/// Two things are done explicitly because leaving them to defaults is what produced a
/// screen that built cleanly and drew nothing:
///
///   A canvas panel reports no desired size. Placed in an overlay it is given zero unless
///   the slot stretches it, so it is wrapped in a size box with a real width and height.
///   The size box's override flags share one bitfield at +0x1B0; without those bits the
///   values at +0x190 and +0x194 are ignored.
///
///   The overlay slot's alignment is written directly at +0x50 and +0x51 rather than
///   through setters. Those setters exist on several slot types, and one resolved against
///   the wrong class fails silently, leaving the alignment at its default.
/// ESlateVisibility. Collapsed takes the widget out of layout as well as out of the draw,
/// which is what a screen change should do; Hidden would leave it occupying its space.
constexpr std::uint8_t kVisible   = 0;
constexpr std::uint8_t kCollapsed = 1;
/// Drawn, but does not answer the mouse itself; its children still do.
///
/// The tab canvases cover the whole screen and are added after the HOST and BROWSE buttons,
/// so they sit on top of them. Hit testable, a canvas swallows the click before it reaches
/// the button underneath, which left exactly those two buttons dead while every button
/// inside a tab kept working.
constexpr std::uint8_t kSelfHitTestInvisible = 4;

/// The menu widgets folded away while the lobby is up, so they can be brought back.
std::vector<std::uintptr_t> g_folded;

void SetWidgetVisibility(const LobbyUIContext& context, std::uintptr_t widget,
                         std::uint8_t visibility) {
    if (context.set_visibility == 0 || widget == 0) {
        return;
    }
    struct Parameters {
        std::uint8_t visibility;
    };
    Parameters parameters{visibility};
    (void)CallFunction(widget, context.set_visibility, &parameters);
}

/// Folds the main menu away, leaving the lobby as the only thing on the root.
///
/// This is what makes the entry a screen rather than a panel drawn over the menu. Without
/// it the campaign list, the fireteam panel and the last played card stay on screen behind
/// the lobby, which is not a multiplayer screen, it is the main menu with something on top
/// of it.
///
/// The children are read from the root rather than taken from a list of offsets, so this
/// stays correct if the menu is laid out differently than expected.
void FoldMenuAway(const LobbyUIContext& context) {
    // The whole menu widget, not the children of its root.
    //
    // Collapsing the children hid everything that was drawn and still left the menu itself
    // in the viewport, alive and handling input: moving the mouse went on playing its hover
    // sounds for buttons that were no longer on screen, and two of its panels were not
    // under that root at all so they stayed visible. The lobby is hosted in the viewport
    // now rather than inside the menu, so the menu can be collapsed whole, which takes its
    // drawing, its layout and its input with it in one call.
    if (context.outer == 0) {
        return;
    }
    SetWidgetVisibility(context, context.outer, kCollapsed);
    g_folded.push_back(context.outer);

    for (const std::uintptr_t widget : context.also_fold) {
        SetWidgetVisibility(context, widget, kCollapsed);
        g_folded.push_back(widget);
    }
    FE_LOG_INFO("main menu 0x{:X} collapsed whole, with {} widget(s) beside it",
                context.outer, context.also_fold.size());
}

/// Points input at the lobby instead of the menu underneath it.
///
/// Folding the menu away hides it, and a collapsed widget cannot be hit tested, but the
/// frontend keeps handling input at the controller level: moving the mouse still played the
/// menu's hover sounds for buttons that were no longer on screen. Taking focus is what
/// makes the lobby a screen the player is actually on rather than a picture over one.
void FocusLobby(const LobbyUIContext& context, std::uintptr_t widget) {
    if (context.set_input_mode_ui == 0 || context.get_player_controller == 0 ||
        context.widget_library == 0 || widget == 0) {
        FE_LOG_WARN("input cannot be given to the lobby: focus functions were not resolved");
        return;
    }

    struct ControllerParameters {
        std::uintptr_t world_context;
        std::int32_t   player_index;
        std::uintptr_t return_value;
    };
    ControllerParameters controller{};
    controller.world_context = context.outer;
    controller.player_index  = 0;
    if (!CallFunction(context.gameplay_statics, context.get_player_controller, &controller)
             .ok() ||
        controller.return_value == 0) {
        FE_LOG_WARN("no player controller to take input from");
        return;
    }

    //   SetInputMode_UIOnlyEx(PlayerController, WidgetToFocus, MouseLockMode, bFlushInput)
    // The flush matters: without it the press that opened the lobby is still in the queue
    // and arrives at the new screen.
    struct InputModeParameters {
        std::uintptr_t player_controller;
        std::uintptr_t widget_to_focus;
        std::uint8_t   mouse_lock_mode;
        bool           flush_input;
    };
    InputModeParameters mode{};
    mode.player_controller = controller.return_value;
    mode.widget_to_focus   = widget;
    mode.mouse_lock_mode   = 0; // DoNotLock
    mode.flush_input       = true;
    (void)CallFunction(context.widget_library, context.set_input_mode_ui, &mode);

    if (context.set_keyboard_focus != 0) {
        (void)CallFunction(widget, context.set_keyboard_focus, nullptr);
    }
    FE_LOG_INFO("input focus moved to the lobby (controller 0x{:X})",
                controller.return_value);
}

void UnfoldMenu(const LobbyUIContext& context) {
    for (const std::uintptr_t widget : g_folded) {
        SetWidgetVisibility(context, widget, kVisible);
    }
    if (!g_folded.empty()) {
        FE_LOG_INFO("restored {} main menu widget(s)", g_folded.size());
    }
    g_folded.clear();
}

[[nodiscard]] std::uintptr_t CreateHostedCanvas(const LobbyUIContext& context) {
    constexpr std::uintptr_t kWidthOverride   = 0x190;
    constexpr std::uintptr_t kHeightOverride  = 0x194;
    constexpr std::uintptr_t kOverrideFlags   = 0x1B0;
    constexpr std::uint8_t   kWidthAndHeight  = 0x03; // bOverride_Width | bOverride_Height
    constexpr std::uintptr_t kSlotHorizontal  = 0x50;
    constexpr std::uintptr_t kSlotVertical    = 0x51;
    constexpr std::uint8_t   kFill            = 0;

    struct SpawnParameters {
        std::uintptr_t object_class;
        std::uintptr_t outer;
        std::uintptr_t return_value;
    };
    const auto spawn = [&](std::uintptr_t widget_class) -> std::uintptr_t {
        SpawnParameters parameters{};
        parameters.object_class = widget_class;
        parameters.outer        = context.outer;
        if (!CallFunction(context.gameplay_statics, context.spawn_object, &parameters).ok()) {
            return 0;
        }
        return parameters.return_value;
    };

    const std::uintptr_t frame = spawn(context.sizebox_class);
    if (frame == 0) {
        FE_LOG_WARN("could not create the sizing frame");
        return 0;
    }
    const float width  = kDesignWidth;
    const float height = kDesignHeight;
    (void)memory::GuardedWrite(frame + kWidthOverride, &width, sizeof(width));
    (void)memory::GuardedWrite(frame + kHeightOverride, &height, sizeof(height));
    (void)memory::GuardedWrite(frame + kOverrideFlags, &kWidthAndHeight,
                               sizeof(kWidthAndHeight));

    const std::uintptr_t canvas = spawn(context.canvas_class);
    if (canvas == 0) {
        FE_LOG_WARN("could not create the lobby canvas");
        return 0;
    }

    // Canvas inside the frame.
    struct AddParameters {
        std::uintptr_t content;
        std::uintptr_t return_value;
    };
    AddParameters inner{};
    inner.content = canvas;
    if (context.add_child == 0 ||
        !CallFunction(frame, context.add_child, &inner).ok() || inner.return_value == 0) {
        FE_LOG_WARN("the sizing frame did not accept the canvas");
        return 0;
    }

    // Frame into a scale box, so the fixed design resizes to the viewport.
    //
    // Without this the size box holds the lobby at exactly 1920 by 1080 regardless of the
    // display, which on anything larger drew the whole screen in the top left corner at one
    // to one with the game still visible around it.
    const std::uintptr_t scaler = spawn(context.scalebox_class);
    if (scaler == 0) {
        FE_LOG_WARN("could not create the scaling box");
        return 0;
    }
    AddParameters scaled{};
    scaled.content = frame;
    if (!CallFunction(scaler, context.add_child, &scaled).ok() || scaled.return_value == 0) {
        FE_LOG_WARN("the scaling box did not accept the lobby frame");
        return 0;
    }
    // EStretch::ScaleToFit keeps the design's proportions and fits it to the viewport.
    struct StretchParameters {
        std::uint8_t stretch;
    };
    StretchParameters stretch{2};
    (void)CallFunction(scaler, context.set_stretch, &stretch);

    // The scale box goes into the viewport inside a host widget, not into the menu.
    //
    // A user widget is the only thing the viewport takes, and its Slate is built when it is
    // added rather than when it is created, so its tree root can be swapped for the lobby
    // in between. That ordering is the whole trick: set the root afterwards and the widget
    // has already been built around whatever it had before.
    //
    //   UserWidget +0x268 WidgetTree, WidgetTree +0x30 RootWidget
    struct CreateParameters {
        std::uintptr_t world_context;
        std::uintptr_t widget_type;
        std::uintptr_t owning_player;
        std::uintptr_t return_value;
    };
    CreateParameters created{};
    created.world_context = context.outer;
    created.widget_type   = context.host_class;
    if (context.create_widget == 0 || context.host_class == 0 ||
        context.add_to_viewport == 0 ||
        !CallFunction(context.widget_library, context.create_widget, &created).ok() ||
        created.return_value == 0) {
        FE_LOG_WARN("could not create the widget that carries the lobby into the viewport");
        return 0;
    }

    std::uintptr_t host_tree = 0;
    if (!memory::GuardedRead(created.return_value + 0x268, &host_tree, sizeof(host_tree)) ||
        host_tree == 0 ||
        !memory::GuardedWrite(host_tree + 0x30, &scaler, sizeof(scaler))) {
        FE_LOG_WARN("the host widget 0x{:X} would not take the lobby as its root",
                    created.return_value);
        return 0;
    }

    // Above the menu, so nothing of the frontend can draw over it.
    struct ViewportParameters {
        std::int32_t z_order;
    };
    ViewportParameters viewport{1000};
    if (!CallFunction(created.return_value, context.add_to_viewport, &viewport).ok()) {
        FE_LOG_WARN("the lobby host was not accepted by the viewport");
        return 0;
    }
    g_open_host_widget = created.return_value;

    g_open_lobby_widget = scaler;

    // What the viewport is actually handing out, which is the number that decides whether
    // the design scales up or down. Guessing at this is what produced a half sized screen.
    struct SizeParameters {
        std::uintptr_t world_context;
        double         x;
        double         y;
    };
    SizeParameters viewport_size{};
    viewport_size.world_context = context.outer;
    if (context.get_viewport_size != 0 && context.layout_library != 0) {
        (void)CallFunction(context.layout_library, context.get_viewport_size, &viewport_size);
    }

    FE_LOG_INFO("lobby hosted in the viewport: host 0x{:X}, scaler 0x{:X}, frame 0x{:X} "
                "({}x{} design), canvas 0x{:X}; viewport is {:.0f}x{:.0f}",
                created.return_value, scaler, frame, static_cast<int>(width),
                static_cast<int>(height), canvas, viewport_size.x, viewport_size.y);

    // Neither folded nor focused here. The lobby is built ahead of time and kept hidden,
    // so the menu must stay exactly as it is until the screen is actually shown.
    return canvas;
}

} // namespace

Result ProbeLobbyUI(const LobbyUIContext& context) {
    if (!context.Complete()) {
        return Result::Fail(ErrorCode::InvalidState, "the lobby UI context is incomplete");
    }
    RemoveLobbyUI(context);

    const std::uintptr_t canvas = CreateHostedCanvas(context);
    if (canvas == 0) {
        return Result::Fail(ErrorCode::InvalidState, "could not host a canvas");
    }
    g_open_lobby_root = canvas;

    const Builder builder(context);
    // Deliberately garish and large: the point is to be impossible to miss if it draws.
    if (builder.Panel(canvas, 200.0F, 200.0F, 900.0F, 500.0F, {1.0F, 0.0F, 0.0F, 1.0F}) == 0) {
        return Result::Fail(ErrorCode::InvalidState, "the probe rectangle was not created");
    }
    (void)builder.Text(canvas, 240.0F, 240.0F, 800.0F, 60.0F, "FORGE EVOLVED UI PROBE",
                       {1.0F, 1.0F, 1.0F, 1.0F}, 40.0F);
    FE_LOG_INFO("probe drawn: a red rectangle with white text should now be on screen");
    return Result::Success();
}

void RemoveLobbyUI(const LobbyUIContext& context) {
    // The viewport holds the hosting widget, so that is what has to be removed. Removing
    // the canvas inside it would leave an empty widget still on screen.
    // The host is what the viewport holds, so removing it takes the scale box, the frame,
    // the canvas and everything on it away in one call.
    if (g_open_host_widget != 0 && context.remove_from_parent != 0) {
        (void)CallFunction(g_open_host_widget, context.remove_from_parent, nullptr);
    }
    g_open_host_widget  = 0;
    g_open_lobby_widget = 0;
    g_open_lobby_root   = 0;
    g_host_tab          = 0;
    g_browse_tab        = 0;

    // The menu comes back with the lobby's departure, not on a separate call, so there is
    // no path that closes the lobby and leaves the player looking at an empty screen.
    UnfoldMenu(context);
}

std::uintptr_t OpenLobbyFrame() { return g_open_lobby_widget; }

void MeasureLobby(const LobbyUIContext& context) {
    if (context.get_desired_size == 0 || g_open_lobby_widget == 0) {
        return;
    }
    // FVector2D is double precision in UE5, so this is two doubles rather than two floats.
    struct SizeParameters {
        double x;
        double y;
    };
    SizeParameters frame_size{};
    SizeParameters canvas_size{};
    (void)CallFunction(g_open_lobby_widget, context.get_desired_size, &frame_size);
    if (g_open_lobby_root != 0) {
        (void)CallFunction(g_open_lobby_root, context.get_desired_size, &canvas_size);
    }
    // Read a frame after building, because desired size is whatever the last layout pass
    // cached and a widget created this frame has not been through one yet.
    FE_LOG_INFO("lobby measured: frame {:.0f}x{:.0f}, canvas {:.0f}x{:.0f}", frame_size.x,
                frame_size.y, canvas_size.x, canvas_size.y);
}

Result ResolveLobbyStatics(const ObjectArray& objects, LobbyUIContext& out_context) {
    LobbyUIContext context;

    // The game's own widget classes, listed once.
    //
    // The lobby is meant to be built from the game's widgets rather than from engine
    // primitives dressed to look like them, and that needs to start from what actually
    // exists in this build rather than from names guessed off a wiki.
    std::vector<std::string> game_widgets;

    /// Candidates for the font, and the game widgets they might belong to.
    std::vector<ObjectInfo>          text_blocks;
    std::unordered_set<std::uintptr_t> game_widget_instances;

    /// Everything needed to resolve a function by name and owning class, gathered in the
    /// one pass this function already makes.
    ///
    /// Each owner qualified lookup used to be its own scan of fifty thousand objects, and
    /// there are nine of them. That was fifteen seconds, and because the menu entry is only
    /// added once this has finished, it was fifteen seconds during which the main menu sat
    /// there without a MULTIPLAYER entry on it. A function's outer is the class that
    /// declares it, so keeping the classes by address and the functions by outer turns all
    /// nine lookups into map lookups over data already in hand.
    struct FunctionRecord {
        std::string    name;
        std::uintptr_t outer{0};
        std::uintptr_t address{0};
    };
    std::vector<FunctionRecord>                     functions;
    std::unordered_map<std::uintptr_t, std::string> class_names;

    // One pass over the object array. Doing a pass per lookup is what made the first lobby
    // stall the game noticeably.
    objects.ForEach([&](const ObjectInfo& object) {
        const bool is_default = object.name.rfind("Default__", 0) == 0;
        const bool is_class   = object.class_name.find("Class") != std::string::npos;

        if (object.class_name == "Function") {
            functions.emplace_back(FunctionRecord{object.name, object.outer_address,
                                                  object.address});
            // Only the unambiguous names are taken here. SetText, SetPosition, SetSize and
            // SetColorAndOpacity each exist on several unrelated classes, and calling one
            // class's function on another object is not a wrong result, it is a crash: an
            // earlier version picked whichever matched first and faulted reading
            // 0xffffffffffffffff inside the text block. Those are resolved by owner below.
            if (!context.spawn_object && object.name == "SpawnObject") {
                context.spawn_object = object.address;
            } else if (!context.convert_function && object.name == "Conv_StringToText") {
                context.convert_function = object.address;
            }
        } else if (is_default) {
            if (object.name == "Default__GameplayStatics") {
                context.gameplay_statics = object.address;
            } else if (object.name == "Default__KismetTextLibrary") {
                context.text_library = object.address;
            } else if (object.name == "Default__WidgetBlueprintLibrary") {
                context.widget_library = object.address;
            } else if (object.name == "Default__WidgetLayoutLibrary") {
                context.layout_library = object.address;
            }
        } else if (object.class_name == "TextBlock") {
            // Kept for a second pass rather than taken here. Any text block will hand over
            // a font, but most of them hand over the engine default: taking the first one
            // found produced a lobby in Roboto, which is exactly what it looked like. Only
            // a block belonging to one of the game's own widgets carries the menu typeface,
            // and whether it does cannot be told without knowing what owns it.
            text_blocks.push_back(object);
        } else if (!is_class && object.name.rfind("WBP_", 0) == 0) {
            // Instances only. Without the class check this arm also catches the classes,
            // which are named the same way, and they then never reach the arm below that
            // resolves them: the host widget and the button both came back null and the
            // lobby failed to open at all.
            game_widget_instances.insert(object.address);
        } else if (is_class) {
            class_names.emplace(object.address, object.name);
            if (object.name.rfind("WBP_", 0) == 0 || object.name.rfind("W_", 0) == 0) {
                game_widgets.push_back(object.name);
            }
            if (object.name == "CanvasPanel") {
                context.canvas_class = object.address;
            } else if (object.name == "TextBlock") {
                context.text_class = object.address;
            } else if (object.name == "Border") {
                context.border_class = object.address;
            } else if (object.name == "SizeBox") {
                context.sizebox_class = object.address;
            } else if (object.name == "ScaleBox") {
                context.scalebox_class = object.address;
            } else if (object.name == "EditableText") {
                // EditableText, not EditableTextBox.
                //
                // The box variant ships a white background brush and a grey text style,
                // which is why the field looked like it belonged to a different program.
                // The plain variant draws only the text and the caret, so the panel behind
                // it is ours and the whole thing matches the rest of the screen.
                context.editable_class = object.address;
            } else if (object.name == "WBP_MeteoriteStandaloneButtonDefault_C") {
                context.button_class = object.address;
            } else if (object.name == "WBP_FrontendMenuBacker_C") {
                // The frontend's own panel backing. A border filled with a colour picked by
                // eye is the last part of the lobby that is not the game's, and this is
                // what the game puts behind its own menu panels.
                context.backer_class = object.address;
            } else if (object.name == "WBP_FadeOverlay_C") {
                // Only ever used as an empty shell to carry the lobby into the viewport:
                // its own tree is replaced before it is shown. A full screen overlay is
                // the right shape for that and has no layout of its own worth keeping.
                context.host_class = object.address;
            }
        }
        return true;
    });

    // Resolved against the class that declares them, so each is the one that belongs to the
    // object it will be called on.
    //
    //   SetText              on TextBlock, not EditableText or RichTextBlock
    //   SetColorAndOpacity   on TextBlock
    //   SetPosition/SetSize  on CanvasPanelSlot, whose SetSize takes a Vector2D; the
    //                        HorizontalBox slot's SetSize takes a SlateChildSize instead
    //   AddChildToCanvas     on CanvasPanel
    //   AddChild             on PanelWidget, which is what makes it work for any panel
    // Owner qualified, because the names are not unique. SetText exists on four unrelated
    // classes and SetPosition on five, and calling one class's function on another object
    // is not a wrong answer, it is a crash: an earlier version took whichever matched first
    // and faulted reading 0xffffffffffffffff inside a text block.
    const auto find = [&](std::string_view name, std::string_view owner) -> std::uintptr_t {
        for (const FunctionRecord& function : functions) {
            if (function.name != name) {
                continue;
            }
            const auto owner_name = class_names.find(function.outer);
            if (owner_name != class_names.end() && owner_name->second == owner) {
                return function.address;
            }
        }
        return 0;
    };

    context.set_text              = find("SetText", "TextBlock");
    context.set_color_and_opacity = find("SetColorAndOpacity", "TextBlock");
    context.set_position          = find("SetPosition", "CanvasPanelSlot");
    context.set_size              = find("SetSize", "CanvasPanelSlot");
    context.add_to_canvas         = find("AddChildToCanvas", "CanvasPanel");
    context.add_child             = find("AddChild", "PanelWidget");
    context.set_horizontal_alignment = find("SetHorizontalAlignment", "OverlaySlot");
    context.set_vertical_alignment   = find("SetVerticalAlignment", "OverlaySlot");
    context.remove_from_parent    = find("RemoveFromParent", "Widget");
    context.set_visibility        = find("SetVisibility", "Widget");
    context.get_children_count    = find("GetChildrenCount", "PanelWidget");
    context.get_child_at          = find("GetChildAt", "PanelWidget");
    context.get_desired_size      = find("GetDesiredSize", "Widget");
    context.set_stretch           = find("SetStretch", "ScaleBox");
    context.set_keyboard_focus    = find("SetKeyboardFocus", "Widget");
    context.get_player_controller = find("GetPlayerController", "GameplayStatics");
    context.set_input_mode_ui  = find("SetInputMode_UIOnlyEx", "WidgetBlueprintLibrary");
    context.create_widget      = find("Create", "WidgetBlueprintLibrary");
    context.add_to_viewport    = find("AddToViewport", "UserWidget");
    context.get_viewport_size  = find("GetViewportSize", "WidgetLayoutLibrary");
    context.get_editable_text  = find("GetText", "EditableText");
    context.text_to_string     = find("Conv_TextToString", "KismetTextLibrary");

    // The font the game's own text is set in, chosen by which one most of it uses.
    //
    // Deciding by ownership was tried first and found nothing at all: it walked from a text
    // block to its outer's outer expecting the user widget, and that chain does not hold
    // here, so all forty nine candidates were rejected and the lobby stayed in Roboto.
    //
    // Counting needs no assumption about how widgets are parented. A frontend has dozens of
    // text blocks and they are nearly all set in its UI font, while the handful the engine
    // itself creates are not, so the most used font asset is the game's by a wide margin.
    //   TextBlock +0x1D0 Font, FSlateFontInfo +0x00 FontObject
    std::unordered_map<std::uintptr_t, int>            font_uses;
    std::unordered_map<std::uintptr_t, std::uintptr_t> font_example;
    for (const ObjectInfo& block : text_blocks) {
        std::uintptr_t font_object = 0;
        if (!memory::GuardedRead(block.address + 0x1D0, &font_object, sizeof(font_object)) ||
            font_object == 0) {
            continue;
        }
        ++font_uses[font_object];
        font_example.try_emplace(font_object, block.address);
    }

    std::uintptr_t best_font = 0;
    int            best_uses = 0;
    for (const auto& [font_object, uses] : font_uses) {
        if (uses > best_uses) {
            best_uses = uses;
            best_font = font_object;
        }
    }
    if (best_font != 0 &&
        memory::GuardedRead(font_example[best_font] + 0x1D0, context.font_template.data(),
                            context.font_template.size())) {
        context.has_font = true;
        FE_LOG_INFO("font asset 0x{:X} taken from text block 0x{:X}; {} of {} block(s) use "
                    "it, out of {} distinct font(s)",
                    best_font, font_example[best_font], best_uses, text_blocks.size(),
                    font_uses.size());
    } else {
        FE_LOG_WARN("no text block carried a font; the lobby will use the engine default "
                    "typeface ({} block(s) considered)", text_blocks.size());
    }
    (void)game_widget_instances;

    std::sort(game_widgets.begin(), game_widgets.end());
    FE_LOG_INFO("the game ships {} widget class(es):", game_widgets.size());
    for (const std::string& widget : game_widgets) {
        FE_LOG_INFO("  {}", widget);
    }

    if (!context.StaticsComplete()) {
        return Result::Fail(
            ErrorCode::InvalidState,
            std::format("lobby UI incomplete: spawn={} statics={} canvasAdd={} addChild={} "
                        "pos={} size={} conv={} textlib={} canvasClass={} textClass={} "
                        "borderClass={} sizeboxClass={} scaleboxClass={} stretch={} "
                        "create={} addToViewport={} hostClass={} buttonClass={} "
                        "widgetLib={}",
                        context.spawn_object != 0, context.gameplay_statics != 0,
                        context.add_to_canvas != 0, context.add_child != 0,
                        context.set_position != 0, context.set_size != 0,
                        context.convert_function != 0, context.text_library != 0,
                        context.canvas_class != 0, context.text_class != 0,
                        context.border_class != 0, context.sizebox_class != 0,
                        context.scalebox_class != 0, context.set_stretch != 0,
                        context.create_widget != 0, context.add_to_viewport != 0,
                        context.host_class != 0, context.button_class != 0,
                        context.widget_library != 0));
    }

    out_context = context;
    return Result::Success();
}

Result BindLobbyMenu(std::uintptr_t menu, LobbyUIContext& context) {
    if (menu == 0) {
        return Result::Fail(ErrorCode::InvalidState, "no live main menu to attach to");
    }

    // The lobby is parented into the main menu's own root, which means the engine owns its
    // lifetime, scaling and draw order rather than this code having to.
    //
    //   UserWidget +0x268 WidgetTree, WidgetTree +0x30 RootWidget
    std::uintptr_t tree = 0;
    if (!memory::GuardedRead(menu + 0x268, &tree, sizeof(tree)) || tree == 0) {
        return Result::Fail(ErrorCode::InvalidState, "the menu has no widget tree");
    }
    std::uintptr_t root = 0;
    if (!memory::GuardedRead(tree + 0x30, &root, sizeof(root)) || root == 0) {
        return Result::Fail(ErrorCode::InvalidState, "the widget tree has no root");
    }

    context.outer       = menu;
    context.root_canvas = root;
    return Result::Success();
}

Result ResolveLobbyUI(const ObjectArray& objects, LobbyUIContext& out_context) {
    LobbyUIContext context;
    if (const Result statics = ResolveLobbyStatics(objects, context); !statics.ok()) {
        return statics;
    }

    std::uintptr_t menu = 0;
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.name.rfind("Default__", 0) == 0 || object.class_name != "WBP_MainMenu_C") {
            return true;
        }
        menu = object.address;
        return false;
    });
    if (const Result bound = BindLobbyMenu(menu, context); !bound.ok()) {
        return bound;
    }

    // Naming the root makes a refusal explainable rather than a guess about its type. Only
    // done on this path, which is not the one a click takes.
    objects.ForEach([&](const ObjectInfo& object) {
        if (object.address != context.root_canvas) {
            return true;
        }
        context.root_class = object.class_name;
        return false;
    });
    FE_LOG_INFO("menu root is 0x{:X} ({})", context.root_canvas,
                context.root_class.empty() ? "unknown class" : context.root_class);

    out_context = context;
    return Result::Success();
}

Result BuildLobbyUI(const LobbyUIContext& context, const LobbyView& view,
                    std::uintptr_t& out_root, std::vector<LobbyControl>& out_controls) {
    out_controls.clear();
    if (!context.Complete()) {
        return Result::Fail(ErrorCode::InvalidState, "the lobby UI context is incomplete");
    }

    const Builder builder(context);

    // Replace rather than stack. Clicking the entry twice previously built a second lobby
    // on top of the first, which then had to be closed twice.
    RemoveLobbyUI(context);

    // Hosted in a user widget and added to the viewport.
    //
    // Parenting into the menu's own widget tree built the whole screen correctly, reported
    // success, and drew nothing. AddToViewport is the path already proven to put a widget
    // on screen in this game, so the lobby is shown the way the engine shows any screen.
    const std::uintptr_t root = CreateHostedCanvas(context);
    if (root == 0) {
        return Result::Fail(ErrorCode::InvalidState,
                            "could not host the lobby canvas in a viewport widget");
    }

    // Backdrop and frame.
    (void)builder.Panel(root, 0.0F, 0.0F, kDesignWidth, kDesignHeight, {0, 0, 0, 0.72F});
    (void)builder.Backer(root, 40.0F, 140.0F, 1840.0F, 795.0F, kPanel);

    // Title.
    (void)builder.Backer(root, 620.0F, 40.0F, 680.0F, 80.0F, kPanelLight);
    (void)builder.Label(root, 640.0F, 36.0F, 640.0F, 88.0F, "MULTIPLAYER LOBBY");

    // Status, top right. Built here rather than per tab, because it describes the session
    // rather than whichever tab happens to be showing.
    (void)builder.Panel(root, 1560.0F, 24.0F, 320.0F, 104.0F, kPanel);
    for (std::size_t line = 0; line < std::size(g_status_line); ++line) {
        g_status_line[line] =
            builder.Text(root, 1576.0F, 36.0F + static_cast<float>(line) * 30.0F, 300.0F,
                         24.0F, "", kTextDim, 16.0F);
    }

    // Tabs, as real buttons so they can be pressed rather than only looked at.
    out_controls.push_back({builder.Button(root, 520.0F, 122.0F, 440.0F, 66.0F, "HOST"),
                            LobbyAction::ShowHost});
    out_controls.push_back({builder.Button(root, 968.0F, 122.0F, 440.0F, 66.0F, "BROWSE"),
                            LobbyAction::ShowBrowse});

    // Both tabs are built, each on its own canvas covering the same area, and switching
    // between them sets one visible and the other collapsed. Rebuilding to change tab meant
    // creating the whole screen again, which was slow enough to see and briefly restored
    // the menu underneath along with its sounds.
    g_host_tab = builder.Canvas(root, 0.0F, 0.0F, kDesignWidth, kDesignHeight);
    if (g_host_tab != 0) {
        DrawHostTab(builder, g_host_tab, view, out_controls);
    }
    g_browse_tab = builder.Canvas(root, 0.0F, 0.0F, kDesignWidth, kDesignHeight);
    if (g_browse_tab != 0) {
        DrawBrowseTab(builder, g_browse_tab, view, out_controls);
    }
    SetLobbyTab(context, view.browsing);
    SetLobbyMode(context, view.mode == "SLAYER");
    {
        int chosen = 0;
        for (std::size_t index = 0; index < std::size(kLobbyMaps); ++index) {
            if (view.map == kLobbyMaps[index].scenario) {
                chosen = static_cast<int>(index);
            }
        }
        SetLobbyMap(context, chosen);
    }

    out_root          = root;
    g_open_lobby_root = root;
    FE_LOG_INFO("lobby UI built: host tab 0x{:X}, browse tab 0x{:X}, {} control(s)",
                g_host_tab, g_browse_tab, out_controls.size());
    return Result::Success();
}

void ShowLobbyUI(const LobbyUIContext& context, bool visible) {
    if (g_open_host_widget == 0) {
        return;
    }
    SetWidgetVisibility(context, g_open_host_widget, visible ? kVisible : kCollapsed);
    if (visible) {
        FoldMenuAway(context);
        FocusLobby(context, g_open_host_widget);

        // The pump has to move with the screen.
        //
        // Queued game thread work runs from a widget's own event path, and the widget it
        // was installed on is the main menu, which this has just collapsed. A collapsed
        // widget receives no events, so the pump stopped firing the moment the lobby
        // opened and every job fell back to the slow path: that is the delay on changing a
        // filter, a mode or a map. The lobby is what is alive now, so the lobby carries it.
        if (const Result pump = InstallGameThreadPump(g_open_host_widget); !pump.ok()) {
            FE_LOG_WARN("the lobby could not take over the game thread pump: {}",
                        pump.message());
        }
        return;
    }

    // Handed back before the lobby goes away, for the same reason in reverse.
    if (context.outer != 0) {
        (void)InstallGameThreadPump(context.outer);
    }

    UnfoldMenu(context);

    // Input has to go back with the menu.
    //
    // Opening the lobby points the player controller's UI focus at the lobby widget. Simply
    // hiding that widget leaves the focus pointing at something collapsed, so the menu comes
    // back on screen and answers nothing: every button on it is dead. Handing focus to the
    // menu itself is what makes it usable again.
    FocusLobby(context, context.outer);
}

void SetLobbyTab(const LobbyUIContext& context, bool browsing) {
    SetWidgetVisibility(context, g_host_tab,
                        browsing ? kCollapsed : kSelfHitTestInvisible);
    SetWidgetVisibility(context, g_browse_tab,
                        browsing ? kSelfHitTestInvisible : kCollapsed);
    FE_LOG_INFO("lobby tab is now {} (host 0x{:X}, browse 0x{:X}, setter 0x{:X})",
                browsing ? "BROWSE" : "HOST", g_host_tab, g_browse_tab,
                context.set_visibility);
}

void SetLobbyMode(const LobbyUIContext& context, bool slayer) {
    SetWidgetVisibility(context, g_mode_marker[0], slayer ? kCollapsed : kVisible);
    SetWidgetVisibility(context, g_mode_marker[1], slayer ? kVisible : kCollapsed);
}

void SetLobbyMap(const LobbyUIContext& context, int map_index) {
    for (int index = 0; index < 4; ++index) {
        SetWidgetVisibility(context, g_map_marker[index],
                            index == map_index ? kVisible : kCollapsed);
    }
}

void SetLobbyServers(const LobbyUIContext& context, const std::vector<ServerEntry>& servers,
                     int selected) {
    const Builder builder(context);
    for (std::size_t index = 0; index < kServerRows; ++index) {
        ServerRowWidgets& row  = g_server_row[index];
        const bool        used = index < servers.size();

        builder.SetVisibilityOf(row.button, used ? kVisibleValue : kCollapsedValue);
        for (const std::uintptr_t block : {row.name, row.mode, row.map, row.players,
                                           row.ping}) {
            builder.SetVisibilityOf(block, used ? kHitTestInvisible : kCollapsedValue);
        }
        builder.SetVisibilityOf(
            row.highlight,
            used && static_cast<int>(index) == selected ? kSelfHitTestInvisibleValue
                                                        : kCollapsedValue);
        if (!used) {
            continue;
        }

        const ServerEntry& entry = servers[index];
        builder.SetText(row.name, entry.name);
        builder.SetText(row.mode, entry.mode);
        builder.SetText(row.map, entry.map);
        builder.SetText(row.players, std::format("{}/{}", entry.players, entry.capacity));
        builder.SetText(row.ping, std::format("{}ms", entry.ping));
    }

    builder.SetVisibilityOf(g_empty_notice,
                            servers.empty() ? kVisibleValue : kCollapsedValue);

    // The details panel follows the selection, and is blanked rather than left showing a
    // server that is no longer in the list.
    const bool have_selection =
        !servers.empty() && selected >= 0 && selected < static_cast<int>(servers.size());
    const std::array<std::string, 5> lines =
        have_selection
            ? std::array<std::string, 5>{
                  std::format("Server: {}", servers[static_cast<std::size_t>(selected)].name),
                  std::format("Mode: {}", servers[static_cast<std::size_t>(selected)].mode),
                  std::format("Map: {}", servers[static_cast<std::size_t>(selected)].map),
                  std::format("Players: {}/{}",
                              servers[static_cast<std::size_t>(selected)].players,
                              servers[static_cast<std::size_t>(selected)].capacity),
                  std::format("Ping: {}ms",
                              servers[static_cast<std::size_t>(selected)].ping)}
            : std::array<std::string, 5>{"No server selected", "", "", "", ""};
    for (std::size_t line = 0; line < lines.size(); ++line) {
        builder.SetText(g_detail_line[line], lines[line]);
        builder.SetVisibilityOf(g_detail_line[line], kHitTestInvisible);
    }
}

void SetLobbyStatus(const LobbyUIContext& context, const LobbyStatus& status) {
    const Builder builder(context);

    builder.SetText(g_status_line[0], status.online ? "NET: ONLINE" : "NET: OFFLINE");
    builder.SetTextAppearance(g_status_line[0], status.online ? kGood : kBad, 16.0F);

    builder.SetText(g_status_line[1], std::format("SESSION: {}", status.session));
    builder.SetTextAppearance(g_status_line[1], status.invitable ? kGood : kTextDim, 16.0F);

    builder.SetText(g_status_line[2], status.version);
    builder.SetTextAppearance(g_status_line[2],
                              status.update_available ? kWarn : kTextDim, 16.0F);

    for (const std::uintptr_t line : g_status_line) {
        builder.SetVisibilityOf(line, kHitTestInvisible);
    }
}

void SetLobbyFilters(const LobbyUIContext& context, const ServerFilter& filter) {
    const Builder builder(context);

    const int mode_choice = filter.mode.empty() ? 0 : (filter.mode == "SLAYER" ? 2 : 1);
    const int ping_choice = filter.max_ping == 0 ? 0 : (filter.max_ping <= 50 ? 1 : 2);
    const std::array<std::pair<std::uintptr_t*, int>, 3> groups = {
        std::pair{g_filter_mode, mode_choice},
        std::pair{g_filter_slots, filter.slots},
        std::pair{g_filter_ping, ping_choice},
    };
    for (const auto& [markers, choice] : groups) {
        for (int option = 0; option < 3; ++option) {
            builder.SetVisibilityOf(markers[option], option == choice
                                                         ? kSelfHitTestInvisibleValue
                                                         : kCollapsedValue);
        }
    }
}

std::string ReadServerName(const LobbyUIContext& context) {
    if (g_server_name_field == 0 || context.get_editable_text == 0 ||
        context.text_to_string == 0 || context.text_library == 0) {
        return {};
    }

    struct TextParameters {
        std::uint8_t text[0x10];
    };
    TextParameters current{};
    if (!CallFunction(g_server_name_field, context.get_editable_text, &current).ok()) {
        return {};
    }

    // An FText owns shared string data, so it is converted rather than read: the engine
    // hands back an FString whose buffer can then be walked.
    struct StringParameters {
        std::uint8_t text[0x10];
        struct {
            wchar_t*     data;
            std::int32_t count;
            std::int32_t capacity;
        } result;
    };
    StringParameters converted{};
    std::memcpy(converted.text, current.text, sizeof(converted.text));
    if (!CallFunction(context.text_library, context.text_to_string, &converted).ok() ||
        converted.result.data == nullptr || converted.result.count <= 0) {
        return {};
    }

    std::string name;
    name.reserve(static_cast<std::size_t>(converted.result.count));
    for (std::int32_t index = 0; index < converted.result.count; ++index) {
        wchar_t character = 0;
        if (!memory::GuardedRead(reinterpret_cast<std::uintptr_t>(converted.result.data) +
                                     static_cast<std::uintptr_t>(index) * sizeof(wchar_t),
                                 &character, sizeof(character)) ||
            character == L'\0') {
            break;
        }
        name.push_back(static_cast<char>(character));
    }
    return name;
}

bool LobbyIsBuilt() { return g_open_host_widget != 0 && g_open_lobby_root != 0; }

} // namespace fe::unreal
