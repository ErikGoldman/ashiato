#include "ashiato/ashiato.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("serialization trace capture records nested bit ranges") {
    ashiato::BitBuffer payload;
    ashiato::SerializationTraceCapture capture(true, &payload, "packet");

    {
        ashiato::ScopedSerializationTraceScope header(&capture, "header");
        payload.write_bits(0xab, 8U);
    }
    {
        ashiato::ScopedSerializationTraceScope body(&capture, "body");
        payload.write_bits(0xcdef, 16U);
    }

    capture.finish();

    REQUIRE(capture.wire_bits() == 24U);
    REQUIRE(capture.payload_bits() == 24U);
    REQUIRE(capture.scopes().size() == 3U);
    REQUIRE(capture.scopes()[0].name == "packet");
    REQUIRE(capture.scopes()[0].payload_bits == 24U);
    REQUIRE(capture.scopes()[1].name == "header");
    REQUIRE(capture.scopes()[1].parent == 0U);
    REQUIRE(capture.scopes()[1].payload_bits == 8U);
    REQUIRE(capture.scopes()[2].name == "body");
    REQUIRE(capture.scopes()[2].parent == 0U);
    REQUIRE(capture.scopes()[2].payload_bits == 16U);
}

TEST_CASE("serialization trace capture truncates rolled back scopes") {
    ashiato::BitBuffer payload;
    ashiato::SerializationTraceCapture capture(true, &payload, "packet");

    {
        ashiato::ScopedSerializationTraceScope kept(&capture, "kept");
        payload.write_bits(0xab, 8U);
    }
    const std::size_t rollback_bits = payload.bit_size();
    {
        ashiato::ScopedSerializationTraceScope rolled_back(&capture, "rolled_back");
        payload.write_bits(0xcdef, 16U);
    }
    payload.truncate_bits(rollback_bits);
    capture.truncate_to_bits(rollback_bits);
    capture.finish();

    REQUIRE(capture.wire_bits() == 8U);
    REQUIRE(capture.scopes().size() == 2U);
    REQUIRE(capture.scopes()[0].payload_bits == 8U);
    REQUIRE(capture.scopes()[1].name == "kept");
    REQUIRE(capture.scopes()[1].payload_bits == 8U);
}

TEST_CASE("serialization trace scope can use component serialization context") {
    ashiato::BitBuffer payload;
    ashiato::SerializationTraceCapture capture(true, &payload, "component");
    ashiato::ComponentSerializationContext context{nullptr, &capture};

    {
        ASHIATO_SERIALIZATION_TRACE_SCOPE("field");
        payload.write_bits(0xab, 8U);
    }
    capture.finish();

    REQUIRE(capture.scopes().size() == 2U);
    REQUIRE(capture.scopes()[1].name == "field");
    REQUIRE(capture.scopes()[1].payload_bits == 8U);
}
