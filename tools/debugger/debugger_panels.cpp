#include "debugger_app_internal.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace ashiato::debugger {
namespace {

std::string display_server_name(const DiscoveredServer& server) {
    if (!server.snapshot.name.empty()) {
        return server.snapshot.name + " :" + std::to_string(server.port);
    }
    return "Ashiato :" + std::to_string(server.port);
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool text_matches(const std::string& value, const std::string& query) {
    return query.empty() || lower_copy(value).find(lower_copy(query)) != std::string::npos;
}

std::vector<const EntitySummary*> filtered_entities(
    const ServerSnapshot& snapshot,
    const std::string& query,
    bool user_only) {
    std::vector<const EntitySummary*> entities;
    for (const EntitySummary& entity : snapshot.entities) {
        if ((!user_only || entity.kind == "User") && matches_entity(entity, query)) {
            entities.push_back(&entity);
        }
    }
    return entities;
}

std::vector<const SingletonInstance*> filtered_singletons(const ServerSnapshot& snapshot, const std::string& query) {
    std::vector<const SingletonInstance*> singletons;
    for (const SingletonInstance& singleton : snapshot.singletons) {
        if (matches_singleton(singleton, query)) {
            singletons.push_back(&singleton);
        }
    }
    return singletons;
}

std::vector<const JobSummary*> filtered_jobs(const ServerSnapshot& snapshot, const std::string& query) {
    std::vector<const JobSummary*> jobs;
    for (const JobSummary& job : snapshot.jobs) {
        if (matches_job(job, query)) {
            jobs.push_back(&job);
        }
    }
    return jobs;
}

const SingletonInstance* selected_singleton(const DebuggerState& state, const ServerSnapshot& snapshot) {
    const auto found = std::find_if(snapshot.singletons.begin(), snapshot.singletons.end(), [&](const SingletonInstance& singleton) {
        return singleton.component.component == state.selected_singleton_component_id;
    });
    return found != snapshot.singletons.end() ? &*found : nullptr;
}

const JobSummary* selected_job_summary(const DebuggerState& state, const ServerSnapshot& snapshot) {
    const auto found = std::find_if(snapshot.jobs.begin(), snapshot.jobs.end(), [&](const JobSummary& job) {
        return job.id == state.selected_job_id;
    });
    return found != snapshot.jobs.end() ? &*found : nullptr;
}

void start_selection_refresh(DebuggerAppState& app) {
    app.status_error.clear();
    start_refresh(app);
}

void draw_detail_title(const char* text) {
    ImGui::SetWindowFontScale(1.18f);
    ImGui::Text("%s", text);
    ImGui::SetWindowFontScale(1.0f);
}

void draw_entity_summary(const EntitySummary& entity) {
    draw_detail_title(entity_label(entity).c_str());
    ImGui::TextDisabled(
        "id %s  index %d  version %d  kind %s",
        entity.id.c_str(),
        entity.index,
        entity.version,
        entity.kind.c_str());
}

std::string compact_entity_label(const EntitySummary& entity) {
    return entity_label(entity) + "  #" + std::to_string(entity.index) + " v" + std::to_string(entity.version) +
        "  " + entity.kind;
}

void draw_section_header(const char* label) {
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::SetWindowFontScale(1.08f);
    ImGui::SeparatorText(label);
    ImGui::SetWindowFontScale(1.0f);
}

const char* component_display_name(const ComponentInstance& component) {
    return !component.name.empty() ? component.name.c_str() : component.component.c_str();
}

const char* tag_display_name(const RegisteredTag& tag) {
    return !tag.name.empty() ? tag.name.c_str() : tag.component.c_str();
}

bool entity_has_component(const EntityDetail& entity, const std::string& component_id) {
    return std::any_of(entity.components.begin(), entity.components.end(), [&](const ComponentInstance& component) {
        return component.component == component_id;
    });
}

bool component_matches_filter(const ComponentInstance& component, const std::string& query) {
    return text_matches(component_display_name(component), query) || text_matches(component.component, query);
}

void remove_entity_component(DebuggerAppState& app, const std::string& entity_id, const std::string& component_id) {
    const DiscoveredServer* server = selected_server(app.data);
    if (server == nullptr) {
        return;
    }

    try {
        GraphQLClient client(server->port);
        if (remove_component(client, entity_id, component_id)) {
            start_selection_refresh(app);
        }
    } catch (const std::exception& exception) {
        app.status_error = exception.what();
    }
}

void add_entity_tag(DebuggerAppState& app, const std::string& entity_id, const std::string& component_id) {
    const DiscoveredServer* server = selected_server(app.data);
    if (server == nullptr) {
        return;
    }

    try {
        GraphQLClient client(server->port);
        if (add_tag(client, entity_id, component_id)) {
            app.add_tag_query.clear();
            ImGui::CloseCurrentPopup();
            start_selection_refresh(app);
        }
    } catch (const std::exception& exception) {
        app.status_error = exception.what();
    }
}

void draw_add_tag_popup(DebuggerAppState& app, const ServerSnapshot& snapshot, const EntityDetail& entity) {
    if (ImGui::Button("Add Tag")) {
        ImGui::OpenPopup("add_tag_popup");
    }

    if (!ImGui::BeginPopup("add_tag_popup")) {
        return;
    }

    ImGui::SetNextItemWidth(260.0f);
    std::array<char, 256> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", app.add_tag_query.c_str());
    if (ImGui::InputTextWithHint("##add_tag_filter", "filter tags", buffer.data(), buffer.size())) {
        app.add_tag_query = buffer.data();
    }
    ImGui::Separator();

    bool drew_tag = false;
    for (const RegisteredTag& tag : snapshot.registered_tags) {
        if (entity_has_component(entity, tag.component) ||
            (!text_matches(tag.name, app.add_tag_query) && !text_matches(tag.component, app.add_tag_query))) {
            continue;
        }

        ImGui::PushID(tag.component.c_str());
        if (ImGui::Selectable(tag_display_name(tag))) {
            add_entity_tag(app, entity.id, tag.component);
        }
        ImGui::PopID();
        drew_tag = true;
    }

    if (!drew_tag) {
        ImGui::TextDisabled("No tags available.");
    }
    ImGui::EndPopup();
}

void draw_tags_section(DebuggerAppState& app, const ServerSnapshot& snapshot, const EntityDetail& entity) {
    draw_section_header("Tags");
    draw_add_tag_popup(app, snapshot, entity);
    ImGui::Spacing();

    bool drew_tag = false;
    for (const ComponentInstance& component : entity.components) {
        if (!component.tag) {
            continue;
        }

        ImGui::PushID(component.component.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.46f, 0.86f, 0.63f, 1.0f));
        const bool clicked = ImGui::Selectable(component_display_name(component), false);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImGui::SetTooltip("Remove?");
        }
        if (clicked) {
            remove_entity_component(app, entity.id, component.component);
        }
        ImGui::PopID();
        drew_tag = true;
    }

    if (!drew_tag) {
        ImGui::TextDisabled("No tags on this entity.");
    }
}

void draw_components_section(DebuggerAppState& app, const EntityDetail& entity) {
    draw_section_header("Components");
    std::array<char, 512> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", app.component_query.c_str());
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##component_search", "filter components", buffer.data(), buffer.size())) {
        app.component_query = buffer.data();
    }
    ImGui::Spacing();

    bool has_component = false;
    bool drew_component = false;
    for (const ComponentInstance& component : entity.components) {
        if (component.tag) {
            continue;
        }
        has_component = true;
        if (!component_matches_filter(component, app.component_query)) {
            continue;
        }
        draw_component_card(app, entity.id, component);
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        drew_component = true;
    }
    if (!drew_component) {
        ImGui::TextDisabled(
            has_component ? "No components match the current filter." : "No non-tag components on this entity.");
    }
}

bool job_matches_selected_entity(const DebuggerAppState& app, const std::string& job_id) {
    if (!app.data.entity_detail || app.data.entity_detail->id != app.data.selected_entity_id) {
        return false;
    }

    const std::vector<JobSummary>& matching_jobs = app.data.entity_detail->matching_jobs;
    return std::any_of(matching_jobs.begin(), matching_jobs.end(), [&](const JobSummary& job) {
        return job.id == job_id;
    });
}

void draw_job_match_square(bool matches) {
    if (matches) {
        ImGui::ColorButton(
            "##matches_entity",
            ImVec4(0.22f, 0.78f, 0.38f, 1.0f),
            ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
            ImVec2(10.0f, 10.0f));
    } else {
        ImGui::Dummy(ImVec2(10.0f, 10.0f));
    }
}

void draw_job_list_row(DebuggerAppState& app, const JobSummary& job) {
    ImGui::PushID(job.id.c_str());
    draw_job_match_square(job_matches_selected_entity(app, job.id));
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(4.0f, 0.0f));
    ImGui::SameLine();

    const bool selected = job.id == app.data.selected_job_id;
    const std::string label = std::string("  ") + job_title(job) + "##job";
    const float row_start_x = ImGui::GetCursorPosX();
    const float matches_width = ImGui::CalcTextSize(std::to_string(job.matching_entity_count).c_str()).x;
    const float right_padding = 14.0f;
    const float selectable_width =
        std::max(80.0f, ImGui::GetContentRegionAvail().x - matches_width - right_padding);
    if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(selectable_width, 0.0f))) {
        app.data.selected_job_id = job.id;
        app.data.job_detail.reset();
        start_selection_refresh(app);
    }

    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), row_start_x + selectable_width + 8.0f));
    ImGui::TextDisabled("%d", job.matching_entity_count);
    ImGui::PopID();
}

}  // namespace

bool begin_debugger_pane(DebuggerAppState& app, const char* id, const ImVec2& size) {
    const bool visible = ImGui::BeginChild(id, size, true);
    if (app.restore_scroll_after_refresh) {
        const auto found = app.scroll_y.find(id);
        if (found != app.scroll_y.end()) {
            ImGui::SetScrollY(found->second);
        }
    }
    return visible;
}

void end_debugger_pane(DebuggerAppState& app, const char* id) {
    app.scroll_y[id] = ImGui::GetScrollY();
    ImGui::EndChild();
}

void debugger_input_text(const char* label, std::string& value) {
    std::array<char, 512> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
    if (ImGui::InputText(label, buffer.data(), buffer.size())) {
        value = buffer.data();
    }
}

void draw_server_strip(DebuggerAppState& app) {
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Servers");
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("Base Port");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(104.0f);
    ImGui::InputInt("##base_port", &app.base_port, 1, 16);
    ImGui::SameLine();
    if (app.auto_refresh) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Refresh")) {
        start_refresh(app);
    }
    if (app.auto_refresh) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Auto", &app.auto_refresh);

    if (!app.status_error.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", app.status_error.c_str());
    }

    ImGui::Spacing();
    for (const DiscoveredServer& server : app.data.servers) {
        if (!server.ok) {
            continue;
        }
        ImGui::PushID(server.port);
        const bool selected = server.port == app.data.selected_port;
        const std::string label = display_server_name(server);
        if (ImGui::Selectable(label.c_str(), selected, 0, ImVec2(170.0f, 0.0f))) {
            app.data.selected_port = server.port;
            app.data.entity_detail.reset();
            app.data.job_detail.reset();
            start_selection_refresh(app);
        }
        ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::NewLine();
    ImGui::Separator();
}

void draw_entity_list(DebuggerAppState& app, const ServerSnapshot& snapshot, const ImVec2& size) {
    if (begin_debugger_pane(app, "entities", size)) {
        if (ImGui::BeginTabBar("entity_source_tabs")) {
            if (ImGui::BeginTabItem("Entities")) {
                app.data.singleton_tab = false;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Singletons")) {
                app.data.singleton_tab = true;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        if (!app.data.singleton_tab) {
            ImGui::Checkbox("Show User Only", &app.show_user_entities_only);
        }
        std::array<char, 512> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%s", app.entity_query.c_str());
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::InputTextWithHint("##entity_search", "filter", buffer.data(), buffer.size())) {
            app.entity_query = buffer.data();
        }
        ImGui::Separator();
        ImGui::Spacing();

        if (app.data.singleton_tab) {
            for (const SingletonInstance* singleton : filtered_singletons(snapshot, app.entity_query)) {
                const bool selected = singleton->component.component == app.data.selected_singleton_component_id;
                const std::string label =
                    std::string("  ") +
                    (!singleton->component.name.empty() ? singleton->component.name : singleton->component.component) +
                    "  #" + std::to_string(singleton->entity.index) + " v" +
                    std::to_string(singleton->entity.version) +
                    "##" + singleton->component.component;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    app.data.selected_singleton_component_id = singleton->component.component;
                    app.data.singleton_tab = true;
                }
            }
        } else {
            for (const EntitySummary* entity :
                 filtered_entities(snapshot, app.entity_query, app.show_user_entities_only)) {
                const bool selected = entity->id == app.data.selected_entity_id;
                const std::string label = std::string("  ") + compact_entity_label(*entity) + "##" + entity->id;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    app.data.selected_entity_id = entity->id;
                    app.data.singleton_tab = false;
                    app.data.entity_detail.reset();
                    start_selection_refresh(app);
                }
            }
        }
    }
    end_debugger_pane(app, "entities");
}

void draw_entity_detail(DebuggerAppState& app, const ServerSnapshot& snapshot, const ImVec2& size) {
    if (begin_debugger_pane(app, "entity_detail", size)) {
        if (app.data.singleton_tab) {
            const SingletonInstance* singleton = selected_singleton(app.data, snapshot);
            if (singleton == nullptr) {
                ImGui::TextDisabled("No singleton selected.");
            } else {
                ImGui::Text("Singleton");
                draw_entity_summary(singleton->entity);
                draw_section_header("Component");
                draw_component_card(app, singleton->entity.id, singleton->component);
            }
        } else if (app.data.selected_entity_id.empty()) {
            ImGui::TextDisabled("No entity selected.");
        } else if (!app.data.entity_detail || app.data.entity_detail->id != app.data.selected_entity_id) {
            ImGui::TextDisabled(app.loading ? "Loading entity detail..." : "Entity detail not loaded.");
        } else {
            const EntityDetail& entity = *app.data.entity_detail;
            draw_entity_summary(entity);
            draw_tags_section(app, snapshot, entity);
            draw_components_section(app, entity);
            draw_section_header("Matching Jobs");
            for (const JobSummary& job : entity.matching_jobs) {
                const std::string label = std::string("  ") + job_title(job) + "##matching-job-" + job.id;
                if (ImGui::Selectable(label.c_str(), job.id == app.data.selected_job_id)) {
                    app.data.selected_job_id = job.id;
                    app.data.job_detail.reset();
                    start_selection_refresh(app);
                }
            }
        }
    }
    end_debugger_pane(app, "entity_detail");
}

void draw_jobs(DebuggerAppState& app, const ServerSnapshot& snapshot, const ImVec2& size) {
    if (begin_debugger_pane(app, "jobs", size)) {
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const float list_height = std::max(180.0f, size.y * 0.46f);
        if (begin_debugger_pane(app, "jobs_list", ImVec2(0.0f, list_height))) {
            ImGui::Text("Jobs");
            std::array<char, 512> buffer{};
            std::snprintf(buffer.data(), buffer.size(), "%s", app.job_query.c_str());
            ImGui::SetNextItemWidth(-1.0f);
            if (ImGui::InputTextWithHint("##jobs_search", "filter jobs", buffer.data(), buffer.size())) {
                app.job_query = buffer.data();
            }
            ImGui::Separator();

            for (const JobSummary* job : filtered_jobs(snapshot, app.job_query)) {
                draw_job_list_row(app, *job);
            }
        }
        end_debugger_pane(app, "jobs_list");

        ImGui::Dummy(ImVec2(0.0f, spacing));
        if (begin_debugger_pane(app, "selected_job", ImVec2(0.0f, 0.0f))) {
            const JobSummary* summary = selected_job_summary(app.data, snapshot);
            if (summary == nullptr) {
                ImGui::TextDisabled("No job selected.");
            } else {
                draw_job_detail(app, *summary);
            }
        }
        end_debugger_pane(app, "selected_job");
    }
    end_debugger_pane(app, "jobs");
}

void draw_empty(DebuggerAppState& app) {
    const ImVec2 size = ImGui::GetContentRegionAvail();
    if (begin_debugger_pane(app, "empty", size)) {
        ImGui::Text("No Ashiato debug server is reachable.");
        ImGui::TextDisabled("Start an Unreal session with the Ashiato debug server enabled, then refresh.");
    }
    end_debugger_pane(app, "empty");
}

}  // namespace ashiato::debugger
