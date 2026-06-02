#include "ashiato/bit_buffer.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

TEST_CASE("bit buffer writes and reads bits bytes and bools") {
    ashiato::BitBuffer buffer;
    buffer.write_bool(true);
    buffer.write_bits(0b101, 3U);
    buffer.write_bytes("AZ", 2U);

    REQUIRE(buffer.bit_size() == 20U);
    REQUIRE(buffer.byte_size() == 3U);
    REQUIRE(buffer.read_bool());
    REQUIRE(buffer.read_bits(3U) == 0b101);

    char bytes[2]{};
    buffer.read_bytes(bytes, 2U);
    REQUIRE(bytes[0] == 'A');
    REQUIRE(bytes[1] == 'Z');
    REQUIRE_THROWS_AS(buffer.read_bool(), std::out_of_range);
}

TEST_CASE("bit buffer preserves exact non byte aligned frame lengths") {
    ashiato::BitBuffer source;
    source.write_bits(0b101, 3U);
    source.write_unsigned_bits(0xcafebabeU, 32U);

    ashiato::BitBuffer loaded;
    loaded.assign_bytes(source.bytes(), source.bit_size());

    REQUIRE(loaded.bit_size() == 35U);
    REQUIRE(loaded.read_bits(3U) == 0b101);
    REQUIRE(loaded.read_unsigned_bits(32U) == 0xcafebabeU);
    REQUIRE(loaded.remaining_bits() == 0U);
}

TEST_CASE("bit buffer clears stale trailing bits after assigning non byte aligned payloads") {
    ashiato::BitBuffer buffer;
    buffer.assign_bytes(std::vector<std::uint8_t>{0xFEU}, 1U);

    buffer.write_bool(false);

    REQUIRE(buffer.bit_size() == 2U);
    REQUIRE(buffer.read_bool() == false);
    REQUIRE(buffer.read_bool() == false);
}

TEST_CASE("bit buffer copies only logical bits from non byte aligned source buffers") {
    ashiato::BitBuffer source;
    source.assign_bytes(std::vector<std::uint8_t>{0xFEU}, 1U);

    ashiato::BitBuffer copied;
    copied.write_buffer_bits(source);
    copied.write_bool(false);

    REQUIRE(copied.bit_size() == 2U);
    REQUIRE(copied.read_bool() == false);
    REQUIRE(copied.read_bool() == false);
}

TEST_CASE("bit buffer validates invalid reads writes and assignment") {
    ashiato::BitBuffer buffer;
    REQUIRE_THROWS_AS(buffer.write_bits(0, 65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_unsigned_bits(65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_bool(), std::out_of_range);
    REQUIRE_THROWS_AS(buffer.assign_bytes({}, 1U), std::invalid_argument);
}

TEST_CASE("bit buffer supports byte aligned unsigned and byte operations") {
    ashiato::BitBuffer buffer;
    buffer.reserve_bytes(8U);
    buffer.write_unsigned_bits(0x1234U, 16U);
    buffer.write_bytes("xy", 2U);

    REQUIRE_FALSE(buffer.empty());
    REQUIRE(buffer.size() == 4U);
    REQUIRE(buffer.data() == buffer.bytes().data());
    REQUIRE(buffer.remaining_bits() == 32U);
    REQUIRE(buffer.read_unsigned_bits(16U) == 0x1234U);

    char bytes[2]{};
    buffer.read_bytes(bytes, 2U);
    REQUIRE(bytes[0] == 'x');
    REQUIRE(bytes[1] == 'y');
    REQUIRE(buffer.remaining_bits() == 0U);

    buffer.clear();
    REQUIRE(buffer.empty());
    REQUIRE(buffer.bit_size() == 0U);
    REQUIRE(buffer.read_offset_bits() == 0U);
}

TEST_CASE("bit buffer supports unaligned unsigned byte and buffer operations") {
    ashiato::BitBuffer buffer;
    buffer.write_bool(true);
    buffer.write_unsigned_bits(0x1ffU, 9U);
    buffer.write_bytes("A", 1U);

    REQUIRE(buffer.read_bool());
    REQUIRE(buffer.read_unsigned_bits(9U) == 0x1ffU);
    char byte{};
    buffer.read_bytes(&byte, 1U);
    REQUIRE(byte == 'A');
    REQUIRE(buffer.remaining_bits() == 0U);

    ashiato::BitBuffer source;
    source.write_bool(false);
    source.write_bool(true);

    ashiato::BitBuffer copied;
    copied.write_bool(true);
    copied.write_buffer_bits(source);
    REQUIRE(copied.bit_size() == 3U);
    REQUIRE(copied.read_bool());
    REQUIRE_FALSE(copied.read_bool());
    REQUIRE(copied.read_bool());

    ashiato::BitBuffer frame;
    frame.write_bool(false);
    frame.write_bytes("B", 1U);
    ashiato::BitBuffer extracted;
    frame.read_buffer_bits(extracted, 9U);
    REQUIRE_FALSE(extracted.read_bool());
    char extracted_byte{};
    extracted.read_bytes(&extracted_byte, 1U);
    REQUIRE(extracted_byte == 'B');
}

TEST_CASE("bit buffer overwrites aligned unaligned and wide bit ranges") {
    ashiato::BitBuffer buffer;
    buffer.write_unsigned_bits(0U, 16U);
    buffer.overwrite_unsigned_bits(0U, 0xabcdU, 16U);
    REQUIRE(buffer.read_unsigned_bits(16U) == 0xabcdU);

    buffer.reset_read();
    buffer.overwrite_unsigned_bits(3U, 0x1fU, 5U);
    buffer.skip_bits(3U);
    REQUIRE(buffer.read_unsigned_bits(5U) == 0x1fU);

    ashiato::BitBuffer wide;
    wide.write_bool(false);
    wide.write_unsigned_bits(0U, 64U);
    wide.overwrite_unsigned_bits(1U, 0xffffffffffffffffULL, 64U);
    wide.skip_bits(1U);
    REQUIRE(wide.read_unsigned_bits(64U) == 0xffffffffffffffffULL);
}

TEST_CASE("bit buffer accepts no-op operations and rejects null byte buffers") {
    ashiato::BitBuffer buffer;
    buffer.write_unsigned_bits(123U, 0U);
    buffer.write_bytes(nullptr, 0U);
    buffer.read_bytes(nullptr, 0U);
    buffer.read_buffer_bits(buffer, 0U);
    REQUIRE(buffer.read_bits(0U) == 0);
    REQUIRE(buffer.bit_size() == 0U);

    REQUIRE_THROWS_AS(buffer.write_bytes(nullptr, 1U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.read_bytes(nullptr, 1U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.overwrite_unsigned_bits(0U, 0U, 65U), std::invalid_argument);
    REQUIRE_THROWS_AS(buffer.overwrite_unsigned_bits(1U, 0U, 1U), std::out_of_range);
}

TEST_CASE("bit buffer reads unaligned 64-bit values across a byte window") {
    ashiato::BitBuffer buffer;
    buffer.write_bool(true);
    buffer.write_unsigned_bits(0x0123456789abcdefULL, 64U);
    buffer.write_bool(false);

    REQUIRE(buffer.read_bool());
    REQUIRE(buffer.read_unsigned_bits(64U) == 0x0123456789abcdefULL);
    REQUIRE_FALSE(buffer.read_bool());
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer masks overwritten tail bits without disturbing neighbors") {
    ashiato::BitBuffer buffer;
    buffer.write_unsigned_bits(0xffffU, 16U);
    buffer.overwrite_unsigned_bits(5U, 0U, 6U);

    REQUIRE(buffer.read_unsigned_bits(5U) == 0x1fU);
    REQUIRE(buffer.read_unsigned_bits(6U) == 0U);
    REQUIRE(buffer.read_unsigned_bits(5U) == 0x1fU);
}

TEST_CASE("bit buffer writes endian-safe scalar values with little-endian byte order") {
    ashiato::BitBuffer buffer;
    buffer.write_uint8(0x12U);
    buffer.write_uint16_le(0x3456U);
    buffer.write_uint32_le(0x789abcdeU);
    buffer.write_uint64_le(0x0123456789abcdefULL);

    const std::vector<std::uint8_t> expected{
        0x12U,
        0x56U,
        0x34U,
        0xdeU,
        0xbcU,
        0x9aU,
        0x78U,
        0xefU,
        0xcdU,
        0xabU,
        0x89U,
        0x67U,
        0x45U,
        0x23U,
        0x01U,
    };
    REQUIRE(buffer.bytes() == expected);

    REQUIRE(buffer.read_uint8() == 0x12U);
    REQUIRE(buffer.read_uint16_le() == 0x3456U);
    REQUIRE(buffer.read_uint32_le() == 0x789abcdeU);
    REQUIRE(buffer.read_uint64_le() == 0x0123456789abcdefULL);
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer round-trips endian-safe signed scalar values") {
    ashiato::BitBuffer buffer;
    buffer.write_int8(static_cast<std::int8_t>(-2));
    buffer.write_int16_le(static_cast<std::int16_t>(-1234));
    buffer.write_int32_le(static_cast<std::int32_t>(-12345678));
    buffer.write_int64_le(static_cast<std::int64_t>(-123456789012345678LL));

    REQUIRE(buffer.read_int8() == static_cast<std::int8_t>(-2));
    REQUIRE(buffer.read_int16_le() == static_cast<std::int16_t>(-1234));
    REQUIRE(buffer.read_int32_le() == static_cast<std::int32_t>(-12345678));
    REQUIRE(buffer.read_int64_le() == static_cast<std::int64_t>(-123456789012345678LL));
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer endian-safe scalar values work at unaligned bit offsets") {
    ashiato::BitBuffer buffer;
    buffer.write_bool(true);
    buffer.write_uint16_le(0x1234U);
    buffer.write_int32_le(static_cast<std::int32_t>(-42));
    buffer.write_float32_le(-0.0F);
    buffer.write_bool(false);

    REQUIRE(buffer.read_bool());
    REQUIRE(buffer.read_uint16_le() == 0x1234U);
    REQUIRE(buffer.read_int32_le() == static_cast<std::int32_t>(-42));

    const float negative_zero = buffer.read_float32_le();
    std::uint32_t negative_zero_bits = 0;
    std::memcpy(&negative_zero_bits, &negative_zero, sizeof(negative_zero_bits));
    REQUIRE(negative_zero_bits == 0x80000000U);

    REQUIRE_FALSE(buffer.read_bool());
    REQUIRE(buffer.remaining_bits() == 0U);
}

TEST_CASE("bit buffer endian-safe float methods preserve exact bit patterns") {
    std::uint32_t raw_float = 0x7fc12345U;
    float float_value = 0.0F;
    std::memcpy(&float_value, &raw_float, sizeof(float_value));

    std::uint64_t raw_double = 0x7ff8123456789abcULL;
    double double_value = 0.0;
    std::memcpy(&double_value, &raw_double, sizeof(double_value));

    ashiato::BitBuffer buffer;
    buffer.write_float32_le(float_value);
    buffer.write_float64_le(double_value);

    const std::vector<std::uint8_t> expected{
        0x45U,
        0x23U,
        0xc1U,
        0x7fU,
        0xbcU,
        0x9aU,
        0x78U,
        0x56U,
        0x34U,
        0x12U,
        0xf8U,
        0x7fU,
    };
    REQUIRE(buffer.bytes() == expected);

    const float read_float = buffer.read_float32_le();
    const double read_double = buffer.read_float64_le();

    std::uint32_t read_float_bits = 0;
    std::memcpy(&read_float_bits, &read_float, sizeof(read_float_bits));
    std::uint64_t read_double_bits = 0;
    std::memcpy(&read_double_bits, &read_double, sizeof(read_double_bits));

    REQUIRE(read_float_bits == raw_float);
    REQUIRE(read_double_bits == raw_double);
}

TEST_CASE("bit buffer endian-safe scalar reads reject truncated payloads") {
    ashiato::BitBuffer buffer;
    buffer.assign_bytes(std::vector<std::uint8_t>{0x34U}, 8U);

    REQUIRE_THROWS_AS(buffer.read_uint16_le(), std::out_of_range);
}
