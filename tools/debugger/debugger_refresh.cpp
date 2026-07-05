#include "debugger_app_internal.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace ashiato::debugger {
namespace {

RefreshResult refresh_data(const DebuggerState& previous, int base_port) {
    RefreshResult result;
    try {
        DebuggerState next = previous;
        reconcile_selection(next, discover_servers(base_port));
        const DiscoveredServer* server = selected_server(next);
        if (server != nullptr) {
            GraphQLClient client(server->port);
            if (!next.singleton_tab && !next.selected_entity_id.empty()) {
                result.entity_detail_id = next.selected_entity_id;
                result.entity_detail = fetch_entity(client, next.selected_entity_id);
            }
            if (!next.selected_job_id.empty()) {
                result.job_detail_id = next.selected_job_id;
                result.job_detail = fetch_job(client, next.selected_job_id);
            }
        }
        result.servers = std::move(next.servers);
    } catch (const std::exception& exception) {
        result.error = exception.what();
    }
    return result;
}

}  // namespace

void start_refresh(DebuggerAppState& app) {
    if (app.loading) {
        return;
    }
    app.loading = true;
    app.status_error.clear();
    const DebuggerState snapshot = app.data;
    const int base_port = app.base_port;
    app.refresh_task = std::async(std::launch::async, [snapshot, base_port]() {
        return refresh_data(snapshot, base_port);
    });
}

void poll_refresh(DebuggerAppState& app) {
    if (!app.loading || !app.refresh_task.valid()) {
        return;
    }
    if (app.refresh_task.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        return;
    }

    RefreshResult result = app.refresh_task.get();
    app.loading = false;
    if (!result.error.empty()) {
        app.status_error = result.error;
        return;
    }

    reconcile_selection(app.data, std::move(result.servers));
    const bool needs_entity_followup =
        !app.data.singleton_tab && !app.data.selected_entity_id.empty() &&
        result.entity_detail_id != app.data.selected_entity_id;
    const bool needs_job_followup =
        !app.data.selected_job_id.empty() && result.job_detail_id != app.data.selected_job_id;

    if (!needs_entity_followup) {
        app.data.entity_detail = std::move(result.entity_detail);
    }
    if (!needs_job_followup) {
        app.data.job_detail = std::move(result.job_detail);
    }

    app.restore_scroll_after_refresh = true;
    if (needs_entity_followup || needs_job_followup) {
        start_refresh(app);
    }
}

}  // namespace ashiato::debugger
