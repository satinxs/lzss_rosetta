const fs = require('fs');
const path = require('path');

class BitStream {
    constructor(buffer) {
        this.buffer = buffer;
        this.position = 0;
        this.byteBuffer = 0;
        this.bitCount = 0;
    }

    unflush() {
        if (this.position < this.buffer.length) {
            this.byteBuffer = this.buffer[this.position++];
            this.bitCount = 8;
        } else
            throw new Error("Out of bounds");
    }

    flush() {
        if (this.bitCount === 0) return;
        if (this.bitCount < 8)
            this.byteBuffer <<= (8 - this.bitCount);
        if (this.position >= this.buffer.length)
            throw new Error("Out of bounds");

        this.buffer[this.position++] = this.byteBuffer & 0xFF;
        this.byteBuffer = 0;
        this.bitCount = 0;
    }

    readBit() {
        if (this.bitCount === 0)
            this.unflush();

        this.bitCount -= 1;
        return (this.byteBuffer & (1 << this.bitCount)) > 0;
    }

    writeBit(bit) {
        this.byteBuffer <<= 1;
        this.byteBuffer |= bit ? 1 : 0;

        this.bitCount += 1;
        if (this.bitCount === 8)
            this.flush();
    }

    readUint32(bits) {
        let value = 0;

        for (let i = 0; i < bits; i += 1) {
            value <<= 1;
            value |= this.readBit() ? 1 : 0;
        }

        return value;
    }

    writeUint32(number, bits) {
        while (bits > 0) {
            const mask = 1 << (bits - 1);
            const bit = (number & mask) > 0;

            this.writeBit(bit);

            bits -= 1;
        }
    }

    // Reads an uint32 using 7-bit VLQ approach
    read7BitUint32() {
        let [n, shift] = [0, 0];

        while (true) {
            const byte = this.readUint32(8);

            n |= (byte & 127) << shift;
            shift += 7;

            if ((byte & 128) === 0 || shift > 32)
                break;
        }

        return n;
    }

    write7BitUint32(number) {
        let n = number;

        //127 = 7 bits
        while (n > 127) {
            const b = 128 | (n & 127); //Set the first bit as 1
            this.writeUint32(b, 8);
            n >>= 7;
        }

        if (n > 0)
            this.writeUint32(n & 127, 8);
    }
}

const _match = (offset, length) => ({ offset, length });

class Lzss {
    constructor(offsetBits, lengthBits, minimumLength) {
        this.offsetBits = offsetBits;
        this.maxOffset = (1 << offsetBits) - 1;

        this.minimumLength = minimumLength;
        this.lengthBits = lengthBits;
        this.maxLength = (1 << lengthBits) - 1;
    }

    getUpperBound(inputLength) {
        const totalBits = 32 + inputLength * 9;
        return Math.ceil(totalBits / 8);
    }

    #getLongestMatch(input, index) {
        if (index + this.minimumLength >= input.length)
            return _match(0, 0);

        let bestOffset = 0, bestLength = 0;
        let offset = this.maxOffset > index ? 0 : index - this.maxOffset;

        while (offset < index && offset < input.length) {
            let length = 0;
            while (
                offset + length < input.length
                && index + length < input.length
                && input[offset + length] === input[index + length]
            )
                length += 1;

            //We compare greater or equal, since a lower offset is better
            if (length >= bestLength) {
                bestLength = length;
                bestOffset = offset;
            }

            offset += 1;
        }

        return _match(index - bestOffset, Math.min(bestLength, this.maxLength));
    }

    encode(input) {
        if (input.length === 0) return [];

        const output = new Uint8Array(this.getUpperBound(input.length));
        const stream = new BitStream(output);

        stream.write7BitUint32(input.length);

        for (let index = 0; index < input.length;) {
            const match = this.#getLongestMatch(input, index);

            if (match.length >= this.minimumLength) {
                stream.writeBit(1); //We write a bit flagging that this is a match
                stream.writeUint32(match.offset, this.offsetBits);
                stream.writeUint32(match.length, this.lengthBits);
                index += match.length;
            } else {
                stream.writeBit(0);
                stream.writeUint32(input[index] & 0xFF, 8);
                index += 1;
            }
        }

        stream.flush();
        return output.slice(0, stream.position);
    }

    decode(input) {
        if (input.length === 0) return;

        const stream = new BitStream(input);
        const originalLength = stream.read7BitUint32();
        const output = new Uint8Array(originalLength);

        if (originalLength !== output.length)
            throw new Error(`Expected output array to have length ${originalLength}`);

        for (let index = 0; index < output.length;) {
            const isPair = stream.readBit();

            if (isPair) {
                const offset = stream.readUint32(this.offsetBits);
                const length = stream.readUint32(this.lengthBits);

                for (let i = 0; i < length; i += 1)
                    output[index + i] = output[(index - offset) + i];

                index += length;
            } else {
                const literal = stream.readUint32(8);
                output[index] = literal & 0xFF;
                index += 1;
            }
        }

        return output;
    }
}

const arraysEqual = (a, b) => {
    if (a.length != b.length) return false;
    for (let i = 0; i < a.length; i++)
        if (a[i] != b[i])
            return false;
    return true;
};

if (process.argv.length != 3) {
    console.log('Expected filename');
    process.exit(1);
}

const input = new Uint8Array(fs.readFileSync(process.argv[2]));

const lzss = new Lzss(10, 6, 2);

const compressed = lzss.encode(input);
const uncompressed = lzss.decode(compressed);

if (!arraysEqual(input, uncompressed)) {
    console.log("Compression error");
    process.exit(1);
}
