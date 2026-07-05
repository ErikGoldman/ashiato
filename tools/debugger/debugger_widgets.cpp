#include "debugger_app_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace ashiato::debugger {
namespace {

const ComponentInstance* selected_entity_component(const DebuggerAppState& app, const ComponentRef& ref) {
    if (ref.singleton || !app.data.entity_detail || app.data.entity_detail->id != app.data.selected_entity_id) {
        return nullptr;
    }

    const auto found =
        std::find_if(app.data.entity_detail->components.begin(), app.data.entity_detail->components.end(), [&](const ComponentInstance& component) {
            return component.component == ref.component;
        });
    return found != app.data.entity_detail->components.end() ? &*found : nullptr;
}

bool selected_entity_context_loaded(const DebuggerAppState& app) {
    return app.data.entity_detail && app.data.entity_detail->id == app.data.selected_entity_id;
}

bool required_ref_mismatches_selected_entity(const DebuggerAppState& app, const ComponentRef& ref) {
    return !ref.singleton && selected_entity_context_loaded(app) && selected_entity_component(app, ref) == nullptr;
}

bool without_ref_mismatches_selected_entity(const DebuggerAppState& app, const ComponentRef& ref) {
    return !ref.singleton && selected_entity_context_loaded(app) && selected_entity_component(app, ref) != nullptr;
}

bool component_ref_is_registered_tag(const DebuggerAppState& app, const ComponentRef& ref) {
    const DiscoveredServer* server = selected_server(app.data);
    if (server == nullptr) {
        return false;
    }

    return std::any_of(server->snapshot.registered_tags.begin(), server->snapshot.registered_tags.end(), [&](const RegisteredTag& tag) {
        return tag.component == ref.component;
    });
}

void draw_detail_title(const std::string& text) {
    ImGui::SetWindowFontScale(1.18f);
    ImGui::Text("%s", text.c_str());
    ImGui::SetWindowFontScale(1.0f);
}

void draw_section_header(const char* label) {
    ImGui::SetWindowFontScale(1.08f);
    ImGui::SeparatorText(label);
    ImGui::SetWindowFontScale(1.0f);
}

std::string compact_entity_label(const EntitySummary& entity) {
    return entity_label(entity) + "  #" + std::to_string(entity.index) + " v" + std::to_string(entity.version) +
        "  " + entity.kind;
}

void draw_component_ref_row(DebuggerAppState& app, const ComponentRef& ref, bool mismatch, const char* mismatch_reason) {
    const ComponentInstance* entity_component = selected_entity_component(app, ref);
    const bool tag = entity_component != nullptr ? entity_component->tag : component_ref_is_registered_tag(app, ref);
    const char* kind = tag ? "tag" : "component";
    const std::string name = component_ref_name(ref) + (ref.singleton ? " [singleton]" : "") + (tag ? " [tag]" : "");

    if (mismatch) {
        ImGui::TextColored(ImVec4(1.0f, 0.34f, 0.30f, 1.0f), "%s", name.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s %s", mismatch_reason, kind);
        }
    } else {
        ImGui::Text("%s", name.c_str());
    }
    ImGui::TextDisabled("  %s", ref.component.c_str());
}

void draw_component_refs(DebuggerAppState& app, const char* label, const std::vector<ComponentRef>& refs, bool without) {
    if (refs.empty()) {
        return;
    }

    draw_section_header(label);
    for (const ComponentRef& ref : refs) {
        const bool mismatch = without ? without_ref_mismatches_selected_entity(app, ref)
                                      : required_ref_mismatches_selected_entity(app, ref);
        draw_component_ref_row(
            app,
            ref,
            mismatch,
            without ? "Selected entity has excluded" : "Selected entity is missing required");
    }
}

void remove_component_from_entity(DebuggerAppState& app, const std::string& entity_id, const std::string& component_id) {
    const DiscoveredServer* server = selected_server(app.data);
    if (server == nullptr) {
        return;
    }

    try {
        GraphQLClient client(server->port);
        if (remove_component(client, entity_id, component_id)) {
            start_refresh(app);
        }
    } catch (const std::exception& exception) {
        app.status_error = exception.what();
    }
}

std::string field_buffer_value(
    DebuggerAppState& app,
    const std::string& entity_id,
    const ComponentInstance& component,
    const ComponentFieldValue& field) {
    const std::string key = field_edit_key(entity_id, component.component, field.name);
    const bool dirty = app.dirty_edit_buffers[key];
    if (!dirty && app.active_edit_key != key) {
        app.edit_buffers[key] = field.value;
    }
    return key;
}

bool input_field_value(const char* label, std::string& value) {
    std::array<char, 512> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
    if (!ImGui::InputText(label, buffer.data(), buffer.size())) {
        return false;
    }
    value = buffer.data();
    return true;
}

void save_field(
    DebuggerAppState& app,
    const std::string& entity_id,
    const ComponentInstance& component,
    const ComponentFieldValue& field,
    const std::string& key) {
    const DiscoveredServer* server = selected_server(app.data);
    if (server == nullptr) {
        return;
    }

    try {
        GraphQLClient client(server->port);
        if (set_component_field(client, entity_id, component.component, field, app.edit_buffers[key])) {
            app.dirty_edit_buffers[key] = false;
            app.active_edit_key.clear();
            start_refresh(app);
        }
    } catch (const std::exception& exception) {
        app.status_error = exception.what();
    }
}

void draw_component_field(
    DebuggerAppState& app,
    const std::string& entity_id,
    const ComponentInstance& component,
    const ComponentFieldValue& field) {
    const std::string key = field_buffer_value(app, entity_id, component, field);
    ImGui::PushID(key.c_str());
    if (ImGui::BeginTable("field_row", 4, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("type", ImGuiTableColumnFlags_WidthFixed, 84.0f);
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.35f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("actions", ImGuiTableColumnFlags_WidthFixed, 116.0f);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", field.type.c_str());

        ImGui::TableSetColumnIndex(1);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", field.name.c_str());

        ImGui::TableSetColumnIndex(2);
        ImGui::SetNextItemWidth(-1.0f);
        if (field.is_bool) {
            bool value = app.edit_buffers[key] == "true" || app.edit_buffers[key] == "1";
            if (ImGui::Checkbox("##value", &value)) {
                app.edit_buffers[key] = value ? "true" : "false";
                app.dirty_edit_buffers[key] = true;
                app.active_edit_key = key;
            }
        } else if (input_field_value("##value", app.edit_buffers[key])) {
            app.dirty_edit_buffers[key] = true;
            app.active_edit_key = key;
        }
        if (ImGui::IsItemActive()) {
            app.active_edit_key = key;
        }

        ImGui::TableSetColumnIndex(3);
        if (app.dirty_edit_buffers[key]) {
            if (ImGui::SmallButton("Save")) {
                save_field(app, entity_id, component, field, key);
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset")) {
                app.edit_buffers[key] = field.value;
                app.dirty_edit_buffers[key] = false;
                if (app.active_edit_key == key) {
                    app.active_edit_key.clear();
                }
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopID();
}

void draw_job_flags(const JobSummary& job) {
    ImGui::TextDisabled(
        "order %d  matching entities %d  max threads %d  min/thread %d",
        job.order,
        job.matching_entity_count,
        job.max_threads,
        job.min_entities_per_thread);
    if (job.structural) {
        ImGui::TextColored(ImVec4(0.95f, 0.74f, 0.35f, 1.0f), "Structural");
        ImGui::SameLine();
    }
    if (job.single_thread) {
        ImGui::TextColored(ImVec4(0.65f, 0.82f, 1.0f, 1.0f), "Single Thread");
    }
}

}  // namespace

void draw_component_card(DebuggerAppState& app, const std::string& entity_id, const ComponentInstance& component) {
    ImGui::PushID(component.component.c_str());
    const std::string title = (!component.name.empty() ? component.name : component.component) +
        (component.singleton ? " [singleton]" : "") + (component.tag ? " [tag]" : "");

    bool open = false;
    const ImGuiTreeNodeFlags flags =
        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_NoTreePushOnOpen |
        ImGuiTreeNodeFlags_SpanAvailWidth;
    if (component.singleton) {
        open = ImGui::TreeNodeEx("component", flags, "%s", title.c_str());
    } else if (ImGui::BeginTable("component_header", 2, ImGuiTableFlags_SizingStretchProp)) {
        const float remove_button_width =
            ImGui::CalcTextSize("x").x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::TableSetupColumn("component_name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("remove", ImGuiTableColumnFlags_WidthFixed, remove_button_width);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        open = ImGui::TreeNodeEx("component", flags, "%s", title.c_str());

        ImGui::TableSetColumnIndex(1);
        if (ImGui::SmallButton("x##remove_component")) {
            remove_component_from_entity(app, entity_id, component.component);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove component");
        }
        ImGui::EndTable();
    }

    if (open) {
        ImGui::Indent();
        if (!component.debug_value.empty()) {
            ImGui::TextWrapped("%s", component.debug_value.c_str());
        }
        if (component.dirty) {
            ImGui::TextColored(ImVec4(1.0f, 0.60f, 0.30f, 1.0f), "Dirty");
        }

        for (const ComponentFieldValue& field : component.fields) {
            draw_component_field(app, entity_id, component, field);
        }

        ImGui::Unindent();
    }
    ImGui::PopID();
}

void draw_job_detail(DebuggerAppState& app, const JobSummary& job) {
    const JobDetail* detail = nullptr;
    if (app.data.job_detail && app.data.job_detail->id == job.id) {
        detail = &*app.data.job_detail;
    }

    draw_detail_title(job_title(job));
    draw_job_flags(job);
    ImGui::Spacing();
    draw_component_refs(app, "Reads", job.reads, false);
    draw_component_refs(app, "Writes", job.writes, false);
    draw_component_refs(app, "Accesses", job.accesses, false);
    draw_component_refs(app, "Without", job.without, true);

    if (detail == nullptr) {
        ImGui::TextDisabled(app.loading ? "Loading job detail..." : "Job detail not loaded.");
        return;
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    draw_section_header("Matching Entities");
    for (const EntitySummary& entity : detail->matching_entities) {
        const std::string label = std::string("  ") + compact_entity_label(entity) + "##job-entity-" + entity.id;
        if (ImGui::Selectable(label.c_str(), entity.id == app.data.selected_entity_id)) {
            app.data.selected_entity_id = entity.id;
            app.data.singleton_tab = false;
            app.data.entity_detail.reset();
            start_refresh(app);
        }
    }
}

}  // namespace ashiato::debugger
