#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace ashiato {

namespace detail {

#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && defined(__ORDER_BIG_ENDIAN__)
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
inline constexpr bool host_is_little_endian = true;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
inline constexpr bool host_is_little_endian = false;
#else
#error "Unsupported byte order"
#endif
#elif defined(_M_IX86) || defined(_M_X64) || defined(_M_ARM) || defined(_M_ARM64)
inline constexpr bool host_is_little_endian = true;
#elif defined(_WIN32)
inline constexpr bool host_is_little_endian = true;
#elif defined(__cppcheck__) || defined(__CPPCHECK__)
inline constexpr bool host_is_little_endian = true;
#else
// cppcheck-suppress preprocessorErrorDirective
#error "Unsupported platform: cannot determine byte order"
#endif

constexpr std::uint16_t byte_swap(std::uint16_t value) noexcept {
    return static_cast<std::uint16_t>((value >> 8U) | (value << 8U));
}

constexpr std::uint32_t byte_swap(std::uint32_t value) noexcept {
    return ((value & 0x000000FFU) << 24U) |
        ((value & 0x0000FF00U) << 8U) |
        ((value & 0x00FF0000U) >> 8U) |
        ((value & 0xFF000000U) >> 24U);
}

constexpr std::uint64_t byte_swap(std::uint64_t value) noexcept {
    return ((value & 0x00000000000000FFULL) << 56U) |
        ((value & 0x000000000000FF00ULL) << 40U) |
        ((value & 0x0000000000FF0000ULL) << 24U) |
        ((value & 0x00000000FF000000ULL) << 8U) |
        ((value & 0x000000FF00000000ULL) >> 8U) |
        ((value & 0x0000FF0000000000ULL) >> 24U) |
        ((value & 0x00FF000000000000ULL) >> 40U) |
        ((value & 0xFF00000000000000ULL) >> 56U);
}

constexpr std::uint8_t little_endian_value(std::uint8_t value) noexcept {
    return value;
}

constexpr std::uint16_t little_endian_value(std::uint16_t value) noexcept {
    return host_is_little_endian ? value : byte_swap(value);
}

constexpr std::uint32_t little_endian_value(std::uint32_t value) noexcept {
    return host_is_little_endian ? value : byte_swap(value);
}

constexpr std::uint64_t little_endian_value(std::uint64_t value) noexcept {
    return host_is_little_endian ? value : byte_swap(value);
}

}  // namespace detail

class BitBuffer {
public:
    void clear() noexcept {
        bytes_.clear();
        bit_size_ = 0;
        read_bit_ = 0;
    }

    bool empty() const noexcept {
        return bit_size_ == 0;
    }

    std::size_t bit_size() const noexcept {
        return bit_size_;
    }

    std::size_t byte_size() const noexcept {
        return (bit_size_ + 7U) / 8U;
    }

    std::size_t size() const noexcept {
        return byte_size();
    }

    const std::vector<std::uint8_t>& bytes() const noexcept {
        return bytes_;
    }

    const std::uint8_t* data() const noexcept {
        return bytes_.data();
    }

    void assign_bytes(std::vector<std::uint8_t> bytes, std::size_t bit_size) {
        if (bit_size > bytes.size() * 8U) {
            throw std::invalid_argument("bit buffer bit size exceeds byte payload");
        }
        bytes_ = std::move(bytes);
        bit_size_ = bit_size;
        read_bit_ = 0;
        clear_unused_tail_bits();
    }

    void reserve_bytes(std::size_t capacity) {
        bytes_.reserve(capacity);
    }

    void truncate_bits(std::size_t bit_size) {
        if (bit_size > bit_size_) {
            throw std::out_of_range("bit buffer truncate past end");
        }
        bit_size_ = bit_size;
        bytes_.resize(byte_size());
        if (read_bit_ > bit_size_) {
            read_bit_ = bit_size_;
        }
        clear_unused_tail_bits();
    }

    std::size_t read_offset_bits() const noexcept {
        return read_bit_;
    }

    std::size_t remaining_bits() const noexcept {
        return bit_size_ - read_bit_;
    }

    void reset_read() noexcept {
        read_bit_ = 0;
    }

    void write_bool(bool value) {
        if ((bit_size_ % 8U) == 0) {
            bytes_.push_back(0);
        }
        if (value) {
            bytes_[bit_size_ / 8U] |= static_cast<std::uint8_t>(1U << (bit_size_ % 8U));
        } else {
            bytes_[bit_size_ / 8U] &= static_cast<std::uint8_t>(~(1U << (bit_size_ % 8U)));
        }
        ++bit_size_;
    }

    void write_bits(std::int64_t value, std::size_t num_bits) {
        write_unsigned_bits(static_cast<std::uint64_t>(value), num_bits);
    }

    void write_unsigned_bits(std::uint64_t value, std::size_t num_bits) {
        if (num_bits > 64U) {
            throw std::invalid_argument("bit buffer cannot push more than 64 bits at once");
        }
        if (num_bits == 0U) {
            return;
        }

        if ((bit_size_ % 8U) == 0 && (num_bits % 8U) == 0) {
            for (std::size_t byte = 0; byte < num_bits / 8U; ++byte) {
                bytes_.push_back(static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xFFU));
            }
            bit_size_ += num_bits;
            return;
        }

        const std::size_t bit_shift = bit_size_ % 8U;
        const std::size_t byte_offset = bit_size_ / 8U;
        const std::size_t touched_bytes = (bit_shift + num_bits + 7U) / 8U;
        const std::size_t new_bit_size = bit_size_ + num_bits;
        const std::size_t new_byte_size = (new_bit_size + 7U) / 8U;
        if (bytes_.size() < new_byte_size) {
            bytes_.resize(new_byte_size, 0U);
        }

        for (std::size_t byte = 0; byte < touched_bytes; ++byte) {
            const int source_start = static_cast<int>(byte * 8U) - static_cast<int>(bit_shift);
            std::uint8_t bits = 0;
            if (source_start >= 0) {
                if (source_start < 64) {
                    bits = static_cast<std::uint8_t>((value >> static_cast<unsigned>(source_start)) & 0xFFU);
                }
            } else {
                bits = static_cast<std::uint8_t>((value << static_cast<unsigned>(-source_start)) & 0xFFU);
            }

            std::uint8_t mask = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int source_bit = source_start + bit;
                if (source_bit >= 0 && static_cast<std::size_t>(source_bit) < num_bits) {
                    mask |= static_cast<std::uint8_t>(1U << bit);
                }
            }
            std::uint8_t& target = bytes_[byte_offset + byte];
            target = static_cast<std::uint8_t>((target & ~mask) | (bits & mask));
        }
        bit_size_ = new_bit_size;
    }

    void overwrite_unsigned_bits(std::size_t bit_offset, std::uint64_t value, std::size_t num_bits) {
        if (num_bits > 64U) {
            throw std::invalid_argument("bit buffer cannot overwrite more than 64 bits at once");
        }
        if (bit_offset > bit_size_ || num_bits > bit_size_ - bit_offset) {
            throw std::out_of_range("bit buffer overwrite past end");
        }
        if (num_bits == 0U) {
            return;
        }

        const std::size_t bit_shift = bit_offset % 8U;
        const std::size_t byte_offset = bit_offset / 8U;
        if (bit_shift == 0U && (num_bits % 8U) == 0U) {
            for (std::size_t byte = 0; byte < num_bits / 8U; ++byte) {
                bytes_[byte_offset + byte] = static_cast<std::uint8_t>((value >> (byte * 8U)) & 0xFFU);
            }
            return;
        }

        const std::size_t touched_bytes = (bit_shift + num_bits + 7U) / 8U;
        if (touched_bytes <= sizeof(std::uint64_t)) {
            std::uint64_t window = 0;
            for (std::size_t byte = 0; byte < touched_bytes; ++byte) {
                window |= std::uint64_t{bytes_[byte_offset + byte]} << (byte * 8U);
            }
            const std::uint64_t value_mask = num_bits == 64U
                ? std::numeric_limits<std::uint64_t>::max()
                : ((std::uint64_t{1} << num_bits) - 1U);
            const std::uint64_t mask = value_mask << bit_shift;
            window = (window & ~mask) | ((value & value_mask) << bit_shift);
            for (std::size_t byte = 0; byte < touched_bytes; ++byte) {
                bytes_[byte_offset + byte] = static_cast<std::uint8_t>((window >> (byte * 8U)) & 0xFFU);
            }
            return;
        }

        for (std::size_t bit = 0; bit < num_bits; ++bit) {
            const std::size_t target = bit_offset + bit;
            const auto mask = static_cast<std::uint8_t>(1U << (target % 8U));
            if (((value >> bit) & 1U) != 0) {
                bytes_[target / 8U] |= mask;
            } else {
                bytes_[target / 8U] &= static_cast<std::uint8_t>(~mask);
            }
        }
    }

    void write_bytes(const char* data, std::size_t num_bytes) {
        if (data == nullptr && num_bytes != 0) {
            throw std::invalid_argument("bit buffer cannot push bytes from null data");
        }
        if (num_bytes == 0) {
            return;
        }

        if ((bit_size_ % 8U) == 0) {
            const auto* begin = reinterpret_cast<const std::uint8_t*>(data);
            bytes_.insert(bytes_.end(), begin, begin + num_bytes);
            bit_size_ += num_bytes * 8U;
            return;
        }

        for (std::size_t index = 0; index < num_bytes; ++index) {
            write_bits(static_cast<unsigned char>(data[index]), 8U);
        }
    }

    void write_uint8(std::uint8_t value) {
        write_little_endian_unsigned(value);
    }

    void write_uint16_le(std::uint16_t value) {
        write_little_endian_unsigned(value);
    }

    void write_uint32_le(std::uint32_t value) {
        write_little_endian_unsigned(value);
    }

    void write_uint64_le(std::uint64_t value) {
        write_little_endian_unsigned(value);
    }

    void write_int8(std::int8_t value) {
        write_little_endian_signed(value);
    }

    void write_int16_le(std::int16_t value) {
        write_little_endian_signed(value);
    }

    void write_int32_le(std::int32_t value) {
        write_little_endian_signed(value);
    }

    void write_int64_le(std::int64_t value) {
        write_little_endian_signed(value);
    }

    void write_float32_le(float value) {
        static_assert(sizeof(float) == sizeof(std::uint32_t), "float32 serialization requires 32-bit float");
        std::uint32_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        write_uint32_le(raw);
    }

    void write_float64_le(double value) {
        static_assert(sizeof(double) == sizeof(std::uint64_t), "float64 serialization requires 64-bit double");
        std::uint64_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        write_uint64_le(raw);
    }

    void write_buffer_bits(const BitBuffer& source) {
        if (source.bit_size_ == 0) {
            return;
        }

        if ((bit_size_ % 8U) == 0) {
            bytes_.insert(bytes_.end(), source.bytes_.begin(), source.bytes_.end());
            bit_size_ += source.bit_size_;
            return;
        }

        for (std::size_t bit = 0; bit < source.bit_size_; ++bit) {
            const bool value =
                (source.bytes_[bit / 8U] & static_cast<std::uint8_t>(1U << (bit % 8U))) != 0;
            write_bool(value);
        }
    }

    void read_buffer_bits(BitBuffer& out, std::size_t num_bits) {
        ensure_can_read(num_bits);
        if (num_bits == 0) {
            return;
        }

        if ((read_bit_ % 8U) == 0 && (out.bit_size_ % 8U) == 0 && (num_bits % 8U) == 0) {
            const std::size_t num_bytes = num_bits / 8U;
            out.write_bytes(reinterpret_cast<const char*>(bytes_.data() + (read_bit_ / 8U)), num_bytes);
            read_bit_ += num_bits;
            return;
        }

        for (std::size_t bit = 0; bit < num_bits; ++bit) {
            out.write_bool(read_bool());
        }
    }

    void skip_bits(std::size_t num_bits) {
        ensure_can_read(num_bits);
        read_bit_ += num_bits;
    }

    bool read_bool() {
        ensure_can_read(1U);
        const bool value = (bytes_[read_bit_ / 8U] & static_cast<std::uint8_t>(1U << (read_bit_ % 8U))) != 0;
        ++read_bit_;
        return value;
    }

    std::int64_t read_bits(std::size_t num_bits) {
        return static_cast<std::int64_t>(read_unsigned_bits(num_bits));
    }

    std::uint64_t read_unsigned_bits(std::size_t num_bits) {
        if (num_bits > 64U) {
            throw std::invalid_argument("bit buffer cannot read more than 64 bits at once");
        }
        if (num_bits == 0U) {
            return 0;
        }
        ensure_can_read(num_bits);

        if ((read_bit_ % 8U) == 0 && (num_bits % 8U) == 0) {
            std::uint64_t value = 0;
            for (std::size_t byte = 0; byte < num_bits / 8U; ++byte) {
                value |= std::uint64_t{bytes_[(read_bit_ / 8U) + byte]} << (byte * 8U);
            }
            read_bit_ += num_bits;
            return value;
        }

        const std::size_t bit_shift = read_bit_ % 8U;
        const std::size_t byte_offset = read_bit_ / 8U;
        std::uint64_t window = 0;
        const std::size_t available_bytes = bytes_.size() - byte_offset;
        const std::size_t low_bytes = available_bytes < sizeof(std::uint64_t)
            ? available_bytes
            : sizeof(std::uint64_t);
        for (std::size_t byte = 0; byte < low_bytes; ++byte) {
            window |= std::uint64_t{bytes_[byte_offset + byte]} << (byte * 8U);
        }

        std::uint64_t value = window >> bit_shift;
        const std::size_t bits_from_window = 64U - bit_shift;
        if (num_bits > bits_from_window) {
            value |= std::uint64_t{bytes_[byte_offset + sizeof(std::uint64_t)]} << bits_from_window;
        }
        if (num_bits < 64U) {
            value &= (std::uint64_t{1} << num_bits) - 1U;
        }
        read_bit_ += num_bits;
        return value;
    }

    void read_bytes(char* out, std::size_t num_bytes) {
        if (out == nullptr && num_bytes != 0) {
            throw std::invalid_argument("bit buffer cannot read bytes into null data");
        }
        if (num_bytes == 0) {
            return;
        }
        ensure_can_read(num_bytes * 8U);

        if ((read_bit_ % 8U) == 0) {
            std::memcpy(out, bytes_.data() + (read_bit_ / 8U), num_bytes);
            read_bit_ += num_bytes * 8U;
            return;
        }

        for (std::size_t index = 0; index < num_bytes; ++index) {
            out[index] = static_cast<char>(read_bits(8U));
        }
    }

    std::uint8_t read_uint8() {
        return read_little_endian_unsigned<std::uint8_t>();
    }

    std::uint16_t read_uint16_le() {
        return read_little_endian_unsigned<std::uint16_t>();
    }

    std::uint32_t read_uint32_le() {
        return read_little_endian_unsigned<std::uint32_t>();
    }

    std::uint64_t read_uint64_le() {
        return read_little_endian_unsigned<std::uint64_t>();
    }

    std::int8_t read_int8() {
        return read_little_endian_signed<std::int8_t>();
    }

    std::int16_t read_int16_le() {
        return read_little_endian_signed<std::int16_t>();
    }

    std::int32_t read_int32_le() {
        return read_little_endian_signed<std::int32_t>();
    }

    std::int64_t read_int64_le() {
        return read_little_endian_signed<std::int64_t>();
    }

    float read_float32_le() {
        static_assert(sizeof(float) == sizeof(std::uint32_t), "float32 serialization requires 32-bit float");
        const std::uint32_t raw = read_uint32_le();
        float value = 0.0F;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    double read_float64_le() {
        static_assert(sizeof(double) == sizeof(std::uint64_t), "float64 serialization requires 64-bit double");
        const std::uint64_t raw = read_uint64_le();
        double value = 0.0;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    friend bool operator==(const BitBuffer& lhs, const BitBuffer& rhs) noexcept {
        return lhs.bit_size_ == rhs.bit_size_ && lhs.bytes_ == rhs.bytes_;
    }

    friend bool operator!=(const BitBuffer& lhs, const BitBuffer& rhs) noexcept {
        return !(lhs == rhs);
    }

private:
    template <typename T>
    void write_little_endian_unsigned(T value) {
        static_assert(std::is_unsigned<T>::value, "little-endian writes require an unsigned integer type");
        const T wire_value = detail::little_endian_value(value);
        write_bytes(reinterpret_cast<const char*>(&wire_value), sizeof(wire_value));
    }

    template <typename T>
    void write_little_endian_signed(T value) {
        static_assert(std::is_signed<T>::value, "little-endian writes require a signed integer type");
        using Unsigned = typename std::make_unsigned<T>::type;
        Unsigned raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));
        write_little_endian_unsigned(raw);
    }

    template <typename T>
    T read_little_endian_unsigned() {
        static_assert(std::is_unsigned<T>::value, "little-endian reads require an unsigned integer type");
        T wire_value = 0;
        read_bytes(reinterpret_cast<char*>(&wire_value), sizeof(wire_value));
        return detail::little_endian_value(wire_value);
    }

    template <typename T>
    T read_little_endian_signed() {
        static_assert(std::is_signed<T>::value, "little-endian reads require a signed integer type");
        using Unsigned = typename std::make_unsigned<T>::type;
        const Unsigned raw = read_little_endian_unsigned<Unsigned>();
        T value = 0;
        std::memcpy(&value, &raw, sizeof(value));
        return value;
    }

    void ensure_can_read(std::size_t num_bits) const {
        if (num_bits > remaining_bits()) {
            throw std::out_of_range("bit buffer read past end");
        }
    }

    void clear_unused_tail_bits() noexcept {
        const std::size_t used_bits = bit_size_ % 8U;
        if (used_bits == 0U || bytes_.empty()) {
            return;
        }

        const auto mask = static_cast<std::uint8_t>((1U << used_bits) - 1U);
        bytes_.back() &= mask;
    }

    std::vector<std::uint8_t> bytes_;
    std::size_t bit_size_ = 0;
    std::size_t read_bit_ = 0;
};

}  // namespace ashiato
