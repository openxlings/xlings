// luban.desktop.imgui: Dear ImGui as the Luban OS app draws with it, imgui.h
// read here and in the host's own backends only. Every name the app uses is
// on this list; one it starts to use is added here, not #included where it
// is used (tools/lint_c_headers.sh). IM_COL32, a macro there, is a function.
module;

#include <imgui.h>
#undef IM_COL32   // the function below takes its place

export module luban.desktop.imgui;

export using ::ImVec2;
export using ::ImVec4;
export using ::ImU32;
export using ::ImFont;
export using ::ImTextureID;
export using ::ImDrawList;
export using ::ImGuiStyle;
export using ::ImGuiViewport;
export using ::ImGuiInputTextCallback;
export using ::ImGuiCol_;
export using ::ImGuiKey;
export using ::ImGuiMouseCursor_;
export using ::ImGuiStyleVar_;
export using ::ImGuiWindowFlags_;
export using ::ImGuiChildFlags_;
export using ::ImGuiTableFlags_;
export using ::ImGuiChildFlags_Borders;
export using ::ImGuiCol_Button;
export using ::ImGuiCol_Header;
export using ::ImGuiCol_HeaderActive;
export using ::ImGuiCol_Text;
export using ::ImGuiCol_TextDisabled;
export using ::ImGuiKey_Escape;
export using ::ImGuiMouseCursor_None;
export using ::ImGuiStyleVar_WindowPadding;
export using ::ImGuiTableFlags_Borders;
export using ::ImGuiTableFlags_RowBg;
export using ::ImGuiWindowFlags_AlwaysAutoResize;
export using ::ImGuiWindowFlags_NoBringToFrontOnFocus;
export using ::ImGuiWindowFlags_NoDecoration;
export using ::ImGuiWindowFlags_NoMove;
export using ::ImGuiWindowFlags_NoSavedSettings;
export using ::ImGuiWindowFlags_NoTitleBar;

export constexpr ImU32 IM_COL32(int r, int g, int b, int a) {
    return static_cast<ImU32>(a) << IM_COL32_A_SHIFT | static_cast<ImU32>(b) << IM_COL32_B_SHIFT
           | static_cast<ImU32>(g) << IM_COL32_G_SHIFT | static_cast<ImU32>(r) << IM_COL32_R_SHIFT;
}

export namespace ImGui {
using ::ImGui::AlignTextToFramePadding;
using ::ImGui::Begin;
using ::ImGui::BeginChild;
using ::ImGui::BeginPopupModal;
using ::ImGui::BeginTabBar;
using ::ImGui::BeginTabItem;
using ::ImGui::BeginTable;
using ::ImGui::Button;
using ::ImGui::CalcTextSize;
using ::ImGui::Checkbox;
using ::ImGui::CloseCurrentPopup;
using ::ImGui::CollapsingHeader;
using ::ImGui::Combo;
using ::ImGui::Dummy;
using ::ImGui::End;
using ::ImGui::EndChild;
using ::ImGui::EndPopup;
using ::ImGui::EndTabBar;
using ::ImGui::EndTabItem;
using ::ImGui::EndTable;
using ::ImGui::GetColorU32;
using ::ImGui::GetContentRegionAvail;
using ::ImGui::GetCursorPosX;
using ::ImGui::GetCursorScreenPos;
using ::ImGui::GetItemRectMax;
using ::ImGui::GetItemRectMin;
using ::ImGui::GetMainViewport;
using ::ImGui::GetScrollMaxY;
using ::ImGui::GetScrollY;
using ::ImGui::GetStyle;
using ::ImGui::GetStyleColorVec4;
using ::ImGui::GetTextLineHeight;
using ::ImGui::GetTime;
using ::ImGui::GetWindowDrawList;
using ::ImGui::GetWindowHeight;
using ::ImGui::GetWindowWidth;
using ::ImGui::InputText;
using ::ImGui::InvisibleButton;
using ::ImGui::IsItemHovered;
using ::ImGui::IsKeyPressed;
using ::ImGui::OpenPopup;
using ::ImGui::PopFont;
using ::ImGui::PopID;
using ::ImGui::PopStyleColor;
using ::ImGui::PopStyleVar;
using ::ImGui::ProgressBar;
using ::ImGui::PushFont;
using ::ImGui::PushID;
using ::ImGui::PushStyleColor;
using ::ImGui::PushStyleVar;
using ::ImGui::RadioButton;
using ::ImGui::SameLine;
using ::ImGui::Selectable;
using ::ImGui::Separator;
using ::ImGui::SetCursorPos;
using ::ImGui::SetCursorPosX;
using ::ImGui::SetCursorPosY;
using ::ImGui::SetCursorScreenPos;
using ::ImGui::SetMouseCursor;
using ::ImGui::SetNextItemWidth;
using ::ImGui::SetNextWindowPos;
using ::ImGui::SetNextWindowSize;
using ::ImGui::SetScrollHereY;
using ::ImGui::SetTooltip;
using ::ImGui::SetWindowFontScale;
using ::ImGui::SliderInt;
using ::ImGui::TableNextColumn;
using ::ImGui::TableNextRow;
using ::ImGui::Text;
using ::ImGui::TextColored;
using ::ImGui::TextDisabled;
using ::ImGui::TextUnformatted;
using ::ImGui::GetFont;
using ::ImGui::GetFontSize;
using ::ImGui::GetCursorPosY;
using ::ImGui::PushTextWrapPos;
using ::ImGui::PopTextWrapPos;
using ::ImGui::BeginDisabled;
using ::ImGui::EndDisabled;
}  // namespace ImGui
