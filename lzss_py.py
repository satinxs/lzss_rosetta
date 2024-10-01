class BitWriter:
    def __init__(self):
        self.buffer = []
        self.byte = 0
        self.bitCount = 0

    def flush(self):
        if self.bitCount == 0:
            return
        
        if self.bitCount < 8:
            self.byte <<= (8 - self.bitCount)
        
        self.buffer.append(self.byte)
        self.byte = 0
        self.bitCount = 0
    
    def writeBit(self, bit):
        self.byte <<= 1
        self.byte |= 1 if bit else 0
        self.bitCount += 1

        if self.bitCount == 8:
            self.flush()
    
    def writeUint32(self, n, bits):
        while bits > 0:
            mask = 1 << (bits - 1)
            bit = (n & mask) > 0
            self.writeBit(bit)
            bits -= 1

    def write7BitUint32(self, n):
        while n > 127:
            b = 128 | (n & 127)
            self.writeUint32(b, 8)
            n >>= 7
        
        if n > 0:
            self.writeUint32(n & 127, 8)

class BitReader:
    def __init__(self, buffer):
        self.buffer = buffer
        self.position = 0
        self.byte = 0
        self.bitCount = 0
    
    def unflush(self):
        if self.position < len(self.buffer):
            self.byte = self.buffer[self.position]
            self.position += 1
            self.bitCount = 8
        else:
            raise "Out of bounds read access"

    def readBit(self):
        if self.bitCount == 0:
            self.unflush()

        self.bitCount -= 1
        return (self.byte & (1 << self.bitCount)) > 0

    def readUint32(self, bits):
        n = 0
        for _ in range(bits):
            n <<= 1
            bit = self.readBit()
            n |= 1 if bit else 0
        
        return n

    def read7BitUint32(self):
        n = 0
        shift = 0
        while True:
            byte = self.readUint32(8)
            n |= (byte & 127) << shift
            shift += 7

            if (byte & 128) == 0 or shift > 32:
                break

        return n

class Lzss:
    def __init__(self, offsetBits, lengthBits, minimumLength):
        self.offsetBits = offsetBits
        self.lengthBits = lengthBits

        self.maximumOffset = (1 << offsetBits) - 1
        
        self.minimumLength = minimumLength
        self.maximumLength = (1 << lengthBits) - 1
    
    def _get_longest_match(self, input, index):
        if index + self.minimumLength >= len(input):
            return (0,0)

        bestOffset = 0
        bestLength = 0
        offset = 0 if (self.maximumOffset > index) else index - self.maximumOffset

        while offset < index and offset < len(input):
            length = 0

            while (offset + length < len(input) 
               and index + length < len(input)
               and input[offset + length] == input[index + length]
            ):
                length += 1

            if length >= bestLength:
                bestOffset = offset
                bestLength = length

            offset +=1

        return (index - bestOffset, min(bestLength, self.maximumLength))

    def encode(self, input):
        if len(input) == 0: return []

        writer = BitWriter()
        writer.write7BitUint32(len(input))

        index = 0
        while index < len(input):
            _match = self._get_longest_match(input, index)

            if _match[1] >= self.minimumLength:
                writer.writeBit(True)
                writer.writeUint32(_match[0], self.offsetBits)
                writer.writeUint32(_match[1], self.lengthBits)
                index += _match[1]
            else:
                writer.writeBit(False)
                writer.writeUint32(input[index], 8)
                index += 1

        writer.flush()

        return writer.buffer
    
    def decode(self, input):
        reader = BitReader(input)
        originalLength = reader.read7BitUint32()
        output = []

        index = 0
        while index < originalLength:
            isPair = reader.readBit()
            if isPair:
                offset = reader.readUint32(self.offsetBits)
                length = reader.readUint32(self.lengthBits)
                for i in range(0, length):
                    output.append(output[(index - offset) + i])
                index += length
            else:
                literal = reader.readUint32(8)
                output.append(literal)
                index += 1

        return output

import sys

if len(sys.argv) != 2:
    print("Expected a filename")
    sys.exit(1)

input = open(sys.argv[1], "rb").read()

lzss = Lzss(10, 6, 2)

compressed = lzss.encode(input)

uncompressed = lzss.decode(compressed)

if not all(x == y for x, y in zip(input, uncompressed)):
    print("Compression failed")
    sys.exit(1)

