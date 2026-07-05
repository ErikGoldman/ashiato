#include "debugger_app.hpp"

#include "debugger_app_internal.hpp"

#include <algorithm>
#include <memory>

#include <imgui.h>

namespace ashiato::debugger {
namespace {

constexpr double auto_refresh_seconds = 1.5;
constexpr float splitter_width = 6.0f;
constexpr float min_entity_panel_width = 170.0f;
constexpr float min_detail_panel_width = 320.0f;
constexpr float min_jobs_panel_width = 260.0f;

float clamp_panel_width(float value, float min_value, float max_value) {
    return std::min(std::max(value, min_value), std::max(min_value, max_value));
}

void draw_vertical_splitter(const char* id, float height, float& left_width, float& right_width) {
    ImGui::PushID(id);
    ImGui::InvisibleButton("splitter", ImVec2(splitter_width, height));
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    const ImU32 color = ImGui::GetColorU32(
        active ? ImGuiCol_ButtonActive : (hovered ? ImGuiCol_ButtonHovered : ImGuiCol_FrameBg));
    const float center_x = (min.x + max.x) * 0.5f;
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(center_x - 1.0f, min.y),
        ImVec2(center_x + 1.0f, max.y),
        color,
        1.0f);
    if (hovered) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (active) {
        const float delta = ImGui::GetIO().MouseDelta.x;
        left_width += delta;
        right_width -= delta;
    }
    ImGui::PopID();
}

void clamp_panel_widths(DebuggerAppState& app, float total_panel_width) {
    const float min_total_width = min_entity_panel_width + min_detail_panel_width + min_jobs_panel_width;
    if (total_panel_width < min_total_width) {
        app.entity_panel_width = std::max(90.0f, total_panel_width * 0.20f);
        app.detail_panel_width = std::max(140.0f, total_panel_width * 0.45f);
        const float max_detail_width = std::max(80.0f, total_panel_width - app.entity_panel_width - 90.0f);
        app.detail_panel_width = std::min(app.detail_panel_width, max_detail_width);
        app.panel_widths_initialized = true;
        return;
    }

    if (!app.panel_widths_initialized) {
        app.entity_panel_width = clamp_panel_width(total_panel_width * 0.16f, min_entity_panel_width, 230.0f);
        app.detail_panel_width =
            clamp_panel_width(total_panel_width * 0.45f, min_detail_panel_width, total_panel_width);
        app.panel_widths_initialized = true;
    }

    const float max_entity_width = total_panel_width - min_detail_panel_width - min_jobs_panel_width;
    app.entity_panel_width = clamp_panel_width(app.entity_panel_width, min_entity_panel_width, max_entity_width);

    const float max_detail_width = total_panel_width - app.entity_panel_width - min_jobs_panel_width;
    app.detail_panel_width = clamp_panel_width(app.detail_panel_width, min_detail_panel_width, max_detail_width);
}

void draw_app(DebuggerAppState& app) {
    poll_refresh(app);
    const double now = ImGui::GetTime();
    if (app.auto_refresh && !app.loading && now >= app.next_auto_refresh) {
        app.next_auto_refresh = now + auto_refresh_seconds;
        start_refresh(app);
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin(
        "Ashiato Debugger",
        nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoSavedSettings);

    draw_server_strip(app);
    const DiscoveredServer* server = selected_server(app.data);
    if (server == nullptr) {
        draw_empty(app);
    } else {
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetContentRegionAvail().y;
        const float total_panel_width = std::max(1.0f, width - splitter_width * 2.0f);
        clamp_panel_widths(app, total_panel_width);
        float jobs_width = total_panel_width - app.entity_panel_width - app.detail_panel_width;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
        ImGui::BeginGroup();
        draw_entity_list(app, server->snapshot, ImVec2(app.entity_panel_width, height));
        ImGui::EndGroup();
        ImGui::SameLine();
        draw_vertical_splitter("entity_detail_splitter", height, app.entity_panel_width, app.detail_panel_width);
        clamp_panel_widths(app, total_panel_width);
        jobs_width = total_panel_width - app.entity_panel_width - app.detail_panel_width;
        ImGui::SameLine();
        ImGui::BeginGroup();
        draw_entity_detail(app, server->snapshot, ImVec2(app.detail_panel_width, height));
        ImGui::EndGroup();
        ImGui::SameLine();
        draw_vertical_splitter("detail_jobs_splitter", height, app.detail_panel_width, jobs_width);
        app.detail_panel_width = total_panel_width - app.entity_panel_width - jobs_width;
        clamp_panel_widths(app, total_panel_width);
        jobs_width = total_panel_width - app.entity_panel_width - app.detail_panel_width;
        ImGui::SameLine();
        ImGui::BeginGroup();
        draw_jobs(app, server->snapshot, ImVec2(jobs_width, height));
        ImGui::EndGroup();
        ImGui::PopStyleVar();
    }

    app.restore_scroll_after_refresh = false;
    ImGui::End();
}

}  // namespace

DebuggerApp::DebuggerApp() : state_(std::make_unique<DebuggerAppState>()) {
    state_->auto_refresh = true;
    start_refresh(*state_);
}

DebuggerApp::~DebuggerApp() = default;
DebuggerApp::DebuggerApp(DebuggerApp&&) noexcept = default;
DebuggerApp& DebuggerApp::operator=(DebuggerApp&&) noexcept = default;

void DebuggerApp::draw() {
    draw_app(*state_);
}

void apply_debugger_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 7.0f);
    style.ScrollbarSize = 14.0f;
    style.WindowRounding = 0.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 5.0f;
    style.TabRounding = 5.0f;
    style.GrabRounding = 5.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.060f, 0.070f, 1.0f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.075f, 0.082f, 0.095f, 1.0f);
    colors[ImGuiCol_FrameBg] = ImVec4(0.115f, 0.128f, 0.150f, 1.0f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.155f, 0.178f, 0.210f, 1.0f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.190f, 0.225f, 0.270f, 1.0f);
    colors[ImGuiCol_Header] = ImVec4(0.135f, 0.160f, 0.195f, 1.0f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.185f, 0.225f, 0.275f, 1.0f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.225f, 0.275f, 0.340f, 1.0f);
    colors[ImGuiCol_Button] = ImVec4(0.135f, 0.158f, 0.190f, 1.0f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.190f, 0.235f, 0.290f, 1.0f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.250f, 0.320f, 0.390f, 1.0f);
    colors[ImGuiCol_CheckMark] = ImVec4(0.330f, 0.680f, 0.820f, 1.0f);
}

}  // namespace ashiato::debugger
