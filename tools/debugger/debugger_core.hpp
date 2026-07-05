#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ashiato::debugger {

struct EntitySummary {
    std::string id;
    int index = 0;
    int version = 0;
    std::string kind;
    std::string display_name;
};

struct ComponentFieldValue {
    std::string name;
    std::string type;
    std::string value;
    bool bool_value = false;
    bool is_bool = false;
};

struct ComponentInstance {
    std::string component;
    std::string name;
    bool tag = false;
    bool singleton = false;
    bool dirty = false;
    std::string debug_value;
    std::vector<ComponentFieldValue> fields;
};

struct ComponentRef {
    std::string component;
    std::string name;
    bool singleton = false;
};

struct JobSummary {
    std::string id;
    std::string name;
    int order = 0;
    bool structural = false;
    bool single_thread = false;
    int max_threads = 0;
    int min_entities_per_thread = 0;
    int matching_entity_count = 0;
    std::vector<ComponentRef> reads;
    std::vector<ComponentRef> writes;
    std::vector<ComponentRef> accesses;
    std::vector<ComponentRef> without;
};

struct RegisteredTag {
    std::string component;
    std::string name;
};

struct SingletonInstance {
    EntitySummary entity;
    ComponentInstance component;
};

struct EntityDetail : EntitySummary {
    std::vector<ComponentInstance> components;
    std::vector<JobSummary> matching_jobs;
};

struct JobDetail : JobSummary {
    std::vector<EntitySummary> matching_entities;
};

struct ServerSnapshot {
    std::string name;
    std::vector<EntitySummary> entities;
    std::vector<SingletonInstance> singletons;
    std::vector<JobSummary> jobs;
    std::vector<RegisteredTag> registered_tags;
};

struct DiscoveredServer {
    int port = 0;
    std::string url;
    bool ok = false;
    int latency_ms = 0;
    std::string error;
    ServerSnapshot snapshot;
};

struct DebuggerState {
    std::vector<DiscoveredServer> servers;
    int selected_port = 0;
    bool singleton_tab = false;
    std::string selected_entity_id;
    std::string selected_singleton_component_id;
    std::string selected_job_id;
    std::optional<EntityDetail> entity_detail;
    std::optional<JobDetail> job_detail;
};

struct RequestResult {
    bool ok = false;
    std::string body;
    std::string error;
    int latency_ms = 0;
};

class GraphQLClient {
public:
    explicit GraphQLClient(int port);

    RequestResult request(
        const std::string& query,
        const std::map<std::string, std::string>& string_variables = {},
        const std::map<std::string, std::map<std::string, std::string>>& object_variables = {},
        std::chrono::milliseconds timeout = std::chrono::milliseconds(700)) const;
    RequestResult post_payload(
        const std::string& payload,
        std::chrono::milliseconds timeout = std::chrono::milliseconds(700)) const;

    int port() const noexcept { return port_; }
    std::string url() const;

private:
    int port_ = 0;
};

std::vector<int> candidate_ports(int base_port);
std::vector<DiscoveredServer> discover_servers(int base_port);
ServerSnapshot fetch_snapshot(
    const GraphQLClient& client,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(700));
std::optional<EntityDetail> fetch_entity(const GraphQLClient& client, const std::string& entity_id);
std::optional<JobDetail> fetch_job(const GraphQLClient& client, const std::string& job_id);
bool remove_component(const GraphQLClient& client, const std::string& entity_id, const std::string& component_id);
bool add_tag(const GraphQLClient& client, const std::string& entity_id, const std::string& component_id);
bool set_component_field(
    const GraphQLClient& client,
    const std::string& entity_id,
    const std::string& component_id,
    const ComponentFieldValue& field,
    const std::string& value);

void reconcile_selection(DebuggerState& state, std::vector<DiscoveredServer> servers);
const DiscoveredServer* selected_server(const DebuggerState& state);
std::string entity_label(const EntitySummary& entity);
std::string job_title(const JobSummary& job);
std::string component_ref_name(const ComponentRef& component);
bool matches_entity(const EntitySummary& entity, const std::string& query);
bool matches_singleton(const SingletonInstance& singleton, const std::string& query);
bool matches_job(const JobSummary& job, const std::string& query);
std::string field_edit_key(const std::string& entity_id, const std::string& component_id, const std::string& field_name);

}  // namespace ashiato::debugger
