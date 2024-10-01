#include <stdio.h>
#include <memory.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum error_t
{
    ERROR_ALL_GOOD = 0,
    ERROR_NO_OP,
    ERROR_BUFFER_OUT_OF_BOUNDS,
    ERROR_COULD_NOT_ALLOCATE,
    ERROR_WRONG_OUTPUT_SIZE
} error_t;

typedef struct array_t
{
    uint8_t *bytes;
    uint32_t length;
} array_t;

typedef struct bit_stream_t
{
    uint8_t *buffer;
    uint32_t buffer_length;
    uint32_t buffer_position;

    uint8_t byte_buffer;
    uint8_t bit_count;
} bit_stream_t;

typedef struct lzss_config_t
{
    uint8_t offset_bits;
    uint32_t max_offset;

    uint8_t minimum_length;
    uint8_t length_bits;
    uint32_t max_length;
} lzss_config_t;

bit_stream_t bit_stream_init(array_t buffer)
{
    return (bit_stream_t){
        .buffer = buffer.bytes,
        .buffer_length = buffer.length,
        .buffer_position = 0,

        .byte_buffer = 0,
        .bit_count = 0,
    };
}

error_t bit_stream_unflush(bit_stream_t *stream)
{
    if (stream->buffer_position < stream->buffer_length)
    {
        stream->byte_buffer = stream->buffer[stream->buffer_position++];
        stream->bit_count = 8;

        return ERROR_ALL_GOOD;
    }

    return ERROR_BUFFER_OUT_OF_BOUNDS;
}

error_t bit_stream_flush(bit_stream_t *stream)
{
    if (stream->bit_count == 0)
        return ERROR_ALL_GOOD;

    if (stream->bit_count < 8)
        stream->byte_buffer <<= (8 - stream->bit_count);

    if (stream->buffer_position >= stream->buffer_length)
        return ERROR_BUFFER_OUT_OF_BOUNDS;

    stream->buffer[stream->buffer_position++] = stream->byte_buffer;
    stream->byte_buffer = 0;
    stream->bit_count = 0;

    return ERROR_ALL_GOOD;
}

error_t bit_stream_read_bit(bit_stream_t *stream, uint8_t *bit)
{
    error_t error = ERROR_ALL_GOOD;

    if (stream->bit_count == 0)
        if ((error = bit_stream_unflush(stream)))
            return error;

    stream->bit_count -= 1;

    *bit = (stream->byte_buffer & (1 << (stream->bit_count))) > 0;

    return error;
}

error_t bit_stream_write_bit(bit_stream_t *stream, uint8_t bit)
{
    stream->byte_buffer <<= 1;

    stream->byte_buffer |= bit & 1;

    stream->bit_count += 1;

    if (stream->bit_count == 8)
        return bit_stream_flush(stream);

    return ERROR_ALL_GOOD;
}

error_t bit_stream_read_uint32(bit_stream_t *stream, uint32_t *number, uint8_t bits)
{
    error_t error = ERROR_ALL_GOOD;

    uint32_t value = 0;
    for (uint32_t i = 0; i < bits; i += 1)
    {
        value <<= 1;

        uint8_t bit = 0;

        if ((error = bit_stream_read_bit(stream, &bit)))
            return error;

        value |= bit & 1;
    }
    *number = value;

    return error;
}

error_t bit_stream_write_uint32(bit_stream_t *stream, uint32_t number, uint8_t bits)
{
    while (bits > 0)
    {
        uint32_t mask = 1 << (bits - 1);
        uint8_t bit = (number & mask) > 0;
        error_t error = bit_stream_write_bit(stream, bit);

        if (error != ERROR_ALL_GOOD)
            return error;

        bits -= 1;
    }

    return ERROR_ALL_GOOD;
}

// Reads an uint32 using 7-bit VLQ approach
error_t bit_stream_read_7bit_uint32(bit_stream_t *stream, uint32_t *number)
{
    error_t error = ERROR_ALL_GOOD;

    uint32_t n = 0;
    uint8_t shift = 0;
    while (1)
    {
        uint32_t byte = 0;
        if ((error = bit_stream_read_uint32(stream, &byte, 8)))
            return error;

        n |= (byte & 127) << shift;
        shift += 7;

        if ((byte & 128) == 0 || shift > 32)
            break;
    }
    *number = n;

    return error;
}

// Writes an uint32 using 7-bit VLQ approach
error_t bit_stream_write_7bit_uint32(bit_stream_t *stream, uint32_t number)
{
    error_t error = ERROR_ALL_GOOD;

    uint32_t n = number;
    // 127 = 7 bits
    while (n > 127)
    {
        uint32_t b = 128 | (n & 127); // Set the first bit as 1
        if ((error = bit_stream_write_uint32(stream, b, 8)))
            return error;

        n >>= 7;
    }

    // This check is probably not required.
    if (n > 0)
        bit_stream_write_uint32(stream, n & 127, 8);

    return error;
}

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

lzss_config_t lzss_config_init(uint8_t offset_bits, uint8_t length_bits, uint8_t minimum_length)
{
    return (lzss_config_t){
        .offset_bits = offset_bits,
        .max_offset = (1 << offset_bits) - 1,

        .minimum_length = minimum_length,
        .length_bits = length_bits,
        .max_length = (1 << length_bits) - 1,
    };
}

uint32_t lzss_get_upper_bound(uint32_t input_length)
{
    // We sum all bits in the worst case scenario: 32 for the total input length and input_lenght * 9 (literal flag + byte)
    uint32_t total_bits = 32 + input_length * 9;

    // If it's divisible by 8, we return the length. If not we sum 1 to account for the extra bits.
    return (total_bits / 8) + ((total_bits % 8 > 0) ? 1 : 0);
}

error_t lzss_get_original_length(array_t input, uint32_t *original_length)
{
    bit_stream_t stream = bit_stream_init(input);

    uint32_t length = 0;
    error_t error = bit_stream_read_7bit_uint32(&stream, &length);

    if (error)
    {
        *original_length = 0;
        return error;
    }

    *original_length = length;
    return error;
}

typedef struct match_t
{
    uint32_t offset;
    uint32_t length;
} match_t;

static inline match_t __get_longest_match(lzss_config_t config, array_t input, uint32_t index)
{
    if (index + config.minimum_length >= input.length)
        return (match_t){.offset = 0, .length = 0};

    uint32_t best_offset = 0, best_length = 0;
    uint32_t offset = (config.max_offset > index) ? 0 : index - config.max_offset;

    while (offset < index && offset < input.length)
    {
        uint32_t length = 0;

        while (
            offset + length < input.length &&
            index + length < input.length &&
            input.bytes[offset + length] == input.bytes[index + length])
        {
            length += 1;
        }

        // We compare greater or equal, since a lower offset is better
        if (length >= best_length)
        {
            best_length = length;
            best_offset = offset;
        }

        offset += 1;
    }

    // Substract the found offset from the actual index to get the resulting offset.
    return (match_t){.offset = index - best_offset, .length = MIN(best_length, config.max_length)};
}

#define try(fn)       \
    if ((error = fn)) \
        goto error_exit;

error_t lzss_encode(lzss_config_t config, array_t input, array_t *output)
{
    error_t error = ERROR_ALL_GOOD;

    // If there are no input bytes, we don't have to do anything.
    if (input.length == 0)
        return ERROR_NO_OP;

    bit_stream_t stream = bit_stream_init(*output);

    // Write the initial size of the buffer
    try(bit_stream_write_7bit_uint32(&stream, input.length)); // TODO: Maybe we should handle this total amount of symbols somewhere else?

    for (uint32_t index = 0; index < input.length;)
    {
        match_t match = __get_longest_match(config, input, index);

        if (match.length >= config.minimum_length)
        {
            try(bit_stream_write_bit(&stream, 1));
            try(bit_stream_write_uint32(&stream, match.offset, config.offset_bits));
            try(bit_stream_write_uint32(&stream, match.length, config.length_bits));
            index += match.length;
        }
        else
        {
            try(bit_stream_write_bit(&stream, 0));
            try(bit_stream_write_uint32(&stream, input.bytes[index], 8));
            index += 1;
        }
    }

    try(bit_stream_flush(&stream));

    goto no_error_exit;

error_exit:
    output->length = 0;
    return error;

no_error_exit:
    output->length = stream.buffer_position;
    return error;
}

error_t lzss_decode(lzss_config_t config, array_t input, array_t *output)
{
    error_t error = ERROR_ALL_GOOD;

    if (input.length == 0 || output->length == 0)
        return ERROR_NO_OP;

    bit_stream_t stream = bit_stream_init(input);

    uint32_t original_size = 0;
    try(bit_stream_read_7bit_uint32(&stream, &original_size));

    if (original_size != output->length)
        return ERROR_WRONG_OUTPUT_SIZE;

    for (uint32_t index = 0; index < output->length;)
    {
        uint8_t is_pair = 0;
        try(bit_stream_read_bit(&stream, &is_pair));

        if (is_pair)
        {
            uint32_t offset = 0;
            try(bit_stream_read_uint32(&stream, &offset, config.offset_bits));

            uint32_t length = 0;
            try(bit_stream_read_uint32(&stream, &length, config.length_bits));

            for (uint32_t i = 0; i < length; i += 1)
                output->bytes[index + i] = output->bytes[(index - offset) + i];

            index += length;
        }
        else
        {
            uint32_t literal = 0;
            try(bit_stream_read_uint32(&stream, &literal, 8));
            output->bytes[index] = (uint8_t)(literal & 0xFF);
            index += 1;
        }
    }

error_exit:
    return error;
}

#undef try

bool read_file(const char *file_name, array_t *buffer)
{
    FILE *file = fopen(file_name, "rb");
    if (file == NULL)
        return false;

    fseek(file, 0, SEEK_END);     // Seek to the end of the file
    buffer->length = ftell(file); // Get how many bytes the file contains
    fseek(file, 0, SEEK_SET);     // Rewind the file pointer to 0

    buffer->bytes = (uint8_t *)malloc(buffer->length);

    if (buffer->bytes == NULL)
    {
        fclose(file);
        return false;
    }

    uint32_t read_value = fread(buffer->bytes, sizeof(uint8_t), buffer->length, file);

    if (read_value != buffer->length)
    {
        free(buffer->bytes);
        buffer->bytes = NULL;
        buffer->length = 0;
        fclose(file);
        return false;
    }

    fclose(file);
    return true;
}

static error_t do_lzss_encoding(array_t input, array_t *output)
{
    lzss_config_t config = lzss_config_init(10, 6, 2);
    uint32_t output_upper_bound = lzss_get_upper_bound(input.length);

    output->bytes = (uint8_t *)malloc(output_upper_bound);
    output->length = output_upper_bound;

    if (output->bytes == NULL)
        return ERROR_COULD_NOT_ALLOCATE;

    return lzss_encode(config, input, output);
}

static error_t do_lzss_decoding(array_t input, array_t *output)
{
    error_t error = ERROR_ALL_GOOD;

    lzss_config_t config = lzss_config_init(10, 6, 2);

    uint32_t original_length = 0;
    if ((error = lzss_get_original_length(input, &original_length)))
        return error;

    output->bytes = (uint8_t *)malloc(original_length);
    output->length = original_length;

    return lzss_decode(config, input, output);
}

int main(int argc, const char **argv)
{
    if (argc != 2)
    {
        printf("Expected a filename\n");
        return 1;
    }

    const char *file_name = argv[1];

    array_t input = {0};
    if (!read_file(file_name, &input))
    {
        printf("Error reading file %s\n", file_name);
        return -1;
    }

    error_t error = ERROR_ALL_GOOD;

    array_t compressed = {0};
    if ((error = do_lzss_encoding(input, &compressed)) != ERROR_ALL_GOOD)
    {
        printf("Error encoding file %d\n", error);
        return -1;
    }

    array_t decompressed = {0};
    if ((error = do_lzss_decoding(compressed, &decompressed)) != ERROR_ALL_GOOD)
    {
        printf("Error decoding file %d\n", error);
        return -1;
    }

    if (memcmp(input.bytes, decompressed.bytes, MIN(input.length, decompressed.length)))
    {
        printf("Compression error\n");
        return 1;
    }

    return 0;
}