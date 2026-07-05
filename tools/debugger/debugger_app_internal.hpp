#pragma once

#include "debugger_core.hpp"

#include <future>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <imgui.h>

namespace ashiato::debugger {

struct RefreshResult {
    std::vector<DiscoveredServer> servers;
    std::optional<EntityDetail> entity_detail;
    std::optional<JobDetail> job_detail;
    std::string entity_detail_id;
    std::string job_detail_id;
    std::string error;
};

struct DebuggerAppState {
    DebuggerState data;
    int base_port = 8080;
    bool auto_refresh = false;
    bool loading = false;
    double next_auto_refresh = 0.0;
    std::string status_error;
    std::string entity_query;
    std::string component_query;
    bool show_user_entities_only = true;
    std::string job_query;
    std::string add_tag_query;
    std::map<std::string, std::string> edit_buffers;
    std::map<std::string, bool> dirty_edit_buffers;
    std::string active_edit_key;
    std::map<std::string, float> scroll_y;
    std::future<RefreshResult> refresh_task;
    bool restore_scroll_after_refresh = false;
    bool panel_widths_initialized = false;
    float entity_panel_width = 190.0f;
    float detail_panel_width = 560.0f;
};

void start_refresh(DebuggerAppState& app);
void poll_refresh(DebuggerAppState& app);

void draw_server_strip(DebuggerAppState& app);
void draw_entity_list(DebuggerAppState& app, const ServerSnapshot& snapshot, const ImVec2& size);
void draw_entity_detail(DebuggerAppState& app, const ServerSnapshot& snapshot, const ImVec2& size);
void draw_jobs(DebuggerAppState& app, const ServerSnapshot& snapshot, const ImVec2& size);
void draw_empty(DebuggerAppState& app);

bool begin_debugger_pane(DebuggerAppState& app, const char* id, const ImVec2& size);
void end_debugger_pane(DebuggerAppState& app, const char* id);
void debugger_input_text(const char* label, std::string& value);
void draw_component_card(DebuggerAppState& app, const std::string& entity_id, const ComponentInstance& component);
void draw_job_detail(DebuggerAppState& app, const JobSummary& job);

}  // namespace ashiato::debugger
