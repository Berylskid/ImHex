#include "content/views/view_hex_editor.hpp"

#include <hex/api/content_registry.hpp>
#include <hex/api/keybinding.hpp>
#include <hex/helpers/utils.hpp>
#include <hex/providers/buffered_reader.hpp>
#include <hex/helpers/crypto.hpp>

#include <content/providers/view_provider.hpp>
#include <content/helpers/math_evaluator.hpp>

#include <imgui_internal.h>
#include <nlohmann/json.hpp>

#include <thread>

using namespace std::literals::string_literals;

namespace hex::plugin::builtin {

    /* Popups */

    class PopupGoto : public ViewHexEditor::Popup {
    public:
        void draw(ViewHexEditor *editor) override {
            ImGui::TextUnformatted("hex.builtin.view.hex_editor.menu.file.goto"_lang);
            if (ImGui::BeginTabBar("goto_tabs")) {
                if (ImGui::BeginTabItem("hex.builtin.view.hex_editor.goto.offset.absolute"_lang)) {
                    this->m_mode = Mode::Absolute;
                    ImGui::EndTabItem();
                }

                ImGui::BeginDisabled(!editor->isSelectionValid());
                if (ImGui::BeginTabItem("hex.builtin.view.hex_editor.goto.offset.relative"_lang)) {
                    this->m_mode = Mode::Relative;
                    ImGui::EndTabItem();
                }
                ImGui::EndDisabled();

                if (ImGui::BeginTabItem("hex.builtin.view.hex_editor.goto.offset.begin"_lang)) {
                    this->m_mode = Mode::Begin;
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("hex.builtin.view.hex_editor.goto.offset.end"_lang)) {
                    this->m_mode = Mode::End;
                    ImGui::EndTabItem();
                }

                if(this->m_requestFocus){
                    ImGui::SetKeyboardFocusHere();
                    this->m_requestFocus = false;
                }
                if (ImGui::InputTextIcon("##input", ICON_VS_SYMBOL_OPERATOR, this->m_input, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                    if (auto result = this->m_evaluator.evaluate(this->m_input); result.has_value()) {
                        const auto inputResult = result.value();
                        u64 newAddress = 0x00;

                        auto provider = ImHexApi::Provider::get();

                        switch (this->m_mode) {
                            case Mode::Absolute: {
                                newAddress = inputResult;
                            }
                                break;
                            case Mode::Relative: {
                                const auto selection = editor->getSelection();
                                newAddress = selection.getStartAddress() + inputResult;
                            }
                                break;
                            case Mode::Begin: {
                                newAddress = provider->getBaseAddress() + provider->getCurrentPageAddress() + inputResult;
                            }
                                break;
                            case Mode::End: {
                                newAddress = provider->getActualSize() - inputResult;
                            }
                                break;
                        }

                        editor->setSelection(newAddress, newAddress);
                        editor->jumpToSelection();
                    }
                }

                ImGui::EndTabBar();
            }
        }

    private:
        enum class Mode : u8 {
            Absolute,
            Relative,
            Begin,
            End
        };

        Mode m_mode = Mode::Absolute;

        bool m_requestFocus = true;
        std::string m_input;
        MathEvaluator<i128> m_evaluator;
    };

    class PopupSelect : public ViewHexEditor::Popup {
    public:

        void draw(ViewHexEditor *editor) override {
            ImGui::TextUnformatted("hex.builtin.view.hex_editor.menu.file.select"_lang);
            if (ImGui::BeginTabBar("select_tabs")) {
                if (ImGui::BeginTabItem("hex.builtin.view.hex_editor.select.offset.region"_lang)) {
                    u64 inputA = this->m_region.getStartAddress();
                    u64 inputB = this->m_region.getEndAddress();
                    ImGui::InputHexadecimal("hex.builtin.view.hex_editor.select.offset.begin"_lang, &inputA, ImGuiInputTextFlags_AutoSelectAll);
                    ImGui::InputHexadecimal("hex.builtin.view.hex_editor.select.offset.end"_lang, &inputB, ImGuiInputTextFlags_AutoSelectAll);

                    if (inputB < inputA)
                        inputB = inputA;

                    this->m_region = { inputA, (inputB - inputA) + 1 };

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("hex.builtin.view.hex_editor.select.offset.size"_lang)) {
                    u64 inputA = this->m_region.getStartAddress();
                    u64 inputB = this->m_region.getSize();
                    ImGui::InputHexadecimal("hex.builtin.view.hex_editor.select.offset.begin"_lang, &inputA, ImGuiInputTextFlags_AutoSelectAll);
                    ImGui::InputHexadecimal("hex.builtin.view.hex_editor.select.offset.size"_lang, &inputB, ImGuiInputTextFlags_AutoSelectAll);

                    if (inputB <= 0)
                        inputB = 1;

                    this->m_region = { inputA, inputB };
                    ImGui::EndTabItem();
                }

                if (ImGui::Button("hex.builtin.view.hex_editor.select.select"_lang) || (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Enter)))) {
                    editor->setSelection(this->m_region.getStartAddress(), this->m_region.getEndAddress());
                    editor->jumpToSelection();
                }

                ImGui::EndTabBar();
            }
        }

    private:
        Region m_region = { 0, 1 };
    };

    class PopupFind : public ViewHexEditor::Popup {
    public:
        PopupFind() {
            EventManager::subscribe<EventRegionSelected>(this, [this](Region region) {
                this->m_searchPosition = this->m_nextSearchPosition.value_or(region.getStartAddress());
                this->m_nextSearchPosition.reset();
            });
        }

        ~PopupFind() override {
            EventManager::unsubscribe<EventRegionSelected>(this);
        }

        void draw(ViewHexEditor *editor) override {
            std::vector<u8> searchSequence;

            ImGui::TextUnformatted("hex.builtin.view.hex_editor.menu.file.search"_lang);
            if (ImGui::BeginTabBar("##find_tabs")) {
                if (ImGui::BeginTabItem("hex.builtin.view.hex_editor.search.hex"_lang)) {
                    if (ImGui::InputTextIcon("##input", ICON_VS_SYMBOL_NUMERIC, this->m_input, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CharsHexadecimal)) {
                        if (!this->m_input.empty()) {
                            this->m_shouldSearch = true;
                            this->m_backwards = false;
                        }
                    }

                    this->drawButtons();

                    if (this->m_shouldSearch) {
                        searchSequence = crypt::decode16(this->m_input);
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem("hex.builtin.view.hex_editor.search.string"_lang)) {
                    if (ImGui::InputTextIcon("##input", ICON_VS_SYMBOL_KEY, this->m_input, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
                        if (!this->m_input.empty()) {
                            this->m_shouldSearch = true;
                            this->m_backwards = false;
                        }
                    }

                    this->drawButtons();

                    if (this->m_shouldSearch) {
                        searchSequence.clear();
                        std::copy(this->m_input.begin(), this->m_input.end(), std::back_inserter(searchSequence));

                        if (!searchSequence.empty() && searchSequence.back() == 0x00)
                            searchSequence.pop_back();
                    }

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            if (!this->m_searchTask.isRunning() && !searchSequence.empty() && this->m_shouldSearch) {
                this->m_searchTask = TaskManager::createTask("hex.builtin.common.processing", ImHexApi::Provider::get()->getActualSize(), [this, editor, searchSequence](auto &) {
                    for (u8 retry = 0; retry < 2; retry++) {
                        auto region = this->findSequence(searchSequence, this->m_backwards);

                        if (region.has_value()) {
                            if (editor->getSelection() == region) {
                                if (this->m_nextSearchPosition.has_value())
                                    this->m_searchPosition = this->m_nextSearchPosition.value();
                                this->m_nextSearchPosition.reset();

                                continue;
                            } else {
                                TaskManager::doLater([editor, region]{
                                    editor->setSelection(region->getStartAddress(), region->getEndAddress());
                                    editor->jumpToSelection();
                                });

                                break;
                            }
                        } else {
                            this->m_reachedEnd = true;
                        }
                    }

                    this->m_shouldSearch = false;
                    this->m_requestFocus = true;
                });
            }
        }

    private:
        void drawButtons() {
            const auto ButtonSize = ImVec2(ImGui::CalcTextSize(ICON_VS_SEARCH).x, ImGui::GetTextLineHeight()) + ImGui::GetStyle().CellPadding * 2;
            const auto ButtonColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);

            if (this->m_requestFocus) {
                ImGui::SetKeyboardFocusHere(-1);
                this->m_requestFocus = false;
            }

            ImGui::BeginDisabled(this->m_searchTask.isRunning());
            {
                ImGui::SameLine();
                if (ImGui::IconButton(ICON_VS_SEARCH "##search", ButtonColor, ButtonSize)) {
                    this->m_shouldSearch = true;
                    this->m_backwards = false;
                    this->m_reachedEnd = false;
                    this->m_searchPosition.reset();
                    this->m_nextSearchPosition.reset();
                }

                ImGui::BeginDisabled(!this->m_searchPosition.has_value());
                {
                    ImGui::BeginDisabled(this->m_reachedEnd && this->m_backwards);
                    {
                        if (ImGui::IconButton(ICON_VS_ARROW_UP "##up", ButtonColor, ButtonSize)) {
                            this->m_shouldSearch = true;
                            this->m_backwards = true;
                            this->m_reachedEnd = false;
                        }
                    }
                    ImGui::EndDisabled();

                    ImGui::SameLine();

                    ImGui::BeginDisabled(this->m_reachedEnd && !this->m_backwards);
                    {
                        if (ImGui::IconButton(ICON_VS_ARROW_DOWN "##down", ButtonColor, ButtonSize)) {
                            this->m_shouldSearch = true;
                            this->m_backwards = false;
                            this->m_reachedEnd = false;
                        }
                    }
                    ImGui::EndDisabled();
                }

                ImGui::EndDisabled();
            }
            ImGui::EndDisabled();
        }

        std::optional<Region> findSequence(const std::vector<u8> &sequence, bool backwards) {
            auto provider = ImHexApi::Provider::get();

            prv::ProviderReader reader(provider);

            reader.seek(this->m_searchPosition.value_or(provider->getBaseAddress()));

            constexpr static auto searchFunction = [](const auto &haystackBegin, const auto &haystackEnd, const auto &needleBegin, const auto &needleEnd) {
                return std::search(haystackBegin, haystackEnd, std::boyer_moore_horspool_searcher(needleBegin, needleEnd));
            };

            if (!backwards) {
                auto occurrence = searchFunction(reader.begin(), reader.end(), sequence.begin(), sequence.end());
                if (occurrence != reader.end()) {
                    this->m_nextSearchPosition = occurrence.getAddress() + sequence.size();
                    return Region { occurrence.getAddress(), sequence.size() };
                }
            } else {
                auto occurrence = searchFunction(reader.rbegin(), reader.rend(), sequence.rbegin(), sequence.rend());
                if (occurrence != reader.rend()) {
                    if (occurrence.getAddress() < sequence.size())
                        this->m_nextSearchPosition = 0x00;
                    else
                        this->m_nextSearchPosition = occurrence.getAddress() - sequence.size();

                    return Region { occurrence.getAddress() - (sequence.size() - 1), sequence.size() };
                }
            }

            return std::nullopt;
        }

        std::string m_input;
        std::optional<u64> m_searchPosition, m_nextSearchPosition;

        bool m_requestFocus = true;
        std::atomic<bool> m_shouldSearch = false;
        std::atomic<bool> m_backwards    = false;
        std::atomic<bool> m_reachedEnd   = false;

        TaskHolder m_searchTask;
    };

    class PopupBaseAddress : public ViewHexEditor::Popup {
    public:
        explicit PopupBaseAddress(u64 baseAddress) : m_baseAddress(baseAddress) { }

        void draw(ViewHexEditor *editor) override {
            ImGui::TextUnformatted("hex.builtin.view.hex_editor.menu.edit.set_base"_lang);

            ImGui::InputHexadecimal("##base_address", &this->m_baseAddress);
            if (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                setBaseAddress(this->m_baseAddress);
                editor->closePopup();
            }

            View::confirmButtons("hex.builtin.common.set"_lang, "hex.builtin.common.cancel"_lang,
                [&, this]{
                    setBaseAddress(this->m_baseAddress);
                    editor->closePopup();
                },
                [&]{
                    editor->closePopup();
                });
        }

    private:
        static void setBaseAddress(u64 baseAddress) {
            if (ImHexApi::Provider::isValid())
                ImHexApi::Provider::get()->setBaseAddress(baseAddress);
        }

    private:
        u64 m_baseAddress;
    };

    class PopupResize : public ViewHexEditor::Popup {
    public:
        explicit PopupResize(u64 currSize) : m_size(currSize) {}

        void draw(ViewHexEditor *editor) override {
            ImGui::TextUnformatted("hex.builtin.view.hex_editor.menu.edit.resize"_lang);

            ImGui::InputHexadecimal("##resize", &this->m_size);
            if (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_Enter))) {
                resize(static_cast<size_t>(this->m_size));
                editor->closePopup();
            }

            View::confirmButtons("hex.builtin.common.set"_lang, "hex.builtin.common.cancel"_lang,
                [&, this]{
                    resize(static_cast<size_t>(this->m_size));
                    editor->closePopup();
                },
                [&]{
                    editor->closePopup();
                });
        }

    private:
        static void resize(size_t newSize) {
            if (ImHexApi::Provider::isValid())
                ImHexApi::Provider::get()->resize(newSize);
        }

    private:
        u64 m_size;
    };

    class PopupInsert : public ViewHexEditor::Popup {
    public:
        PopupInsert(u64 address, size_t size) : m_address(address), m_size(size) {}

        void draw(ViewHexEditor *editor) override {
            ImGui::TextUnformatted("hex.builtin.view.hex_editor.menu.edit.insert"_lang);

            ImGui::InputHexadecimal("hex.builtin.common.address"_lang, &this->m_address);
            ImGui::InputHexadecimal("hex.builtin.common.size"_lang, &this->m_size);

            View::confirmButtons("hex.builtin.common.set"_lang, "hex.builtin.common.cancel"_lang,
                [&, this]{
                    insert(this->m_address, static_cast<size_t>(this->m_size));
                    editor->closePopup();
                },
                [&]{
                    editor->closePopup();
                });
        }

    private:
        static void insert(u64 address, size_t size) {
            if (ImHexApi::Provider::isValid())
                ImHexApi::Provider::get()->insert(address, size);
        }

    private:
        u64 m_address;
        u64 m_size;
    };

    class PopupRemove : public ViewHexEditor::Popup {
    public:
        PopupRemove(u64 address, size_t size) : m_address(address), m_size(size) {}

        void draw(ViewHexEditor *editor) override {
            ImGui::TextUnformatted("hex.builtin.view.hex_editor.menu.edit.remove"_lang);

            ImGui::InputHexadecimal("hex.builtin.common.address"_lang, &this->m_address);
            ImGui::InputHexadecimal("hex.builtin.common.size"_lang, &this->m_size);

            View::confirmButtons("hex.builtin.common.set"_lang, "hex.builtin.common.cancel"_lang,
                [&, this]{
                    remove(this->m_address, static_cast<size_t>(this->m_size));
                    editor->closePopup();
                },
                [&]{
                    editor->closePopup();
                });
        }

    private:
        static void remove(u64 address, size_t size) {
            if (ImHexApi::Provider::isValid())
                ImHexApi::Provider::get()->remove(address, size);
        }

    private:
        u64 m_address;
        u64 m_size;
    };

    /* Hex Editor */

    ViewHexEditor::ViewHexEditor() : View("hex.builtin.view.hex_editor.name") {
        this->m_hexEditor.setForegroundHighlightCallback([](u64 address, const u8 *data, size_t size) -> std::optional<color_t> {
            std::optional<color_t> result;
            for (const auto &[id, callback] : ImHexApi::HexEditor::impl::getForegroundHighlightingFunctions()) {
                if (auto color = callback(address, data, size, result.has_value()); color.has_value())
                    result = color;
            }

            if (result.has_value())
                return result;

            for (const auto &[id, highlighting] : ImHexApi::HexEditor::impl::getForegroundHighlights()) {
                if (highlighting.getRegion().overlaps({ address, size }))
                    return highlighting.getColor();
            }

            return std::nullopt;
        });

        this->m_hexEditor.setBackgroundHighlightCallback([](u64 address, const u8 *data, size_t size) -> std::optional<color_t> {
            std::optional<color_t> result;
            for (const auto &[id, callback] : ImHexApi::HexEditor::impl::getBackgroundHighlightingFunctions()) {
                if (auto color = callback(address, data, size, result.has_value()); color.has_value())
                    return color.value();
            }

            if (result.has_value())
                return result;

            for (const auto &[id, highlighting] : ImHexApi::HexEditor::impl::getBackgroundHighlights()) {
                if (highlighting.getRegion().overlaps({ address, size }))
                    return highlighting.getColor();
            }

            return std::nullopt;
        });

        this->m_hexEditor.setTooltipCallback([](u64 address, const u8 *data, size_t size) {
            for (const auto &[id, callback] : ImHexApi::HexEditor::impl::getTooltipFunctions()) {
                callback(address, data, size);
            }

            for (const auto &[id, tooltip] : ImHexApi::HexEditor::impl::getTooltips()) {
                if (tooltip.getRegion().overlaps({ address, size })) {
                    ImGui::BeginTooltip();
                    if (ImGui::BeginTable("##tooltips", 1, ImGuiTableFlags_NoHostExtendX | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoClip)) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();

                        ImGui::ColorButton(tooltip.getValue().c_str(), ImColor(tooltip.getColor()));
                        ImGui::SameLine(0, 10);
                        ImGui::TextUnformatted(tooltip.getValue().c_str());

                        ImGui::PushStyleColor(ImGuiCol_TableRowBg, tooltip.getColor());
                        ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, tooltip.getColor());
                        ImGui::EndTable();
                        ImGui::PopStyleColor(2);
                    }
                    ImGui::EndTooltip();
                }
            }
        });

        this->registerShortcuts();
        this->registerEvents();
        this->registerMenuItems();
    }

    ViewHexEditor::~ViewHexEditor() {
        EventManager::unsubscribe<EventProviderChanged>(this);
    }

    void ViewHexEditor::drawPopup() {
        // Popup windows
        if (this->m_shouldOpenPopup) {
            this->m_shouldOpenPopup = false;
            ImGui::OpenPopup("##hex_editor_popup");
        }

        ImGui::SetNextWindowPos(ImGui::GetWindowPos() + ImGui::GetWindowContentRegionMin() - ImGui::GetStyle().WindowPadding, ImGuiCond_Appearing);
        if (ImGui::BeginPopup("##hex_editor_popup", ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |ImGuiWindowFlags_NoTitleBar)) {
            // Force close the popup when user is editing an input
            if(ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Escape))){
                ImGui::CloseCurrentPopup();
            }

            if (this->m_currPopup != nullptr)
                this->m_currPopup->draw(this);
            else
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        } else {
            this->closePopup();
        }

        // Right click menu
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
            EventManager::post<RequestOpenPopup>("hex.builtin.menu.edit");
    }

    void ViewHexEditor::drawContent() {
        if (ImGui::Begin(View::toWindowName(this->getUnlocalizedName()).c_str(), &this->getWindowOpenState(), ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            this->m_hexEditor.setProvider(ImHexApi::Provider::get());

            this->m_hexEditor.draw();

            this->drawPopup();
        }
        ImGui::End();
    }

    static void save() {
        ImHexApi::Provider::get()->save();
    }

    static void saveAs() {
        fs::openFileBrowser(fs::DialogMode::Save, {}, [](const auto &path) {
            ImHexApi::Provider::get()->saveAs(path);
        });
    }

    static void copyBytes(const Region &selection) {
        constexpr static auto Format = "{0:02X} ";

        auto provider = ImHexApi::Provider::get();

        auto reader = prv::ProviderReader (provider);
        reader.seek(selection.getStartAddress());
        reader.setEndAddress(selection.getEndAddress());

        std::string result;
        result.reserve(fmt::format(Format, 0x00).size() * selection.getSize());

        for (const auto &byte : reader)
            result += fmt::format(Format, byte);
        result.pop_back();

        ImGui::SetClipboardText(result.c_str());
    }

    static void pasteBytes(const Region &selection, bool selectionCheck) {
        auto provider = ImHexApi::Provider::get();

        std::string clipboard = ImGui::GetClipboardText();
        if (clipboard.empty())
            return;

        // Remove common hex prefixes and commas
        hex::replaceStrings(clipboard, "0x", "");
        hex::replaceStrings(clipboard, "0X", "");
        hex::replaceStrings(clipboard, ",", "");

        // Check for non-hex characters
        bool isValidHexString = std::find_if(clipboard.begin(), clipboard.end(), [](char c) {
            return !std::isxdigit(c) && !std::isspace(c);
        }) == clipboard.end();

        if (!isValidHexString) return;

        // Remove all whitespace
        clipboard.erase(std::remove_if(clipboard.begin(), clipboard.end(), [](char c) { return std::isspace(c); }), clipboard.end());

        // Only paste whole bytes
        if (clipboard.length() % 2 != 0) return;

        // Convert hex string to bytes
        std::vector<u8> buffer = crypt::decode16(clipboard);

        if (!selectionCheck) {
            if (selection.getStartAddress() + buffer.size() >= provider->getActualSize())
                provider->resize(selection.getStartAddress() + buffer.size());
        }

        // Write bytes
        auto size = selectionCheck ? std::min(buffer.size(), selection.getSize()) : buffer.size();
        provider->write(selection.getStartAddress() + provider->getBaseAddress() + provider->getCurrentPageAddress(), buffer.data(), size);
    }

    static void copyString(const Region &selection) {
        auto provider = ImHexApi::Provider::get();

        std::string buffer(selection.size, 0x00);
        buffer.reserve(selection.size);
        provider->read(selection.getStartAddress(), buffer.data(), selection.size);

        ImGui::SetClipboardText(buffer.c_str());
    }

    static void copyCustomEncoding(const EncodingFile &customEncoding, const Region &selection) {
        auto provider = ImHexApi::Provider::get();

        std::vector<u8> buffer(customEncoding.getLongestSequence(), 0x00);
        std::string string;

        u64 offset = selection.getStartAddress();
        while (offset < selection.getEndAddress()) {
            provider->read(offset, buffer.data(), std::min<size_t>(buffer.size(), selection.size - (offset - selection.getStartAddress())));
            auto [result, size] = customEncoding.getEncodingFor(buffer);

            string += result;
            offset += size;
        };

        ImGui::SetClipboardText(string.c_str());
    }

    void ViewHexEditor::registerShortcuts() {
        // Save operations
        ShortcutManager::addShortcut(this, CTRLCMD + Keys::S, [] {
            save();
        });
        ShortcutManager::addShortcut(this, CTRLCMD + SHIFT + Keys::S, [] {
            saveAs();
        });

        // Select All
        ShortcutManager::addShortcut(this, CTRLCMD + Keys::A, [this] {
            if (ImHexApi::Provider::isValid())
                this->setSelection(size_t(0), ImHexApi::Provider::get()->getActualSize());
        });

        // Select range
        ShortcutManager::addShortcut(this, CTRLCMD + SHIFT + Keys::A, [this] {
            if (ImHexApi::Provider::isValid())
                this->openPopup<PopupSelect>();
        });

        // Remove selection
        ShortcutManager::addShortcut(this, Keys::Escape, [this] {
            auto provider = ImHexApi::Provider::get();
            auto &data = ProviderExtraData::get(provider).editor;

            data.selectionStart.reset();
            data.selectionEnd.reset();

            EventManager::post<EventRegionSelected>(ImHexApi::HexEditor::ProviderRegion{ this->getSelection(), provider });
        });

        // Move cursor around
        ShortcutManager::addShortcut(this, Keys::Up, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition().value_or(selection.getEndAddress());

            if (cursor >= this->m_hexEditor.getBytesPerRow()) {
                auto pos = cursor - this->m_hexEditor.getBytesPerRow();
                this->setSelection(pos, pos);
                this->m_hexEditor.scrollToSelection();
                this->m_hexEditor.jumpIfOffScreen();
            }
        });
        ShortcutManager::addShortcut(this, Keys::Down, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition().value_or(selection.getEndAddress());

            auto pos = cursor + this->m_hexEditor.getBytesPerRow();
            this->setSelection(pos, pos);
            this->m_hexEditor.scrollToSelection();
            this->m_hexEditor.jumpIfOffScreen();
        });
        ShortcutManager::addShortcut(this, Keys::Left, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition().value_or(selection.getEndAddress());

            if (cursor > 0) {
                auto pos = cursor - 1;
                this->setSelection(pos, pos);
                this->m_hexEditor.scrollToSelection();
                this->m_hexEditor.jumpIfOffScreen();
            }
        });
        ShortcutManager::addShortcut(this, Keys::Right, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition().value_or(selection.getEndAddress());

            auto pos = cursor + 1;
            this->setSelection(pos, pos);
            this->m_hexEditor.scrollToSelection();
            this->m_hexEditor.jumpIfOffScreen();
        });

        ShortcutManager::addShortcut(this, Keys::PageUp, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition().value_or(selection.getEndAddress());

            u64 visibleByteCount = this->m_hexEditor.getBytesPerRow() * this->m_hexEditor.getVisibleRowCount();
            if (cursor >= visibleByteCount) {
                auto pos = cursor - visibleByteCount;
                this->setSelection(pos, pos);
                this->m_hexEditor.scrollToSelection();
                this->m_hexEditor.jumpIfOffScreen();
            }
        });
        ShortcutManager::addShortcut(this, Keys::PageDown, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition().value_or(selection.getEndAddress());

            auto pos = cursor + (this->m_hexEditor.getBytesPerRow() * this->m_hexEditor.getVisibleRowCount());
            this->setSelection(pos, pos);
            this->m_hexEditor.scrollToSelection();
            this->m_hexEditor.jumpIfOffScreen();
        });

        // Move selection around
        ShortcutManager::addShortcut(this, SHIFT + Keys::Up, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition();

            if (cursor == selection.getEndAddress()) {
                auto newCursor = std::max<u64>(cursor.value_or(selection.getEndAddress()), this->m_hexEditor.getBytesPerRow()) - this->m_hexEditor.getBytesPerRow();
                setSelection(selection.getStartAddress(), newCursor);
                this->m_hexEditor.setCursorPosition(newCursor);
            } else {
                auto newCursor = std::max<u64>(cursor.value_or(selection.getEndAddress()), this->m_hexEditor.getBytesPerRow()) - this->m_hexEditor.getBytesPerRow();
                setSelection(newCursor, selection.getEndAddress());
                this->m_hexEditor.setCursorPosition(newCursor);
            }

            this->m_hexEditor.scrollToSelection();
            this->m_hexEditor.jumpIfOffScreen();
        });
        ShortcutManager::addShortcut(this, SHIFT + Keys::Down, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition();

            if (cursor == selection.getEndAddress()) {
                auto newCursor = cursor.value_or(selection.getEndAddress()) + this->m_hexEditor.getBytesPerRow();
                setSelection(selection.getStartAddress(), newCursor);
                this->m_hexEditor.setCursorPosition(newCursor);
            } else {
                auto newCursor = cursor.value_or(selection.getEndAddress()) + this->m_hexEditor.getBytesPerRow();
                setSelection(newCursor, selection.getEndAddress());
                this->m_hexEditor.setCursorPosition(newCursor);
            }

            this->m_hexEditor.scrollToSelection();
            this->m_hexEditor.jumpIfOffScreen();
        });
        ShortcutManager::addShortcut(this, SHIFT + Keys::Left, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition();

            if (cursor == selection.getEndAddress()) {
                auto newCursor = std::max<u64>(cursor.value_or(selection.getEndAddress()), 1) - 1;
                setSelection(selection.getStartAddress(), newCursor);
                this->m_hexEditor.setCursorPosition(newCursor);
            } else {
                auto newCursor = std::max<u64>(cursor.value_or(selection.getEndAddress()), 1) - 1;
                setSelection(newCursor, selection.getEndAddress());
                this->m_hexEditor.setCursorPosition(newCursor);
            }

            this->m_hexEditor.scrollToSelection();
            this->m_hexEditor.jumpIfOffScreen();
        });
        ShortcutManager::addShortcut(this, SHIFT + Keys::Right, [this] {
            auto selection = getSelection();
            auto cursor = this->m_hexEditor.getCursorPosition();

            if (cursor == selection.getEndAddress()) {
                auto newCursor = cursor.value_or(selection.getEndAddress()) + 1;
                setSelection(selection.getStartAddress(), newCursor);
                this->m_hexEditor.setCursorPosition(newCursor);
            } else {
                auto newCursor = cursor.value_or(selection.getEndAddress()) + 1;
                setSelection(newCursor, selection.getEndAddress());
                this->m_hexEditor.setCursorPosition(newCursor);
            }

            this->m_hexEditor.scrollToSelection();
            this->m_hexEditor.jumpIfOffScreen();
        });
        ShortcutManager::addShortcut(this, Keys::PageUp, [this] {
            auto selection = getSelection();
            u64 visibleByteCount = this->m_hexEditor.getBytesPerRow() * this->m_hexEditor.getVisibleRowCount();

            if (selection.getEndAddress() >= visibleByteCount) {
                auto pos = selection.getEndAddress() - visibleByteCount;
                this->setSelection(pos, selection.getEndAddress());
                this->m_hexEditor.scrollToSelection();
                this->m_hexEditor.jumpIfOffScreen();
            }
        });
        ShortcutManager::addShortcut(this, Keys::PageDown, [this] {
            auto selection = getSelection();
            auto pos = selection.getEndAddress() + (this->m_hexEditor.getBytesPerRow() * this->m_hexEditor.getVisibleRowCount());

            this->setSelection(pos, selection.getEndAddress());
            this->m_hexEditor.scrollToSelection();
            this->m_hexEditor.jumpIfOffScreen();
        });

        ShortcutManager::addShortcut(this, CTRLCMD + Keys::G, [this] {
            if (!ImHexApi::Provider::isValid()) return;

            this->openPopup<PopupGoto>();
        });
        ShortcutManager::addShortcut(this, CTRLCMD + Keys::F, [this] {
            if (!ImHexApi::Provider::isValid()) return;

            this->openPopup<PopupFind>();
        });

        // Copy
        ShortcutManager::addShortcut(this, CTRLCMD + Keys::C, [this] {
            const auto selection = getSelection();
            copyBytes(selection);
        });
        ShortcutManager::addShortcut(this, CTRLCMD + SHIFT + Keys::C, [this] {
            const auto selection = getSelection();
            copyString(selection);
        });

        // Paste
        ShortcutManager::addShortcut(this, CTRLCMD + Keys::V, [this] {
            const auto selection = getSelection();
            pasteBytes(selection, true);
        });

        // Paste and resize
        ShortcutManager::addShortcut(this, CTRLCMD + SHIFT + Keys::V, [this] {
            const auto selection = getSelection();
            pasteBytes(selection, false);
        });

        // Undo / Redo
        ShortcutManager::addShortcut(this, CTRLCMD + Keys::Z, [] {
            if (ImHexApi::Provider::isValid())
                ImHexApi::Provider::get()->undo();
        });
        ShortcutManager::addShortcut(this, CTRLCMD + Keys::Y, [] {
            if (ImHexApi::Provider::isValid())
                ImHexApi::Provider::get()->redo();
        });
        ShortcutManager::addShortcut(this, CTRLCMD + SHIFT + Keys::Z, [] {
            if (ImHexApi::Provider::isValid())
                ImHexApi::Provider::get()->redo();
        });
    }

    void ViewHexEditor::registerEvents() {
        EventManager::subscribe<RequestSelectionChange>(this, [this](Region region) {
            auto provider = ImHexApi::Provider::get();

            if (region == Region::Invalid()) {
                auto &providerData = ProviderExtraData::get(provider).editor;

                providerData.selectionStart.reset();
                providerData.selectionEnd.reset();
                EventManager::post<EventRegionSelected>(ImHexApi::HexEditor::ProviderRegion({ Region::Invalid(), nullptr }));

                return;
            }

            auto page = provider->getPageOfAddress(region.getStartAddress());
            if (!page.has_value())
                return;

            if (region.size != 0) {
                provider->setCurrentPage(page.value());
                this->setSelection(region);
                this->jumpToSelection();
            }
        });

        EventManager::subscribe<EventProviderChanged>(this, [this](auto *oldProvider, auto *newProvider) {
            if (oldProvider != nullptr) {
                auto &oldData = ProviderExtraData::get(oldProvider).editor;

                auto selection = this->m_hexEditor.getSelection();

                if (selection != Region::Invalid()) {
                    oldData.selectionStart  = selection.getStartAddress();
                    oldData.selectionEnd    = selection.getEndAddress();
                    oldData.scrollPosition  = this->m_hexEditor.getScrollPosition();
                }
            }

            if (newProvider != nullptr) {
                auto &newData = ProviderExtraData::get(newProvider).editor;

                this->m_hexEditor.setSelectionUnchecked(newData.selectionStart, newData.selectionEnd);
                this->m_hexEditor.setScrollPosition(newData.scrollPosition);
            }

            this->m_hexEditor.forceUpdateScrollPosition();
            if (isSelectionValid()) {
                EventManager::post<EventRegionSelected>(ImHexApi::HexEditor::ProviderRegion{ this->getSelection(), newProvider });
            }
        });
    }

    void ViewHexEditor::registerMenuItems() {

        ContentRegistry::Interface::addMenuItemSeparator({ "hex.builtin.menu.file" }, 1300);

        /* Save */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.file", "hex.builtin.view.hex_editor.menu.file.save"_lang }, 1350,
                                                CTRLCMD + Keys::S,
                                                save,
                                                [] {
                                                    auto provider      = ImHexApi::Provider::get();
                                                    bool providerValid = ImHexApi::Provider::isValid();

                                                    return providerValid && provider->isWritable();
                                                });

        /* Save As */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.file", "hex.builtin.view.hex_editor.menu.file.save_as"_lang }, 1375,
                                                CTRLCMD + SHIFT + Keys::S,
                                                saveAs,
                                                [] {
                                                    auto provider      = ImHexApi::Provider::get();
                                                    bool providerValid = ImHexApi::Provider::isValid();

                                                    return providerValid && provider->isWritable();
                                                });

        /* Load Encoding File */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.file", "hex.builtin.menu.file.import", "hex.builtin.menu.file.import.custom_encoding" }, 5050,
                                                Shortcut::None,
                                                [this]{
                                                    std::vector<std::fs::path> paths;
                                                    for (const auto &path : fs::getDefaultPaths(fs::ImHexPath::Encodings)) {
                                                        std::error_code error;
                                                        for (const auto &entry : std::fs::recursive_directory_iterator(path, error)) {
                                                            if (!entry.is_regular_file()) continue;

                                                            paths.push_back(entry);
                                                        }
                                                    }

                                                    View::showFileChooserPopup(paths, { {"Thingy Table File", "tbl"} }, false,
                                                                               [this](const auto &path) {
                                                                                   this->m_hexEditor.setCustomEncoding(EncodingFile(EncodingFile::Type::Thingy, path));
                                                                               });
                                                },
                                                ImHexApi::Provider::isValid);

        ContentRegistry::Interface::addMenuItemSeparator({ "hex.builtin.menu.file" }, 1500);

        /* Search */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.file", "hex.builtin.view.hex_editor.menu.file.search" }, 1550,
                                                CTRLCMD + Keys::F,
                                                [this] {
                                                    this->openPopup<PopupFind>();
                                                },
                                                ImHexApi::Provider::isValid);

        /* Goto */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.file", "hex.builtin.view.hex_editor.menu.file.goto" }, 1600,
                                                CTRLCMD + Keys::G,
                                                [this] {
                                                    this->openPopup<PopupGoto>();
                                                },
                                                ImHexApi::Provider::isValid);

        /* Select */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.file", "hex.builtin.view.hex_editor.menu.file.select" }, 1650,
                                                CTRLCMD + SHIFT + Keys::A,
                                                [this] {
                                                    this->openPopup<PopupSelect>();
                                                },
                                                ImHexApi::Provider::isValid);



        ContentRegistry::Interface::addMenuItemSeparator({ "hex.builtin.menu.edit" }, 1100);

        /* Copy */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.copy" }, 1150,
                                                CurrentView + CTRLCMD + Keys::C,
                                                [] {
                                                    auto selection = ImHexApi::HexEditor::getSelection();
                                                    if (selection.has_value() && selection != Region::Invalid())
                                                        copyBytes(*selection);
                                                },
                                                ImHexApi::HexEditor::isSelectionValid,
                                                this);

        /* Copy As */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.copy_as", "hex.builtin.view.hex_editor.copy.ascii" }, 1200,
                                                CurrentView + CTRLCMD + SHIFT + Keys::C,
                                                [] {
                                                    auto selection = ImHexApi::HexEditor::getSelection();
                                                    if (selection.has_value() && selection != Region::Invalid())
                                                        copyString(*selection);
                                                },
                                                ImHexApi::HexEditor::isSelectionValid,
                                                this);

        /* Copy address */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.copy_as", "hex.builtin.view.hex_editor.copy.address" }, 1250,
                                                Shortcut::None,
                                                [] {
                                                    auto selection = ImHexApi::HexEditor::getSelection();
                                                    if (selection.has_value() && selection != Region::Invalid())
                                                        ImGui::SetClipboardText(hex::format("0x{:08X}", selection->getStartAddress()).c_str());
                                                },
                                                ImHexApi::HexEditor::isSelectionValid);

        /* Copy custom encoding */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.copy_as", "hex.builtin.view.hex_editor.copy.custom_encoding" }, 1300,
                                                Shortcut::None,
                                                [this] {
                                                    auto selection = ImHexApi::HexEditor::getSelection();
                                                    auto customEncoding = this->m_hexEditor.getCustomEncoding();
                                                    if (customEncoding.has_value() && selection.has_value() && selection != Region::Invalid())
                                                        copyCustomEncoding(*customEncoding, *selection);
                                                },
                                                [this] {
                                                    return ImHexApi::HexEditor::isSelectionValid() && this->m_hexEditor.getCustomEncoding().has_value();
                                                });

        ContentRegistry::Interface::addMenuItemSeparator({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.copy_as" }, 1350);

        /* Copy as... */
        ContentRegistry::Interface::addMenuItemSubMenu({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.copy_as" }, 1400, []{
            auto selection = ImHexApi::HexEditor::getSelection();
            auto provider  = ImHexApi::Provider::get();

            for (const auto &[unlocalizedName, callback] : ContentRegistry::DataFormatter::impl::getEntries()) {
                if (ImGui::MenuItem(LangEntry(unlocalizedName))) {
                    ImGui::SetClipboardText(
                            callback(
                                    provider,
                                    selection->getStartAddress() + provider->getBaseAddress() + provider->getCurrentPageAddress(),
                                    selection->size
                            ).c_str()
                    );
                }
            }
        });

        /* Paste */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.paste" }, 1450, CurrentView + CTRLCMD + Keys::V,
                                                [] {
                                                    pasteBytes(*ImHexApi::HexEditor::getSelection(), true);
                                                },
                                                ImHexApi::HexEditor::isSelectionValid,
                                                this);

        /* Paste All */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.paste_all" }, 1500, CurrentView + CTRLCMD + SHIFT + Keys::V,
                                                [] {
                                                    pasteBytes(*ImHexApi::HexEditor::getSelection(), false);
                                                },
                                                ImHexApi::HexEditor::isSelectionValid,
                                                this);

        /* Select All */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.select_all" }, 1550, CurrentView + CTRLCMD + Keys::A,
                                                [] {
                                                    auto provider = ImHexApi::Provider::get();
                                                    ImHexApi::HexEditor::setSelection(provider->getBaseAddress(), provider->getActualSize());
                                                },
                                                ImHexApi::HexEditor::isSelectionValid,
                                                this);


        ContentRegistry::Interface::addMenuItemSeparator({ "hex.builtin.menu.edit" }, 1600);

        /* Set Base Address */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.set_base" }, 1650, Shortcut::None,
                                                [this] {
                                                    auto provider = ImHexApi::Provider::get();
                                                    this->openPopup<PopupBaseAddress>(provider->getBaseAddress());
                                                },
                                                [] { return ImHexApi::Provider::isValid() && ImHexApi::Provider::get()->isReadable(); });

        /* Resize */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.resize" }, 1700, Shortcut::None,
                                                [this] {
                                                    auto provider = ImHexApi::Provider::get();
                                                    this->openPopup<PopupResize>(provider->getBaseAddress());
                                                },
                                                [] { return ImHexApi::Provider::isValid() && ImHexApi::Provider::get()->isResizable(); });

        /* Insert */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.insert" }, 1750, Shortcut::None,
                                                [this] {
                                                    auto selection      = ImHexApi::HexEditor::getSelection();

                                                    this->openPopup<PopupInsert>(selection->getStartAddress(), 0x00);
                                                },
                                                [] { return ImHexApi::HexEditor::isSelectionValid() && ImHexApi::Provider::isValid() && ImHexApi::Provider::get()->isResizable(); });

        /* Remove */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.remove" }, 1800, Shortcut::None,
                                                [this] {
                                                    auto selection      = ImHexApi::HexEditor::getSelection();

                                                    this->openPopup<PopupRemove>(selection->getStartAddress(), selection->getSize());
                                                },
                                                [] { return ImHexApi::HexEditor::isSelectionValid() && ImHexApi::Provider::isValid() && ImHexApi::Provider::get()->isResizable(); });

        /* Jump to */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.jump_to" }, 1850, Shortcut::None,
                                                [] {
                                                    auto provider = ImHexApi::Provider::get();
                                                    auto selection      = ImHexApi::HexEditor::getSelection();

                                                    u64 value = 0;
                                                    provider->read(selection->getStartAddress(), &value, selection->getSize());

                                                    if (value < provider->getBaseAddress() + provider->getActualSize()) {
                                                        ImHexApi::HexEditor::setSelection(value, 1);
                                                    }
                                                },
                                                [] { return ImHexApi::Provider::isValid() && ImHexApi::HexEditor::isSelectionValid() && ImHexApi::HexEditor::getSelection()->getSize() <= sizeof(u64); });
                                                        // Popups

        ContentRegistry::Interface::addMenuItemSeparator({ "hex.builtin.menu.edit" }, 1900);

        /* Open in new provider */
        ContentRegistry::Interface::addMenuItem({ "hex.builtin.menu.edit", "hex.builtin.view.hex_editor.menu.edit.open_in_new_provider" }, 1950, Shortcut::None,
                                                [] {
                                                    auto selection = ImHexApi::HexEditor::getSelection();

                                                    auto newProvider = ImHexApi::Provider::createProvider("hex.builtin.provider.view", true);
                                                    if (auto *viewProvider = dynamic_cast<ViewProvider*>(newProvider); viewProvider != nullptr) {
                                                        viewProvider->setProvider(selection->getStartAddress(), selection->getSize(), selection->getProvider());
                                                        if (viewProvider->open())
                                                            EventManager::post<EventProviderOpened>(viewProvider);
                                                    }
                                                },
                                                [] { return ImHexApi::HexEditor::isSelectionValid() && ImHexApi::Provider::isValid(); });
    }

}