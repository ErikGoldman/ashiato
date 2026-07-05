#include "debugger_core.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace ashiato::debugger;

namespace {

DiscoveredServer make_server() {
    DiscoveredServer server;
    server.port = 8080;
    server.ok = true;
    server.url = "http://127.0.0.1:8080/graphql";
    server.snapshot.name = "server";
    server.snapshot.entities = {
        EntitySummary{"10", 10, 1, "User", "hero"},
        EntitySummary{"11", 11, 2, "System", ""},
    };
    ComponentInstance singleton_component;
    singleton_component.component = "30";
    singleton_component.name = "GameTime";
    server.snapshot.singletons = {
        SingletonInstance{EntitySummary{"12", 12, 1, "System", ""}, singleton_component},
    };
    JobSummary job;
    job.id = "20";
    job.name = "Move";
    job.order = 3;
    job.matching_entity_count = 2;
    server.snapshot.jobs = {job};
    return server;
}

}  // namespace

TEST_CASE("debugger candidate ports scan the base port and next fifteen ports") {
    const std::vector<int> ports = candidate_ports(9000);

    REQUIRE(ports.size() == 16);
    REQUIRE(ports.front() == 9000);
    REQUIRE(ports.back() == 9015);
}

TEST_CASE("debugger labels prefer display names and fallback to stable summaries") {
    REQUIRE(entity_label(EntitySummary{"1", 1, 2, "User", "ball"}) == "ball");
    REQUIRE(entity_label(EntitySummary{"2", 7, 3, "System", ""}) == "System #7 v3");

    JobSummary named;
    named.id = "10";
    named.name = "Move";
    named.order = 5;
    REQUIRE(job_title(named) == "Move");

    named.name.clear();
    REQUIRE(job_title(named) == "Job #10 order 5");
}

TEST_CASE("debugger filters match important entity singleton and job fields") {
    const DiscoveredServer server = make_server();
    REQUIRE(matches_entity(server.snapshot.entities[0], "hero"));
    REQUIRE(matches_entity(server.snapshot.entities[1], "system"));
    REQUIRE(matches_singleton(server.snapshot.singletons[0], "gametime"));
    REQUIRE(matches_job(server.snapshot.jobs[0], "move"));
    REQUIRE_FALSE(matches_job(server.snapshot.jobs[0], "missing"));
}

TEST_CASE("debugger reconcile preserves valid selections and clears stale details") {
    DebuggerState state;
    state.selected_port = 8080;
    state.singleton_tab = true;
    state.selected_entity_id = "11";
    state.selected_singleton_component_id = "30";
    state.selected_job_id = "20";
    state.entity_detail = EntityDetail{};
    state.job_detail = JobDetail{};

    reconcile_selection(state, {make_server()});

    REQUIRE(state.selected_port == 8080);
    REQUIRE(state.singleton_tab);
    REQUIRE(state.selected_entity_id == "11");
    REQUIRE(state.selected_singleton_component_id == "30");
    REQUIRE(state.selected_job_id == "20");
    REQUIRE(state.entity_detail.has_value());
    REQUIRE(state.job_detail.has_value());

    DiscoveredServer changed = make_server();
    changed.snapshot.entities.erase(changed.snapshot.entities.begin() + 1);
    changed.snapshot.jobs.clear();
    reconcile_selection(state, {changed});

    REQUIRE(state.selected_entity_id == "10");
    REQUIRE(state.selected_job_id.empty());
    REQUIRE_FALSE(state.entity_detail.has_value());
    REQUIRE_FALSE(state.job_detail.has_value());
}
