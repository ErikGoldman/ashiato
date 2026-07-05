#include "debugger_core.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstring>
#include <future>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ashiato::debugger {
namespace {

using Json = nlohmann::json;

constexpr int probe_count = 16;
constexpr std::chrono::milliseconds discovery_timeout(700);

const char* entities_query =
    "query Entities { entities { id index version kind displayName } }";
const char* singletons_query =
    "query Singletons { singletons { entity { id index version kind displayName } component { component name tag "
    "singleton dirty debugValue fields { name type value } } } }";
const char* jobs_query =
    "query Jobs { jobs { id name order structural singleThread maxThreads minEntitiesPerThread matchingEntityCount "
    "reads { component name singleton } writes { component name singleton } accesses { component name singleton } "
    "without { component name singleton } } }";
const char* registered_tags_query = "query RegisteredTags { registeredTags { component name } }";
const char* server_name_query = "query ServerName { serverName }";
const char* entity_query =
    "query EntityDetail($id: ID!) { entity(id: $id) { id index version kind displayName components { component name "
    "tag singleton dirty debugValue fields { name type value } } matchingJobs { id name order structural singleThread "
    "maxThreads minEntitiesPerThread matchingEntityCount reads { component name singleton } writes { component name "
    "singleton } accesses { component name singleton } without { component name singleton } } } }";
const char* job_query =
    "query JobDetail($id: ID!) { job(id: $id) { id name order structural singleThread maxThreads minEntitiesPerThread "
    "matchingEntityCount reads { component name singleton } writes { component name singleton } accesses { component "
    "name singleton } without { component name singleton } matchingEntities { id index version kind displayName } } }";
const char* remove_component_mutation =
    "mutation RemoveComponent($entity: ID!, $component: ID!) { removeComponent(entity: $entity, component: $component) "
    "{ ok } }";
const char* set_component_mutation =
    "mutation SetComponent($entity: ID!, $component: ID!, $value: JSON) { setComponent(entity: $entity, component: "
    "$component, value: $value) { component name tag singleton dirty debugValue fields { name type value } } }";

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalid_socket = INVALID_SOCKET;
void close_socket(Socket socket) {
    closesocket(socket);
}
int socket_error() {
    return WSAGetLastError();
}
bool would_block(int error) {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINTR;
}
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        started_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketRuntime() {
        if (started_) {
            WSACleanup();
        }
    }

private:
    bool started_ = false;
};
#else
using Socket = int;
constexpr Socket invalid_socket = -1;
void close_socket(Socket socket) {
    close(socket);
}
int socket_error() {
    return errno;
}
bool would_block(int error) {
    return error == EAGAIN || error == EWOULDBLOCK || error == EINPROGRESS || error == EINTR;
}
class SocketRuntime {
public:
    SocketRuntime() = default;
};
#endif

SocketRuntime& socket_runtime() {
    static SocketRuntime runtime;
    return runtime;
}

bool set_nonblocking(Socket socket) {
#ifdef _WIN32
    u_long mode = 1;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

bool wait_socket(Socket socket, bool write, std::chrono::milliseconds timeout) {
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    FD_SET(socket, write ? &write_set : &read_set);
    timeval tv{};
    tv.tv_sec = static_cast<long>(timeout.count() / 1000);
    tv.tv_usec = static_cast<long>((timeout.count() % 1000) * 1000);
    const int selected = select(
        static_cast<int>(socket) + 1,
        write ? nullptr : &read_set,
        write ? &write_set : nullptr,
        nullptr,
        &tv);
    return selected > 0;
}

bool send_all(Socket socket, const std::string& bytes, std::chrono::milliseconds timeout, std::string& error) {
    const char* cursor = bytes.data();
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        if (!wait_socket(socket, true, timeout)) {
            error = "send timeout";
            return false;
        }
#ifdef _WIN32
        const int sent = send(socket, cursor, static_cast<int>(remaining), 0);
#else
        const ssize_t sent = send(socket, cursor, remaining, 0);
#endif
        if (sent > 0) {
            cursor += sent;
            remaining -= static_cast<std::size_t>(sent);
            continue;
        }
        const int err = socket_error();
        if (would_block(err)) {
            continue;
        }
        error = "send failed";
        return false;
    }
    return true;
}

std::optional<std::size_t> content_length(const std::string& response) {
    const std::string key = "Content-Length:";
    std::size_t found = response.find(key);
    if (found == std::string::npos) {
        found = response.find("content-length:");
    }
    if (found == std::string::npos) {
        return std::nullopt;
    }
    found += key.size();
    while (found < response.size() && std::isspace(static_cast<unsigned char>(response[found])) != 0) {
        ++found;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(response.c_str() + found, &end, 10);
    if (end == response.c_str() + found) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(parsed);
}

bool response_complete(const std::string& response) {
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }
    const std::optional<std::size_t> length = content_length(response);
    return !length || response.size() >= header_end + 4 + *length;
}

std::string response_body(const std::string& response) {
    const std::size_t header_end = response.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return {};
    }
    const std::optional<std::size_t> length = content_length(response);
    if (!length) {
        return response.substr(header_end + 4);
    }
    return response.substr(header_end + 4, *length);
}

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool contains_case_insensitive(const std::string& value, const std::string& query) {
    return lower_copy(value).find(lower_copy(query)) != std::string::npos;
}

bool has_id(const std::vector<EntitySummary>& entities, const std::string& id) {
    return std::any_of(entities.begin(), entities.end(), [&](const EntitySummary& entity) { return entity.id == id; });
}

bool has_singleton(const std::vector<SingletonInstance>& singletons, const std::string& component) {
    return std::any_of(singletons.begin(), singletons.end(), [&](const SingletonInstance& singleton) {
        return singleton.component.component == component;
    });
}

bool has_job(const std::vector<JobSummary>& jobs, const std::string& id) {
    return std::any_of(jobs.begin(), jobs.end(), [&](const JobSummary& job) { return job.id == id; });
}

const Json& data_field(const Json& root, const char* field) {
    if (root.contains("errors") && root["errors"].is_array() && !root["errors"].empty()) {
        throw std::runtime_error(root["errors"][0].value("message", "GraphQL error"));
    }
    if (!root.contains("data")) {
        throw std::runtime_error("GraphQL response did not include data");
    }
    return root["data"].at(field);
}

EntitySummary parse_entity(const Json& json) {
    EntitySummary entity;
    entity.id = json.value("id", "");
    entity.index = json.value("index", 0);
    entity.version = json.value("version", 0);
    entity.kind = json.value("kind", "");
    entity.display_name = json.value("displayName", "");
    return entity;
}

ComponentFieldValue parse_field(const Json& json) {
    ComponentFieldValue field;
    field.name = json.value("name", "");
    field.type = json.value("type", "");
    if (json.contains("value") && json["value"].is_boolean()) {
        field.is_bool = true;
        field.bool_value = json["value"].get<bool>();
        field.value = field.bool_value ? "true" : "false";
    } else if (json.contains("value") && json["value"].is_number()) {
        field.value = json["value"].dump();
    } else {
        field.value = json.value("value", "");
        field.is_bool = field.type == "bool";
        field.bool_value = field.value == "true";
    }
    return field;
}

ComponentInstance parse_component(const Json& json) {
    ComponentInstance component;
    component.component = json.value("component", "");
    component.name = json.value("name", "");
    component.tag = json.value("tag", false);
    component.singleton = json.value("singleton", false);
    component.dirty = json.value("dirty", false);
    component.debug_value = json.value("debugValue", "");
    if (json.contains("fields") && json["fields"].is_array()) {
        for (const Json& field : json["fields"]) {
            component.fields.push_back(parse_field(field));
        }
    }
    return component;
}

ComponentRef parse_component_ref(const Json& json) {
    ComponentRef component;
    component.component = json.value("component", "");
    component.name = json.value("name", "");
    component.singleton = json.value("singleton", false);
    return component;
}

std::vector<ComponentRef> parse_component_refs(const Json& json) {
    std::vector<ComponentRef> refs;
    if (!json.is_array()) {
        return refs;
    }
    refs.reserve(json.size());
    for (const Json& ref : json) {
        refs.push_back(parse_component_ref(ref));
    }
    return refs;
}

JobSummary parse_job(const Json& json) {
    JobSummary job;
    job.id = json.value("id", "");
    job.name = json.value("name", "");
    job.order = json.value("order", 0);
    job.structural = json.value("structural", false);
    job.single_thread = json.value("singleThread", false);
    job.max_threads = json.value("maxThreads", 0);
    job.min_entities_per_thread = json.value("minEntitiesPerThread", 0);
    job.matching_entity_count = json.value("matchingEntityCount", 0);
    job.reads = parse_component_refs(json.value("reads", Json::array()));
    job.writes = parse_component_refs(json.value("writes", Json::array()));
    job.accesses = parse_component_refs(json.value("accesses", Json::array()));
    job.without = parse_component_refs(json.value("without", Json::array()));
    return job;
}

std::vector<EntitySummary> parse_entities(const Json& json) {
    std::vector<EntitySummary> entities;
    if (!json.is_array()) {
        return entities;
    }
    entities.reserve(json.size());
    for (const Json& entity : json) {
        entities.push_back(parse_entity(entity));
    }
    return entities;
}

std::vector<JobSummary> parse_jobs(const Json& json) {
    std::vector<JobSummary> jobs;
    if (!json.is_array()) {
        return jobs;
    }
    jobs.reserve(json.size());
    for (const Json& job : json) {
        jobs.push_back(parse_job(job));
    }
    return jobs;
}

std::vector<SingletonInstance> parse_singletons(const Json& json) {
    std::vector<SingletonInstance> singletons;
    if (!json.is_array()) {
        return singletons;
    }
    singletons.reserve(json.size());
    for (const Json& singleton_json : json) {
        SingletonInstance singleton;
        singleton.entity = parse_entity(singleton_json.at("entity"));
        singleton.component = parse_component(singleton_json.at("component"));
        singletons.push_back(std::move(singleton));
    }
    return singletons;
}

std::vector<RegisteredTag> parse_registered_tags(const Json& json) {
    std::vector<RegisteredTag> tags;
    if (!json.is_array()) {
        return tags;
    }
    tags.reserve(json.size());
    for (const Json& tag_json : json) {
        RegisteredTag tag;
        tag.component = tag_json.value("component", "");
        tag.name = tag_json.value("name", "");
        tags.push_back(std::move(tag));
    }
    return tags;
}

Json parse_body(const RequestResult& result) {
    if (!result.ok) {
        throw std::runtime_error(result.error);
    }
    return Json::parse(result.body);
}

Json make_payload(
    const std::string& query,
    const std::map<std::string, std::string>& string_variables,
    const std::map<std::string, std::map<std::string, std::string>>& object_variables) {
    Json variables = Json::object();
    for (const auto& variable : string_variables) {
        variables[variable.first] = variable.second;
    }
    for (const auto& variable : object_variables) {
        Json object = Json::object();
        for (const auto& entry : variable.second) {
            object[entry.first] = entry.second;
        }
        variables[variable.first] = std::move(object);
    }
    Json payload;
    payload["query"] = query;
    payload["variables"] = std::move(variables);
    return payload;
}

Json parse_field_value(const ComponentFieldValue& field, const std::string& value) {
    if (field.type == "bool") {
        return value == "true" || value == "1";
    }
    if (field.type == "string") {
        return value;
    }
    char* end = nullptr;
    if (field.type == "f32" || field.type == "f64") {
        const double parsed = std::strtod(value.c_str(), &end);
        if (end == value.c_str()) {
            throw std::runtime_error(field.name + " must be a number");
        }
        return parsed;
    }
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str()) {
        throw std::runtime_error(field.name + " must be an integer");
    }
    return parsed;
}

}  // namespace

GraphQLClient::GraphQLClient(int port) : port_(port) {}

std::string GraphQLClient::url() const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/graphql";
}

RequestResult GraphQLClient::request(
    const std::string& query,
    const std::map<std::string, std::string>& string_variables,
    const std::map<std::string, std::map<std::string, std::string>>& object_variables,
    std::chrono::milliseconds timeout) const {
    return post_payload(make_payload(query, string_variables, object_variables).dump(), timeout);
}

RequestResult GraphQLClient::post_payload(const std::string& body, std::chrono::milliseconds timeout) const {
    (void)socket_runtime();
    const auto start = std::chrono::steady_clock::now();
    RequestResult result;
    Socket socket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket == invalid_socket) {
        result.error = "failed to create socket";
        return result;
    }
    if (!set_nonblocking(socket)) {
        result.error = "failed to set socket nonblocking";
        close_socket(socket);
        return result;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<std::uint16_t>(port_));
#ifdef _WIN32
    InetPtonA(AF_INET, "127.0.0.1", &address.sin_addr);
#else
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
#endif

    const int connected = connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    if (connected != 0) {
        const int err = socket_error();
        if (!would_block(err) || !wait_socket(socket, true, timeout)) {
            result.error = "connection failed";
            close_socket(socket);
            return result;
        }
    }

    const std::string request_bytes =
        "POST /graphql HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: text/plain\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    if (!send_all(socket, request_bytes, timeout, result.error)) {
        close_socket(socket);
        return result;
    }

    std::string response;
    std::array<char, 8192> buffer{};
    while (!response_complete(response)) {
        if (!wait_socket(socket, false, timeout)) {
            result.error = "receive timeout";
            close_socket(socket);
            return result;
        }
#ifdef _WIN32
        const int count = recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const ssize_t count = recv(socket, buffer.data(), buffer.size(), 0);
#endif
        if (count > 0) {
            response.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            break;
        }
        const int err = socket_error();
        if (would_block(err)) {
            continue;
        }
        result.error = "receive failed";
        close_socket(socket);
        return result;
    }
    close_socket(socket);

    result.body = response_body(response);
    result.ok = !result.body.empty();
    if (!result.ok) {
        result.error = "empty response";
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    result.latency_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    return result;
}

std::vector<int> candidate_ports(int base_port) {
    std::vector<int> ports;
    ports.reserve(probe_count);
    for (int index = 0; index < probe_count; ++index) {
        ports.push_back(base_port + index);
    }
    return ports;
}

ServerSnapshot fetch_snapshot(const GraphQLClient& client, std::chrono::milliseconds timeout) {
    ServerSnapshot snapshot;
    snapshot.entities =
        parse_entities(data_field(parse_body(client.request(entities_query, {}, {}, timeout)), "entities"));
    try {
        snapshot.name = data_field(parse_body(client.request(server_name_query, {}, {}, timeout)), "serverName")
                            .get<std::string>();
    } catch (const std::exception&) {
        snapshot.name.clear();
    }
    try {
        snapshot.singletons =
            parse_singletons(data_field(parse_body(client.request(singletons_query, {}, {}, timeout)), "singletons"));
    } catch (const std::exception&) {
        snapshot.singletons.clear();
    }
    snapshot.jobs = parse_jobs(data_field(parse_body(client.request(jobs_query, {}, {}, timeout)), "jobs"));
    try {
        snapshot.registered_tags =
            parse_registered_tags(
                data_field(parse_body(client.request(registered_tags_query, {}, {}, timeout)), "registeredTags"));
    } catch (const std::exception&) {
        snapshot.registered_tags.clear();
    }
    return snapshot;
}

DiscoveredServer discover_server(int port) {
    DiscoveredServer server;
    server.port = port;
    GraphQLClient client(port);
    server.url = client.url();
    const auto start = std::chrono::steady_clock::now();
    try {
        server.snapshot = fetch_snapshot(client, discovery_timeout);
        server.ok = true;
    } catch (const std::exception& exception) {
        server.ok = false;
        server.error = exception.what();
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    server.latency_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
    return server;
}

std::vector<DiscoveredServer> discover_servers(int base_port) {
    std::vector<std::future<DiscoveredServer>> tasks;
    const std::vector<int> ports = candidate_ports(base_port);
    tasks.reserve(ports.size());
    for (int port : ports) {
        tasks.push_back(std::async(std::launch::async, [port]() {
            return discover_server(port);
        }));
    }

    std::vector<DiscoveredServer> servers;
    servers.reserve(tasks.size());
    for (std::future<DiscoveredServer>& task : tasks) {
        servers.push_back(task.get());
    }
    return servers;
}

std::optional<EntityDetail> fetch_entity(const GraphQLClient& client, const std::string& entity_id) {
    const Json entity_json = data_field(parse_body(client.request(entity_query, {{"id", entity_id}})), "entity");
    if (entity_json.is_null()) {
        return std::nullopt;
    }
    EntityDetail detail;
    static_cast<EntitySummary&>(detail) = parse_entity(entity_json);
    for (const Json& component : entity_json.value("components", Json::array())) {
        detail.components.push_back(parse_component(component));
    }
    detail.matching_jobs = parse_jobs(entity_json.value("matchingJobs", Json::array()));
    return detail;
}

std::optional<JobDetail> fetch_job(const GraphQLClient& client, const std::string& job_id) {
    const Json job_json = data_field(parse_body(client.request(job_query, {{"id", job_id}})), "job");
    if (job_json.is_null()) {
        return std::nullopt;
    }
    JobDetail detail;
    static_cast<JobSummary&>(detail) = parse_job(job_json);
    detail.matching_entities = parse_entities(job_json.value("matchingEntities", Json::array()));
    return detail;
}

bool remove_component(const GraphQLClient& client, const std::string& entity_id, const std::string& component_id) {
    const Json root = parse_body(client.request(
        remove_component_mutation,
        {{"entity", entity_id}, {"component", component_id}}));
    return data_field(root, "removeComponent").value("ok", false);
}

bool add_tag(const GraphQLClient& client, const std::string& entity_id, const std::string& component_id) {
    const Json root = parse_body(client.request(
        set_component_mutation,
        {{"entity", entity_id}, {"component", component_id}},
        {{"value", {}}}));
    (void)data_field(root, "setComponent");
    return true;
}

bool set_component_field(
    const GraphQLClient& client,
    const std::string& entity_id,
    const std::string& component_id,
    const ComponentFieldValue& field,
    const std::string& value) {
    Json variables;
    variables["entity"] = entity_id;
    variables["component"] = component_id;
    variables["value"] = Json::object({{field.name, parse_field_value(field, value)}});
    const Json payload = Json{{"query", set_component_mutation}, {"variables", variables}};
    const RequestResult result = client.post_payload(payload.dump());
    if (!result.ok) {
        throw std::runtime_error(result.error);
    }
    const Json root = parse_body(result);
    (void)data_field(root, "setComponent");
    return true;
}

void reconcile_selection(DebuggerState& state, std::vector<DiscoveredServer> servers) {
    state.servers = std::move(servers);
    const auto live_begin = std::find_if(state.servers.begin(), state.servers.end(), [](const DiscoveredServer& server) {
        return server.ok;
    });
    const bool selected_alive = std::any_of(state.servers.begin(), state.servers.end(), [&](const DiscoveredServer& server) {
        return server.ok && server.port == state.selected_port;
    });
    if (!selected_alive) {
        state.selected_port = live_begin != state.servers.end() ? live_begin->port : 0;
        state.entity_detail.reset();
        state.job_detail.reset();
    }

    const DiscoveredServer* server = selected_server(state);
    if (server == nullptr) {
        state.selected_entity_id.clear();
        state.selected_singleton_component_id.clear();
        state.selected_job_id.clear();
        state.entity_detail.reset();
        state.job_detail.reset();
        return;
    }

    if (!has_id(server->snapshot.entities, state.selected_entity_id)) {
        state.selected_entity_id =
            !server->snapshot.entities.empty() ? server->snapshot.entities.front().id : std::string{};
        state.entity_detail.reset();
    }
    if (!has_singleton(server->snapshot.singletons, state.selected_singleton_component_id)) {
        state.selected_singleton_component_id =
            !server->snapshot.singletons.empty() ? server->snapshot.singletons.front().component.component : std::string{};
    }
    if (!has_job(server->snapshot.jobs, state.selected_job_id)) {
        state.selected_job_id = !server->snapshot.jobs.empty() ? server->snapshot.jobs.front().id : std::string{};
        state.job_detail.reset();
    }
    if (state.singleton_tab && state.selected_singleton_component_id.empty()) {
        state.singleton_tab = false;
    }
}

const DiscoveredServer* selected_server(const DebuggerState& state) {
    const auto found = std::find_if(state.servers.begin(), state.servers.end(), [&](const DiscoveredServer& server) {
        return server.ok && server.port == state.selected_port;
    });
    return found != state.servers.end() ? &*found : nullptr;
}

std::string entity_label(const EntitySummary& entity) {
    if (!entity.display_name.empty()) {
        return entity.display_name;
    }
    return entity.kind + " #" + std::to_string(entity.index) + " v" + std::to_string(entity.version);
}

std::string job_title(const JobSummary& job) {
    if (!job.name.empty()) {
        return job.name;
    }
    return "Job #" + job.id + " order " + std::to_string(job.order);
}

std::string component_ref_name(const ComponentRef& component) {
    return !component.name.empty() ? component.name : component.component;
}

bool matches_entity(const EntitySummary& entity, const std::string& query) {
    if (query.empty()) {
        return true;
    }
    return contains_case_insensitive(entity.id, query) ||
        contains_case_insensitive(entity.display_name, query) ||
        contains_case_insensitive(entity.kind, query) ||
        contains_case_insensitive(std::to_string(entity.index), query) ||
        contains_case_insensitive(std::to_string(entity.version), query);
}

bool matches_singleton(const SingletonInstance& singleton, const std::string& query) {
    if (query.empty()) {
        return true;
    }
    return contains_case_insensitive(singleton.entity.id, query) ||
        contains_case_insensitive(singleton.component.component, query) ||
        contains_case_insensitive(singleton.component.name, query) ||
        contains_case_insensitive(singleton.component.debug_value, query) ||
        contains_case_insensitive(std::to_string(singleton.entity.index), query) ||
        contains_case_insensitive(std::to_string(singleton.entity.version), query);
}

bool matches_job(const JobSummary& job, const std::string& query) {
    if (query.empty()) {
        return true;
    }
    if (contains_case_insensitive(job.id, query) ||
        contains_case_insensitive(job.name, query) ||
        contains_case_insensitive(std::to_string(job.order), query)) {
        return true;
    }
    const auto matches_refs = [&](const std::vector<ComponentRef>& refs) {
        return std::any_of(refs.begin(), refs.end(), [&](const ComponentRef& ref) {
            return contains_case_insensitive(ref.component, query) || contains_case_insensitive(ref.name, query);
        });
    };
    return matches_refs(job.reads) || matches_refs(job.writes) || matches_refs(job.accesses) || matches_refs(job.without);
}

std::string field_edit_key(const std::string& entity_id, const std::string& component_id, const std::string& field_name) {
    return entity_id + "/" + component_id + "/" + field_name;
}

}  // namespace ashiato::debugger
