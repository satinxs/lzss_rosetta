#include <iostream>
#include <stdexcept>
#include <cstdint>

template <typename T>
class Array
{
private:
    T *buffer;

public:
    Array(uint32_t length)
    {
        this->buffer = new T[length];
        this->length = length;
    }

    uint32_t length;

    T &operator[](std::size_t idx) { return this->buffer[idx]; }
    const T &operator[](std::size_t idx) const { return this->buffer[idx]; }

    T *get_buffer()
    {
        return this->buffer;
    }
};

class BitStream
{
private:
    Array<uint8_t> buffer;

    uint32_t buffer_length;

    uint8_t byte_buffer;
    uint8_t bit_count;

public:
    BitStream(Array<uint8_t> buffer) : buffer(buffer)
    {
        this->buffer_length = buffer.length;
        this->buffer_position = 0;
        this->byte_buffer = 0;
        this->bit_count = 0;
    }

    uint32_t buffer_position;

    void flush()
    {
        if (this->bit_count == 0)
            return;

        if (this->bit_count < 8)
            this->byte_buffer <<= (8 - this->bit_count);

        if (this->buffer_position >= this->buffer_length)
            throw std::out_of_range("buffer");

        this->buffer[this->buffer_position++] = this->byte_buffer;
        this->byte_buffer = 0;
        this->bit_count = 0;
    }

    void unflush()
    {
        if (this->buffer_position < this->buffer_length)
        {
            this->byte_buffer = this->buffer[this->buffer_position++];
            this->bit_count = 8;
        }
        else
            throw std::out_of_range("buffer");
    }

    bool read_bit()
    {
        if (this->bit_count == 0)
            this->unflush();

        this->bit_count -= 1;

        return (this->byte_buffer & (1 << (this->bit_count))) > 0;
    }

    void write_bit(bool bit)
    {
        this->byte_buffer <<= 1;
        this->byte_buffer |= bit ? 1 : 0;

        this->bit_count += 1;

        if (this->bit_count == 8)
            this->flush();
    }

    uint32_t read_uint32(uint8_t bits)
    {
        uint32_t number = 0;
        for (uint32_t i = 0; i < bits; i += 1)
        {
            number <<= 1;

            bool bit = this->read_bit();

            number |= bit ? 1 : 0;
        }

        return number;
    }

    void write_uint32(uint32_t number, uint8_t bits)
    {
        while (bits > 0)
        {
            uint32_t mask = 1 << (bits - 1);
            bool bit = (number & mask) > 0;

            this->write_bit(bit);

            bits -= 1;
        }
    }

    // Reads an uint32 using 7-bit VLQ approach
    uint32_t read_7bit_uint32()
    {
        uint32_t number = 0;
        uint32_t shift = 0;
        while (true)
        {
            uint8_t byte = this->read_uint32(8);
            number |= (byte & 127) << shift;
            shift += 7;

            if ((byte & 128) == 0 || shift > 32)
                break;
        }

        return number;
    }

    // Writes an uint32 using 7-bit VLQ approach
    void write_7bit_uint32(uint32_t number)
    {
        while (number > 127)
        {
            uint32_t b = 128 | (number & 127); // Set the first bit as 1
            this->write_uint32(b, 8);
            number >>= 7;
        }

        if (number > 0)
            this->write_uint32(number & 127, 8);
    }
};

typedef struct match_t
{
    uint32_t offset;
    uint32_t length;
} match_t;

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

static inline match_t _create_match(uint32_t offset, uint32_t length)
{
    match_t match = {0};

    match.offset = offset;
    match.length = length;

    return match;
}

class Lzss
{
private:
    uint8_t offset_bits;
    uint8_t length_bits;

    uint32_t max_offset;
    uint32_t minimum_length;
    uint32_t maximum_length;

    match_t get_longest_match(Array<uint8_t> input, uint32_t index)
    {
        if (index + this->minimum_length >= input.length)
            return _create_match(0, 0);

        uint32_t best_offset = 0, best_length = 0;
        uint32_t offset = (this->max_offset > index) ? 0 : index - this->max_offset;

        while (offset < index && offset < input.length)
        {
            uint32_t length = 0;

            while (offset + length < input.length && index + length < input.length && input[offset + length] == input[index + length])
                length += 1;

            // We compare greater or equal, since a lower offset is better
            if (length >= best_length)
            {
                best_length = length;
                best_offset = offset;
            }

            offset += 1;
        }

        // Substract the found offset from the actual index to get the resulting offset.
        return _create_match(index - best_offset, MIN(best_length, this->maximum_length));
    }

public:
    Lzss(uint8_t offset_bits, uint8_t length_bits, uint8_t minimum_length)
    {
        this->offset_bits = offset_bits;
        this->max_offset = (1 << offset_bits) - 1;

        this->minimum_length = minimum_length;
        this->length_bits = length_bits;
        this->maximum_length = (1 << length_bits) - 1;
    }

    uint32_t get_upper_bound(uint32_t input_length)
    {
        // We sum all bits in the worst case scenario: 32 for the total input length and input_lenght * 9 (literal flag + byte)
        uint32_t total_bits = 32 + input_length * 9;

        // If it's divisible by 8, we return the length. If not we sum 1 to account for the extra bits.
        return (total_bits / 8) + ((total_bits % 8 > 0) ? 1 : 0);
    }

    Array<uint8_t> encode(Array<uint8_t> input)
    {
        Array<uint8_t> output(get_upper_bound(input.length));

        BitStream stream(output);

        stream.write_7bit_uint32(input.length);

        for (uint32_t index = 0; index < input.length;)
        {
            match_t match = this->get_longest_match(input, index);

            if (match.length >= this->minimum_length)
            {
                stream.write_bit(true);
                stream.write_uint32(match.offset, this->offset_bits);
                stream.write_uint32(match.length, this->length_bits);
                index += match.length;
            }
            else
            {
                stream.write_bit(false);
                stream.write_uint32(input[index], 8);
                index += 1;
            }
        }

        stream.flush();
        output.length = stream.buffer_position;

        return output;
    }

    Array<uint8_t> decode(Array<uint8_t> input)
    {
        BitStream stream(input);
        uint32_t original_length = stream.read_7bit_uint32();
        Array<uint8_t> output(original_length);

        for (uint32_t index = 0; index < original_length;)
        {
            bool is_pair = stream.read_bit();
            if (is_pair)
            {
                uint32_t offset = stream.read_uint32(this->offset_bits);
                uint32_t length = stream.read_uint32(this->length_bits);
                for (uint32_t i = 0; i < length; i += 1)
                    output[index + i] = output[(index - offset) + i];
                index += length;
            }
            else
            {
                auto literal = stream.read_uint32(8);
                output[index] = literal & 0xFF;
                index += 1;
            }
        }

        return output;
    }
};

Array<uint8_t> read_file(const char *file_name)
{
    FILE *file = fopen(file_name, "rb");
    if (file == NULL)
        throw std::invalid_argument("file_name");

    fseek(file, 0, SEEK_END);      // Seek to the end of the file
    uint32_t length = ftell(file); // Get how many bytes the file contains
    fseek(file, 0, SEEK_SET);      // Rewind the file pointer to 0

    Array<uint8_t> buffer(length);

    uint32_t read_value = fread(buffer.get_buffer(), sizeof(uint8_t), buffer.length, file);

    if (read_value != buffer.length)
    {
        buffer.~Array();
        fclose(file);
        throw std::logic_error("Could not read file");
    }

    fclose(file);

    return buffer;
}

int main(int argc, const char **argv)
{
    if (argc != 2)
    {
        std::cout << "Expected a filename\n";
        return -1;
    }

    auto input = read_file(argv[1]);

    Lzss lzss(10, 6, 2);

    auto compressed = lzss.encode(input);

    auto uncompressed = lzss.decode(compressed);

    for (uint32_t i = 0; i < input.length; i++)
    {
        if (input[i] != uncompressed[i])
        {
            std::cout << "Byte mismatch at byte " << i << "\n";
            return -1;
        }
    }

    return 0;
}
