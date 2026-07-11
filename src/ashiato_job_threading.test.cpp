#include "ashiato_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>

namespace {

template <typename Context, typename = void>
struct HasCurrentStructuralAdd : std::false_type {};

template <typename Context>
struct HasCurrentStructuralAdd<
    Context,
    std::void_t<decltype(std::declval<Context&>().template add<Disabled>())>> : std::true_type {};

template <typename Context, typename = void>
struct HasTargetedStructuralAdd : std::false_type {};

template <typename Context>
struct HasTargetedStructuralAdd<
    Context,
    std::void_t<decltype(std::declval<Context&>().template add<Disabled>(std::declval<ashiato::Entity>()))>>
    : std::true_type {};

}  // namespace

TEST_CASE("run jobs batches independent jobs through the executor") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{2.0f, 0.0f}) != nullptr);

    registry.job<Position>(0).each([](ashiato::Entity, Position&) {});
    registry.job<Velocity>(1).each([](ashiato::Entity, Velocity&) {});

    std::vector<std::size_t> batch_sizes;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        batch_sizes.push_back(tasks.size());
        for (const ashiato::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(batch_sizes == std::vector<std::size_t>{2});
}

TEST_CASE("job executor must run every task before returning") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    registry.job<Position>(0).each([](ashiato::Entity, Position&) {});

    std::vector<ashiato::JobThreadTask> saved_tasks;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        saved_tasks = tasks;
    });

    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
    REQUIRE(saved_tasks.size() == 1);
    REQUIRE_THROWS_AS(saved_tasks.front().run(), std::logic_error);
}

TEST_CASE("job executor cannot run a task more than once") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int calls = 0;
    registry.job<Position>(0).each([&](ashiato::Entity, Position&) {
        ++calls;
    });

    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>& tasks) {
        REQUIRE(tasks.size() == 1);
        tasks.front().run();
        tasks.front().run();
    });

    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
    REQUIRE(calls == 1);
}

TEST_CASE("threaded jobs split entity ranges using max threads and minimum entity counts") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 5; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    registry.job<Position>(0).max_threads(3).min_entities_per_thread(2).each([](ashiato::Entity, Position& position) {
        position.y = position.x + 10;
    });

    std::vector<std::size_t> thread_indices;
    std::vector<std::size_t> thread_counts;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        for (const ashiato::JobThreadTask& task : tasks) {
            thread_indices.push_back(task.thread_index);
            thread_counts.push_back(task.thread_count);
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(thread_indices == std::vector<std::size_t>{0, 1, 2});
    REQUIRE(thread_counts == std::vector<std::size_t>{3, 3, 3});

    int visited = 0;
    registry.view<const Position>().each([&](ashiato::Entity, const Position& position) {
        REQUIRE(position.y == position.x + 10);
        ++visited;
    });
    REQUIRE(visited == 5);
}

TEST_CASE("threaded jobs defer dirty marking until split ranges complete") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    std::vector<ashiato::Entity> entities;
    for (int i = 0; i < 128; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
        entities.push_back(entity);
    }
    registry.clear_all_dirty<Position>();

    registry.job<Position>(0).max_threads(4).min_entities_per_thread(1).each(
        [](ashiato::Entity, Position& position) {
            position.y = position.x + 1000;
        });

    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>& tasks) {
        std::vector<std::thread> threads;
        threads.reserve(tasks.size());
        for (const ashiato::JobThreadTask& task : tasks) {
            const ashiato::JobThreadTask* task_ptr = &task;
            threads.emplace_back([task_ptr]() {
                task_ptr->run();
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    });

    registry.run_jobs();

    for (ashiato::Entity entity : entities) {
        const Position& position = registry.get<Position>(entity);
        REQUIRE(position.y == position.x + 1000);
        REQUIRE(registry.is_dirty<Position>(entity));
    }
}

TEST_CASE("threaded optional writes use deferred dirty logs without dirtying readonly optionals") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Health>("Health");

    std::vector<ashiato::Entity> with_health;
    std::vector<ashiato::Entity> without_health;
    for (int i = 0; i < 16; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
        if ((i % 2) == 0) {
            REQUIRE(registry.add<Health>(entity, Health{i}) != nullptr);
            with_health.push_back(entity);
        } else {
            without_health.push_back(entity);
        }
    }

    registry.clear_all_dirty<Position>();
    registry.clear_all_dirty<Health>();
    registry.job<Position>(0).optional<Health>().max_threads(4).min_entities_per_thread(1).each(
        [](auto& view, ashiato::Entity, Position& position) {
            position.y = position.x + 1;
            if (view.template contains<Health>()) {
                view.template write<Health>().value += 10;
            }
        });

    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>& tasks) {
        std::vector<std::thread> threads;
        threads.reserve(tasks.size());
        for (const ashiato::JobThreadTask& task : tasks) {
            const ashiato::JobThreadTask* task_ptr = &task;
            threads.emplace_back([task_ptr]() {
                task_ptr->run();
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    });

    registry.run_jobs();

    for (ashiato::Entity entity : with_health) {
        REQUIRE(registry.is_dirty<Position>(entity));
        REQUIRE(registry.is_dirty<Health>(entity));
        REQUIRE(registry.get<Health>(entity).value >= 10);
    }
    for (ashiato::Entity entity : without_health) {
        REQUIRE(registry.is_dirty<Position>(entity));
        REQUIRE_FALSE(registry.contains<Health>(entity));
        REQUIRE_FALSE(registry.is_dirty<Health>(entity));
    }
}

TEST_CASE("mutable singleton jobs stay single threaded even when max threads is requested") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<GameTime>("GameTime");

    for (int i = 0; i < 8; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }
    registry.clear_all_dirty<GameTime>();

    registry.job<Position, GameTime>(0).max_threads(4).min_entities_per_thread(1).each(
        [](ashiato::Entity, Position&, GameTime& time) {
            ++time.tick;
        });

    std::vector<std::size_t> thread_counts;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        for (const ashiato::JobThreadTask& task : tasks) {
            thread_counts.push_back(task.thread_count);
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(thread_counts == std::vector<std::size_t>{1});
    REQUIRE(registry.get<GameTime>().tick == 8);
    REQUIRE(registry.is_dirty<GameTime>());
}

TEST_CASE("force single threaded run ignores executor chunking") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 4; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    int calls = 0;
    registry.job<Position>(0).max_threads(4).min_entities_per_thread(1).each([&](ashiato::Entity, Position&) {
        ++calls;
    });

    int executor_calls = 0;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        ++executor_calls;
        for (const ashiato::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    registry.run_jobs(ashiato::RunJobsOptions{true});

    REQUIRE(calls == 4);
    REQUIRE(executor_calls == 0);
}

TEST_CASE("threaded job executor must run every task before returning") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 4; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    registry.job<Position>(0)
        .max_threads(4)
        .min_entities_per_thread(1)
        .each([](ashiato::Entity, Position& position) {
            position.x += 1;
        });
    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>&) {});

    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
}

TEST_CASE("threaded job executor cannot run a task after returning") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    for (int i = 0; i < 2; ++i) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{i, 0}) != nullptr);
    }

    std::vector<ashiato::JobThreadTask> captured;
    registry.job<Position>(0)
        .max_threads(2)
        .min_entities_per_thread(1)
        .each([](ashiato::Entity, Position&) {});
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        captured = tasks;
    });

    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
    REQUIRE_FALSE(captured.empty());
    REQUIRE_THROWS_AS(captured.front().run(), std::logic_error);
}

TEST_CASE("threaded job exceptions are rethrown after task completion") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    registry.job<Position>(0)
        .max_threads(2)
        .min_entities_per_thread(1)
        .each([](ashiato::Entity, Position&) {
            throw std::runtime_error("job failed");
        });
    registry.set_job_thread_executor([](const std::vector<ashiato::JobThreadTask>& tasks) {
        for (const ashiato::JobThreadTask& task : tasks) {
            task.run();
        }
    });

    REQUIRE_THROWS_AS(registry.run_jobs(), std::runtime_error);
}

TEST_CASE("structural jobs expose declared add and remove operations and stay single threaded") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    registry.job<const Position>(0).max_threads(4).min_entities_per_thread(1).structural<Disabled>().each(
        [](auto& view, ashiato::Entity, const Position&) {
            REQUIRE(view.template add<Disabled>());
        });

    std::vector<std::size_t> batch_sizes;
    registry.set_job_thread_executor([&](const std::vector<ashiato::JobThreadTask>& tasks) {
        batch_sizes.push_back(tasks.size());
        for (const ashiato::JobThreadTask& task : tasks) {
            REQUIRE(task.thread_count == 1);
            task.run();
        }
    });

    registry.run_jobs();

    REQUIRE(batch_sizes == std::vector<std::size_t>{1});
    REQUIRE(registry.has<Disabled>(entity));

    registry.job<const Position>(1).structural<Disabled>().each([](auto& view, ashiato::Entity, const Position&) {
        REQUIRE(view.template remove<Disabled>());
    });

    registry.run_jobs();

    REQUIRE_FALSE(registry.has<Disabled>(entity));
}

TEST_CASE("structural contexts only expose current-entity structural operations") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    const ashiato::Entity job = registry.job<const Position>(0).structural<Disabled>().each(
        [](auto& context, ashiato::Entity, const Position&) {
            using Context = typename std::remove_reference<decltype(context)>::type;
            STATIC_REQUIRE(HasCurrentStructuralAdd<Context>::value);
            STATIC_REQUIRE_FALSE(HasTargetedStructuralAdd<Context>::value);
            REQUIRE(context.template add<Disabled>());
        });

    REQUIRE(registry.job_info(job)->structural);
    REQUIRE_FALSE(registry.job_info(job)->structural_any);

    registry.run_jobs();
    REQUIRE(registry.has<Disabled>(entity));
}

TEST_CASE("structural_any contexts expose targeted rather than implicit structural operations") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    const ashiato::Entity job = registry.job<const Position>(0).structural_any<Disabled>().each(
        [](auto& context, ashiato::Entity current, const Position&) {
            using Context = typename std::remove_reference<decltype(context)>::type;
            STATIC_REQUIRE_FALSE(HasCurrentStructuralAdd<Context>::value);
            STATIC_REQUIRE(HasTargetedStructuralAdd<Context>::value);
            REQUIRE(context.template add<Disabled>(current));
        });

    REQUIRE(registry.job_info(job)->structural);
    REQUIRE(registry.job_info(job)->structural_any);

    registry.run_jobs();
    REQUIRE(registry.has<Disabled>(entity));
}

#if ASHIATO_RUNTIME_REGISTRY_ACCESS_CHECKING
TEST_CASE("structural runtime checking also constrains mutations made by lifecycle hooks") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");
    registry.register_component<Active>("Active");

    const ashiato::Entity current = registry.create();
    const ashiato::Entity other = registry.create();
    REQUIRE(registry.add<Position>(current, Position{}) != nullptr);

    auto subscription = registry.on_component_add<Disabled>(
        [&](ashiato::Registry& hooked_registry, ashiato::Entity) {
            hooked_registry.add<Active>(other);
        });
    REQUIRE(subscription.active());

    registry.job<const Position>(0).structural<Disabled>().each(
        [](auto& context, ashiato::Entity, const Position&) {
            (void)context.template add<Disabled>();
        });

    REQUIRE_THROWS_AS(registry.run_jobs(), std::logic_error);
    REQUIRE_FALSE(registry.has<Active>(other));
}
#endif

TEST_CASE("all direct job iteration runs in reverse dense order") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    std::vector<ashiato::Entity> entities;
    for (int value = 0; value < 4; ++value) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{value, 0}) != nullptr);
        entities.push_back(entity);
    }

    std::vector<ashiato::Entity> visited;
    registry.job<const Position>(0).each(
        [&](ashiato::Entity entity, const Position&) { visited.push_back(entity); });

    registry.run_jobs();
    REQUIRE(visited == std::vector<ashiato::Entity>{entities[3], entities[2], entities[1], entities[0]});
}

TEST_CASE("structural jobs remove the current entity from owned groups without skips or duplicates") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.declare_owned_group<Position, Velocity>();

    std::vector<ashiato::Entity> entities;
    for (int value = 0; value < 4; ++value) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{value, 0}) != nullptr);
        REQUIRE(registry.add<Velocity>(entity, Velocity{}) != nullptr);
        entities.push_back(entity);
    }

    std::vector<ashiato::Entity> visited;
    registry.job<const Position, const Velocity>(0).structural<Velocity>().each(
        [&](auto& context, ashiato::Entity entity, const Position&, const Velocity&) {
            visited.push_back(entity);
            REQUIRE(context.template remove<Velocity>());
        });

    registry.run_jobs();

    REQUIRE(visited == std::vector<ashiato::Entity>{entities[3], entities[2], entities[1], entities[0]});
    for (ashiato::Entity entity : entities) {
        REQUIRE_FALSE(registry.contains<Velocity>(entity));
    }
}

TEST_CASE("structural jobs defer current-entity owned-group entry until reverse iteration completes") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.declare_owned_group<Position, Velocity>();

    std::vector<ashiato::Entity> entities;
    for (int value = 0; value < 4; ++value) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{value, 0}) != nullptr);
        if (value < 2) {
            REQUIRE(registry.add<Velocity>(entity, Velocity{}) != nullptr);
        }
        entities.push_back(entity);
    }

    std::vector<ashiato::Entity> visited;
    registry.job<const Position>(0).structural<Velocity>().each(
        [&](auto& context, ashiato::Entity entity, const Position&) {
            visited.push_back(entity);
            if (entity == entities[2] || entity == entities[3]) {
                REQUIRE(context.template add<Velocity>(Velocity{}) != nullptr);
            }
        });

    registry.run_jobs();

    REQUIRE(visited == std::vector<ashiato::Entity>{entities[3], entities[2], entities[1], entities[0]});
    std::vector<ashiato::Entity> grouped;
    registry.view<const Position, const Velocity>().each(
        [&](ashiato::Entity entity, const Position&, const Velocity&) { grouped.push_back(entity); });
    std::sort(grouped.begin(), grouped.end(), [](ashiato::Entity lhs, ashiato::Entity rhs) {
        return lhs.value < rhs.value;
    });
    REQUIRE(grouped == entities);
}

TEST_CASE("structural_any snapshots versioned matches before arbitrary group reordering") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.declare_owned_group<Position, Velocity>();

    std::vector<ashiato::Entity> entities;
    for (int value = 0; value < 4; ++value) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{value, 0}) != nullptr);
        REQUIRE(registry.add<Velocity>(entity, Velocity{}) != nullptr);
        entities.push_back(entity);
    }

    std::vector<ashiato::Entity> visited;
    registry.job<const Position>(0).structural_any<Velocity>().each(
        [&](auto& context, ashiato::Entity entity, const Position&) {
            visited.push_back(entity);
            if (entity == entities.front()) {
                REQUIRE(context.template remove<Velocity>(entities[2]));
            }
        });

    registry.run_jobs();

    REQUIRE(visited == entities);
    REQUIRE_FALSE(registry.contains<Velocity>(entities[2]));
}

TEST_CASE("structural_any revalidates snapshot entries that stop matching") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");

    std::vector<ashiato::Entity> entities;
    for (int value = 0; value < 4; ++value) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{value, 0}) != nullptr);
        REQUIRE(registry.add<Velocity>(entity, Velocity{}) != nullptr);
        entities.push_back(entity);
    }

    std::vector<ashiato::Entity> visited;
    registry.job<const Position, const Velocity>(0).structural_any<Velocity>().each(
        [&](auto& context, ashiato::Entity entity, const Position&, const Velocity&) {
            visited.push_back(entity);
            if (entity == entities.front()) {
                REQUIRE(context.template remove<Velocity>(entities[2]));
            }
        });

    registry.run_jobs();
    REQUIRE(visited == std::vector<ashiato::Entity>{entities[0], entities[1], entities[3]});
}

TEST_CASE("structural_any snapshots entity versions and skips replacements that reuse an index") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    std::vector<ashiato::Entity> entities;
    for (int value = 0; value < 4; ++value) {
        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{value, 0}) != nullptr);
        entities.push_back(entity);
    }

    ashiato::Entity replacement;
    auto subscription = registry.on_component_add<Disabled>(
        [&](ashiato::Registry& hooked_registry, ashiato::Entity) {
            REQUIRE(hooked_registry.destroy(entities[2]));
            replacement = hooked_registry.create();
            REQUIRE(ashiato::Registry::entity_index(replacement) == ashiato::Registry::entity_index(entities[2]));
            REQUIRE(replacement != entities[2]);
            REQUIRE(hooked_registry.add<Position>(replacement, Position{99, 0}) != nullptr);
        });
    REQUIRE(subscription.active());

    std::vector<ashiato::Entity> visited;
    registry.job<const Position>(0).structural_any<Disabled>().each(
        [&](auto& context, ashiato::Entity entity, const Position&) {
            visited.push_back(entity);
            if (entity == entities.front()) {
                REQUIRE(context.template add<Disabled>(entity));
            }
        });

    registry.run_jobs();

    REQUIRE(visited == std::vector<ashiato::Entity>{entities[0], entities[1], entities[3]});
    REQUIRE(replacement);
    REQUIRE(std::find(visited.begin(), visited.end(), replacement) == visited.end());
}

TEST_CASE("structural_any composes with external access and tag filters") {
    SECTION("external access") {
        ashiato::Registry registry;
        registry.register_component<Position>("Position");
        registry.register_component<Velocity>("Velocity");
        registry.register_component<Disabled>("Disabled");

        const ashiato::Entity first = registry.create();
        const ashiato::Entity second = registry.create();
        REQUIRE(registry.add<Position>(first, Position{}) != nullptr);
        REQUIRE(registry.add<Position>(second, Position{}) != nullptr);
        REQUIRE(registry.add<Velocity>(first, Velocity{}) != nullptr);
        REQUIRE(registry.add<Velocity>(second, Velocity{}) != nullptr);

        registry.job<const Position>(0)
            .access_other_entities<Velocity>()
            .structural_any<Disabled>()
            .each([&](auto& context, ashiato::Entity current, const Position&) {
                context.template write<Velocity>(current).dx += 1.0f;
                if (current == first) {
                    REQUIRE(context.template add<Disabled>(second));
                }
            });

        registry.run_jobs();
        REQUIRE(registry.has<Disabled>(second));
        REQUIRE(registry.get<Velocity>(first).dx == 1.0f);
        REQUIRE(registry.get<Velocity>(second).dx == 1.0f);
    }

    SECTION("tag filters") {
        ashiato::Registry registry;
        registry.register_component<Position>("Position");
        registry.register_component<Active>("Active");
        registry.register_component<Disabled>("Disabled");

        const ashiato::Entity entity = registry.create();
        REQUIRE(registry.add<Position>(entity, Position{}) != nullptr);
        REQUIRE(registry.add<Active>(entity));

        registry.job<const Position>(0)
            .with_tags<const Active>()
            .structural_any<Disabled>()
            .each([](auto& context, ashiato::Entity current, const Position&) {
                REQUIRE(context.template add<Disabled>(current));
            });

        registry.run_jobs();
        REQUIRE(registry.has<Disabled>(entity));
    }
}

TEST_CASE("structural jobs dispatch matching lifecycle hooks") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int added_count = 0;
    int removed_count = 0;
    auto add_subscription = registry.on_component_add<Disabled>(
        [&](ashiato::Registry& hooked_registry, ashiato::Entity hooked_entity) {
            REQUIRE(hooked_entity == entity);
            REQUIRE(hooked_registry.has<Disabled>(hooked_entity));
            ++added_count;
        });
    auto remove_subscription = registry.on_component_remove<Disabled>(
        [&](ashiato::Registry& hooked_registry, ashiato::Entity hooked_entity) {
            REQUIRE(hooked_entity == entity);
            REQUIRE(hooked_registry.has<Disabled>(hooked_entity));
            ++removed_count;
        });
    REQUIRE(add_subscription.active());
    REQUIRE(remove_subscription.active());

    registry.job<const Position>(0).structural<Disabled>().each(
        [](auto& view, ashiato::Entity, const Position&) {
            REQUIRE(view.template add<Disabled>());
        });
    registry.job<const Position>(1).structural<Disabled>().each(
        [](auto& view, ashiato::Entity, const Position&) {
            REQUIRE(view.template remove<Disabled>());
        });

    registry.run_jobs();

    REQUIRE(added_count == 1);
    REQUIRE(removed_count == 1);
    REQUIRE_FALSE(registry.has<Disabled>(entity));
}

TEST_CASE("mutable jobs and writes do not dispatch lifecycle hooks") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);

    int added_count = 0;
    auto add_subscription = registry.on_component_add<Position>(
        [&](ashiato::Registry&, ashiato::Entity) {
            ++added_count;
        });
    REQUIRE(add_subscription.active());

    registry.write<Position>(entity).x = 2;
    registry.job<Position>(0).each([](ashiato::Entity, Position& position) {
        position.x += 1;
    });

    registry.run_jobs();

    REQUIRE(registry.get<Position>(entity).x == 3);
    REQUIRE(added_count == 0);
}

TEST_CASE("structural jobs are isolated from otherwise independent jobs") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity structural =
        registry.job<const Position>(0).structural<Disabled>().each([](auto&, ashiato::Entity, const Position&) {});
    const ashiato::Entity independent = registry.job<Velocity>(1).each([](ashiato::Entity, Velocity&) {});

    const ashiato::JobSchedule schedule = ashiato::Orchestrator(registry).schedule();

    REQUIRE(schedule.stages.size() == 2);
    REQUIRE(schedule.stages[0].jobs == std::vector<ashiato::Entity>{structural});
    REQUIRE(schedule.stages[1].jobs == std::vector<ashiato::Entity>{independent});
}

TEST_CASE("structural access jobs can use access views and declared structural operations") {
    ashiato::Registry registry;
    registry.register_component<Position>("Position");
    registry.register_component<Velocity>("Velocity");
    registry.register_component<Disabled>("Disabled");

    const ashiato::Entity entity = registry.create();
    REQUIRE(registry.add<Position>(entity, Position{1, 0}) != nullptr);
    REQUIRE(registry.add<Velocity>(entity, Velocity{2.0f, 0.0f}) != nullptr);

    registry.job<const Position>(0).access_other_entities<Velocity>().structural<Disabled>().each(
        [](auto& view, ashiato::Entity current, const Position& position) {
            Velocity& velocity = view.template write<Velocity>(current);
            velocity.dx += static_cast<float>(position.x);
            REQUIRE(view.template add<Disabled>());
        });

    registry.run_jobs();

    REQUIRE(registry.get<Velocity>(entity).dx == 3.0f);
    REQUIRE(registry.has<Disabled>(entity));
}
