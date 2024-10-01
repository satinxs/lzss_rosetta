const std = @import("std");
const Allocator = std.mem.Allocator;
const print = std.debug.print;

const BitStreamErrors = error{
    OutOfBounds,
};

const BitStream = struct {
    buffer: []u8,
    bufferPosition: usize,

    byteBuffer: u8,
    bitCount: u8,

    pub fn init(buffer: []u8) BitStream {
        return .{
            .buffer = buffer,
            .bufferPosition = 0,
            .byteBuffer = 0,
            .bitCount = 0,
        };
    }

    fn unflush(stream: *BitStream) !void {
        if (stream.bufferPosition < stream.buffer.len) {
            stream.byteBuffer = stream.buffer[stream.bufferPosition];
            stream.bufferPosition += 1;
            stream.bitCount = 8;
            return;
        }

        return BitStreamErrors.OutOfBounds;
    }

    fn flush(stream: *BitStream) !void {
        if (stream.bitCount == 0)
            return;

        if (stream.bitCount < 8)
            stream.byteBuffer <<= @as(u3, @intCast(8 - stream.bitCount));

        if (stream.bufferPosition >= stream.buffer.len)
            return BitStreamErrors.OutOfBounds;

        stream.buffer[stream.bufferPosition] = stream.byteBuffer;
        stream.bufferPosition += 1;
        stream.byteBuffer = 0;
        stream.bitCount = 0;
    }

    fn readBit(stream: *BitStream) !bool {
        if (stream.bitCount == 0)
            try stream.unflush();

        stream.bitCount -= 1;

        const bitMask = @as(usize, 1) <<| stream.bitCount;

        return (stream.byteBuffer & bitMask) > 0;
    }

    fn writeBit(stream: *BitStream, bit: bool) !void {
        stream.byteBuffer <<= 1;
        stream.byteBuffer |= if (bit) 1 else 0;

        stream.bitCount += 1;

        if (stream.bitCount == 8)
            try stream.flush();
    }

    fn readUint32(stream: *BitStream, bits: u8) !usize {
        var value: usize = 0;

        for (0..bits) |_| {
            value <<= 1;
            value |= if (try stream.readBit()) 1 else 0;
        }

        return value;
    }

    fn writeUint32(stream: *BitStream, number: usize, bits: u8) !void {
        var _bits = bits;
        while (_bits > 0) : (_bits -= 1) {
            const mask = @as(usize, 1) << @as(u6, @intCast(_bits)) - 1;
            const bit = (number & mask) > 0;
            try stream.writeBit(bit);
        }
    }

    // Reads an uint32 using 7-bit VLQ approach
    fn read7BitUint32(stream: *BitStream) !usize {
        var number: usize = 0;
        var shift: usize = 0;

        while (true) {
            const byte = try stream.readUint32(8);
            number |= (byte & 127) <<| shift;
            shift += 7;

            if ((byte & 128) == 0 or shift > 32)
                break;
        }

        return number;
    }

    // Writes an uint32 using 7-bit VLQ approach
    fn write7BitUint32(stream: *BitStream, number: usize) !void {
        var n = number;
        while (n > 127) {
            const b = 128 | (n & 127); //Set the first bit as 1

            try stream.writeUint32(b, 8);

            n >>= 7;
        }

        if (n > 0)
            try stream.writeUint32(n & 127, 8);
    }
};

fn getUpperBound(inputLength: usize) usize {
    // We sum all bits in the worst case scenario: 32 for the total input length and input_lenght * 9 (literal flag + byte)
    const totalBits = 32 + inputLength * 9;

    var upperBound = totalBits / 8;

    // If not divisible by 8 we sum 1 to account for the extra bits.
    if (totalBits % 8 > 0)
        upperBound += 1;

    return upperBound;
}

const Lzss = struct {
    allocator: Allocator,

    offsetBits: u8,
    lengthBits: u8,

    maximumOffset: usize,

    minimumLength: usize,
    maximumLength: usize,

    pub fn init(allocator: Allocator, offsetBits: u8, lengthBits: u8, minimumLength: usize) Lzss {
        return .{
            .allocator = allocator,
            .offsetBits = offsetBits,
            .lengthBits = lengthBits,

            .maximumOffset = (@as(usize, 1) <<| offsetBits) - 1,

            .minimumLength = minimumLength,
            .maximumLength = (@as(usize, 1) <<| lengthBits) - 1,
        };
    }

    const Match = struct { offset: usize, length: usize };

    fn getLongestMatch(lzss: *const Lzss, input: []u8, index: usize) Match {
        if (index + lzss.minimumLength >= input.len)
            return Match{ .offset = 0, .length = 0 };

        var bestOffset: usize = 0;
        var bestLength: usize = 0;
        var offset: usize = if (lzss.maximumOffset > index) 0 else index - lzss.maximumOffset;

        while (offset < index and offset < input.len) {
            var length: usize = 0;

            while (offset + length < input.len and index + length < input.len and input[offset + length] == input[index + length])
                length += 1;

            if (length >= bestLength) {
                bestLength = length;
                bestOffset = offset;
            }

            offset += 1;
        }

        return .{
            .offset = index - bestOffset,
            .length = @min(bestLength, lzss.maximumLength),
        };
    }

    fn encode(lzss: *const Lzss, input: []u8) ![]u8 {
        if (input.len == 0)
            return (error{NoOp}).NoOp;

        const output = try lzss.allocator.alloc(u8, getUpperBound(input.len));

        var stream = BitStream.init(output);

        try stream.write7BitUint32(input.len);

        var index: usize = 0;
        while (index < input.len) {
            const match = lzss.getLongestMatch(input, index);

            if (match.length >= lzss.minimumLength) {
                try stream.writeBit(true);
                try stream.writeUint32(match.offset, lzss.offsetBits);
                try stream.writeUint32(match.length, lzss.lengthBits);
                index += match.length;
            } else {
                try stream.writeBit(false);
                try stream.writeUint32(input[index], 8);
                index += 1;
            }
        }

        try stream.flush();

        //Slice the output array to the actual length & clone it
        return try lzss.allocator.realloc(output, stream.bufferPosition);
    }

    fn decode(lzss: *const Lzss, input: []u8) ![]u8 {
        if (input.len == 0)
            return (error{NoOp}).NoOp;

        var stream = BitStream.init(input);

        const originalSize = try stream.read7BitUint32();

        const output = try lzss.allocator.alloc(u8, originalSize);

        var index: usize = 0;
        while (index < originalSize) {
            const isPair = try stream.readBit();

            if (isPair) {
                const offset = try stream.readUint32(lzss.offsetBits);
                const length = try stream.readUint32(lzss.lengthBits);

                for (0..length) |i|
                    output[index + i] = output[(index - offset) + i];

                index += length;
            } else {
                const literal = try stream.readUint32(8);
                output[index] = @as(u8, @intCast(literal));
                index += 1;
            }
        }

        return output;
    }
};

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();

    const allocator = gpa.allocator();

    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    if (args.len != 2) {
        print("Expected a filename\n", .{});
        std.process.exit(1);
    }

    var file = try std.fs.cwd().openFile(args[1], .{});
    defer file.close();

    const input = try file.readToEndAlloc(allocator, std.math.maxInt(usize));
    defer allocator.free(input);

    const lzss = Lzss.init(allocator, 10, 6, 2);

    const compressed = try lzss.encode(input);
    defer allocator.free(compressed);

    const uncompressed = try lzss.decode(compressed);
    defer allocator.free(uncompressed);

    if (!std.mem.eql(u8, input, uncompressed)) {
        print("Assertion failed\n", .{});
        std.process.exit(1);
    }
}
