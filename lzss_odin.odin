package main

import "base:runtime"
import "core:bytes"
import "core:fmt"
import "core:os"

lzss_errors :: enum {
	AllGood,
	OutOfBounds,
	NoOp,
	AllocationFailure,
}

bit_stream_t :: struct {
	buffer:          []u8,
	buffer_position: u32,
	byte_buffer:     u8,
	bit_count:       u8,
}

bit_stream_init :: proc(buffer: []u8) -> bit_stream_t {
	return bit_stream_t{buffer = buffer, buffer_position = 0, bit_count = 0, byte_buffer = 0}
}

bit_stream_unflush :: proc(stream: ^bit_stream_t) -> lzss_errors {
	if stream.buffer_position < u32(len(stream.buffer)) {
		stream.byte_buffer = stream.buffer[stream.buffer_position]
		stream.buffer_position += 1
		stream.bit_count = 8
		return .AllGood
	}

	return .OutOfBounds
}

bit_stream_flush :: proc(stream: ^bit_stream_t) -> lzss_errors {
	if stream.bit_count == 0 {
		return .AllGood
	}

	if stream.bit_count < 8 {
		stream.byte_buffer <<= (8 - stream.bit_count)
	}

	if stream.buffer_position >= u32(len(stream.buffer)) {
		return .OutOfBounds
	}

	stream.buffer[stream.buffer_position] = stream.byte_buffer
	stream.buffer_position += 1
	stream.byte_buffer = 0
	stream.bit_count = 0

	return .AllGood
}

bit_stream_read_bit :: proc(stream: ^bit_stream_t) -> (bit: bool, error: lzss_errors) {
	error = .AllGood

	if stream.bit_count == 0 {
		bit_stream_unflush(stream) or_return
	}

	stream.bit_count -= 1
	bit = (stream.byte_buffer & (1 << stream.bit_count)) > 0
	return bit, error
}

bit_stream_write_bit :: proc(stream: ^bit_stream_t, bit: bool) -> lzss_errors {
	stream.byte_buffer <<= 1
	stream.byte_buffer |= bit ? 1 : 0
	stream.bit_count += 1

	if stream.bit_count == 8 {
		bit_stream_flush(stream) or_return
	}

	return .AllGood
}

bit_stream_read_uint32 :: proc(stream: ^bit_stream_t, bits: u8) -> (n: u32, error: lzss_errors) {
	error = .AllGood

	n = 0
	for i: u8 = 0; i < bits; i += 1 {
		n <<= 1
		bit := bit_stream_read_bit(stream) or_return
		n |= bit ? 1 : 0
	}

	return n, error
}

bit_stream_write_uint32 :: proc(stream: ^bit_stream_t, number: u32, bits: u8) -> lzss_errors {
	_bits := bits
	for _bits > 0 {
		mask: u32 = 1 << (_bits - 1)
		bit := (number & mask) > 0

		bit_stream_write_bit(stream, bit) or_return
		_bits -= 1
	}

	return .AllGood
}

// Reads an uint32 using 7-bit VLQ approach
bit_stream_read_7bit_uint32 :: proc(stream: ^bit_stream_t) -> (number: u32, error: lzss_errors) {
	error = .AllGood
	number = 0
	shift: u32 = 0
	for {
		b := bit_stream_read_uint32(stream, 8) or_return
		number |= (b & 127) << shift
		shift += 7

		if (b & 128) == 0 || shift > 32 {
			break
		}
	}

	return number, error
}

// Writes an uint32 using 7-bit VLQ approach
bit_stream_write_7bit_uint32 :: proc(stream: ^bit_stream_t, number: u32) -> lzss_errors {
	n: u32 = number
	for n > 127 {
		b: u32 = 128 | (n & 127)
		bit_stream_write_uint32(stream, b, 8) or_return
		n >>= 7
	}

	if n > 0 {
		bit_stream_write_uint32(stream, n & 127, 8) or_return
	}

	return .AllGood
}

lzss_t :: struct {
	offset_bits, length_bits:       u8,
	maximum_offset:                 u32,
	minimum_length, maximum_length: u32,
}

lzss_init :: proc(offset_bits, length_bits: u8, minimum_length: u32) -> lzss_t {
	return lzss_t {
		offset_bits = offset_bits,
		length_bits = length_bits,
		maximum_offset = (1 << offset_bits) - 1,
		minimum_length = minimum_length,
		maximum_length = (1 << length_bits) - 1,
	}
}

lzss_get_upper_bound :: proc(input_length: u32) -> u32 {
	// We sum all bits in the worst case scenario: 32 for the total input length and input_lenght * 9 (literal flag + byte)
	total_bits: u32 = 32 + input_length * 9

	// If it's divisible by 8, we return the length. If not we sum 1 to account for the extra bits.
	return (total_bits / 8) + ((total_bits % 8 > 0) ? 1 : 0)
}

match_t :: struct {
	offset, length: u32,
}

__get_longest_match :: proc(lzss: lzss_t, input: []u8, index: u32) -> match_t {
	input_length: u32 = u32(len(input))

	if index + lzss.minimum_length >= input_length {
		return match_t{offset = 0, length = 0}
	}

	best_offset: u32 = 0
	best_length: u32 = 0
	offset: u32 = (lzss.maximum_offset > index) ? 0 : index - lzss.maximum_offset

	for offset < index && offset < input_length {
		length: u32 = 0

		for offset + length < input_length &&
		    index + length < input_length &&
		    input[offset + length] == input[index + length] {
			length += 1
		}

		if length >= best_length {
			best_length = length
			best_offset = offset
		}

		offset += 1
	}

	return match_t{offset = index - best_offset, length = min(best_length, lzss.maximum_length)}
}

lzss_encode :: proc(lzss: lzss_t, input: []u8) -> (output: []u8, error: lzss_errors) {
	input_length: u32 = u32(len(input))
	if input_length == 0 {
		return nil, .NoOp
	}

	buffer, err := make_slice([]u8, lzss_get_upper_bound(input_length))
	if err != .None {
		return nil, .AllocationFailure
	}
	output = buffer

	stream := bit_stream_init(output)
	bit_stream_write_7bit_uint32(&stream, input_length) or_return

	for index: u32 = 0; index < input_length; {
		match := __get_longest_match(lzss, input, index)

		if match.length >= lzss.minimum_length {
			bit_stream_write_bit(&stream, true) or_return
			bit_stream_write_uint32(&stream, match.offset, lzss.offset_bits) or_return
			bit_stream_write_uint32(&stream, match.length, lzss.length_bits) or_return
			index += match.length
		} else {
			bit_stream_write_bit(&stream, false) or_return
			bit_stream_write_uint32(&stream, u32(input[index]), 8) or_return
			index += 1
		}
	}

	bit_stream_flush(&stream) or_return
	output = output[0:stream.buffer_position]

	return output, error
}

lzss_decode :: proc(lzss: lzss_t, input: []u8) -> (output: []u8, error: lzss_errors) {
	input_length: u32 = u32(len(input))
	if input_length == 0 {
		return nil, .NoOp
	}

	stream := bit_stream_init(input)
	original_length: u32 = bit_stream_read_7bit_uint32(&stream) or_return
	buffer, err := make_slice([]u8, original_length)
	if err != .None {
		return nil, .AllocationFailure
	}
	output = buffer

	for index: u32 = 0; index < original_length; {
		is_pair := bit_stream_read_bit(&stream) or_return
		if (is_pair) {
			offset := bit_stream_read_uint32(&stream, lzss.offset_bits) or_return
			length := bit_stream_read_uint32(&stream, lzss.length_bits) or_return

			for i: u32 = 0; i < length; i += 1 {
				output[index + i] = output[(index - offset) + i]
			}

			index += length
		} else {
			literal := bit_stream_read_uint32(&stream, 8) or_return
			output[index] = u8(literal)
			index += 1
		}
	}

	return output, .AllGood
}

main :: proc() {
	if len(os.args) != 2 {
		fmt.println("Expected a filename")
		os.exit(1)
	}

	input, success := os.read_entire_file(os.args[1])

	if !success {
		panic("Could not read file")
	}

	lzss := lzss_init(10, 6, 2)

	compressed := lzss_encode(lzss, input) or_else panic("Could not encode")

	uncompressed := lzss_decode(lzss, compressed) or_else panic("Could not decode")

	if !bytes.equal(input, uncompressed) {
		panic("Compression error")
	}
}
