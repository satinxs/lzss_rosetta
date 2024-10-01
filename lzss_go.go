package main

import (
	"errors"
	"fmt"
	"math"
	"os"
)

// Silly silly Go
func ternary[T any](condition bool, a T, b T) T {
	if condition {
		return a
	}

	return b
}

type bitStream struct {
	buffer         []byte
	bufferLength   uint32
	bufferPosition uint32
	byteBuffer     byte
	bitCount       byte
}

func (b *bitStream) unflush() error {
	if b.bufferPosition < b.bufferLength {
		b.byteBuffer = b.buffer[b.bufferPosition]
		b.bufferPosition += 1
		b.bitCount = 8

		return nil
	}

	return errors.New("Out of bounds")
}

func (b *bitStream) flush() error {
	if b.bitCount == 0 {
		return nil
	}

	if b.bitCount < 8 {
		b.byteBuffer <<= (8 - b.bitCount)
	}

	if b.bufferPosition >= b.bufferLength {
		return errors.New("Out of bounds")
	}

	b.buffer[b.bufferPosition] = b.byteBuffer
	b.bufferPosition += 1
	b.byteBuffer = 0
	b.bitCount = 0

	return nil
}

func (b *bitStream) readBit() (bool, error) {
	if b.bitCount == 0 {
		err := b.unflush()
		if err != nil {
			return false, err
		}
	}

	b.bitCount -= 1
	return (b.byteBuffer & (1 << b.bitCount)) > 0, nil
}

func (b *bitStream) writeBit(bit bool) error {
	b.byteBuffer <<= 1
	b.byteBuffer |= ternary[byte](bit, 1, 0)

	b.bitCount += 1
	if b.bitCount == 8 {
		return b.flush()
	}

	return nil
}

func (b *bitStream) readUint32(bits byte) (uint32, error) {
	value := uint32(0)

	for i := byte(0); i < bits; i += 1 {
		value <<= 1
		bit, err := b.readBit()
		if err != nil {
			return 0, err
		}
		value |= ternary[uint32](bit, 1, 0)
	}

	return value, nil
}

func (b *bitStream) writeUint32(number uint32, bits byte) error {
	for bits > 0 {
		mask := uint32(1 << (bits - 1))
		bit := (number & mask) > 0

		err := b.writeBit(bit)
		if err != nil {
			return err
		}

		bits -= 1
	}

	return nil
}

func (b *bitStream) read7BitUint32() (uint32, error) {
	number := uint32(0)
	shift := uint32(0)

	for {
		by, err := b.readUint32(8)
		if err != nil {
			return 0, err
		}

		number |= (by & 127) << shift
		shift += 7

		if (by&128) == 0 || shift > 32 {
			break
		}
	}

	return number, nil
}

func (b *bitStream) write7BitUint32(number uint32) error {
	//127 = 7 bits
	for number > 127 {
		by := 128 | (number & 127) //Set the first bit as 1
		err := b.writeUint32(by, 8)
		if err != nil {
			return err
		}

		number >>= 7
	}

	if number > 0 {
		return b.writeUint32(number&127, 8)
	}

	return nil
}

type Lzss struct {
	offsetBits byte
	lengthBits byte

	maxOffset uint32

	minimumLength uint32
	maximumLength uint32
}

func NewLzss(offsetBits, lengthBits byte, minimumLength uint32) Lzss {
	return Lzss{
		offsetBits: offsetBits,
		lengthBits: lengthBits,

		maxOffset: (1 << offsetBits) - 1,

		minimumLength: minimumLength,
		maximumLength: (1 << lengthBits) - 1,
	}
}

func (l *Lzss) GetUpperBound(inputLength uint32) uint32 {
	totalBits := 32 + inputLength*9
	return uint32(math.Ceil(float64(totalBits) / 8))
}

func (l *Lzss) GetOriginalLength(input []byte) (uint32, error) {
	stream := bitStream{buffer: input, bufferLength: uint32(len(input))}
	return stream.read7BitUint32()
}

type match struct {
	offset, length uint32
}

func (l *Lzss) getLongestMatch(input []byte, index uint32) match {
	inputLength := uint32(len(input))

	if index+l.minimumLength >= inputLength {
		return match{}
	}

	bestOffset := uint32(0)
	bestLength := uint32(0)
	offset := ternary(l.maxOffset > index, 0, index-l.maxOffset)

	for offset < index && offset < inputLength {
		length := uint32(0)

		for offset+length < inputLength && index+length < inputLength && input[offset+length] == input[index+length] {
			length += 1
		}

		if length >= bestLength {
			bestLength = length
			bestOffset = offset
		}

		offset += 1
	}

	return match{
		offset: index - bestOffset,
		length: ternary(bestLength > l.maximumLength, l.maximumLength, bestLength),
	}
}

func (l *Lzss) Encode(input []byte) ([]byte, error) {
	inputLength := uint32(len(input))

	if inputLength == 0 {
		return []byte{}, nil
	}

	output := make([]byte, l.GetUpperBound(inputLength))
	stream := bitStream{buffer: output, bufferLength: uint32(len(output))}

	err := stream.write7BitUint32(inputLength)
	if err != nil {
		return nil, err
	}

	for index := uint32(0); index < inputLength; {
		match := l.getLongestMatch(input, index)
		if match.length >= l.minimumLength {
			err = stream.writeBit(true) //We write a bit flagging that this is a match
			if err != nil {
				return nil, err
			}
			err = stream.writeUint32(match.offset, l.offsetBits)
			if err != nil {
				return nil, err
			}
			err = stream.writeUint32(match.length, l.lengthBits)
			if err != nil {
				return nil, err
			}
			index += match.length
		} else {
			err = stream.writeBit(false)
			if err != nil {
				return nil, err
			}
			err = stream.writeUint32(uint32(input[index]), 8)
			if err != nil {
				return nil, err
			}
			index += 1
		}
	}

	err = stream.flush()
	if err != nil {
		return nil, err
	}

	//Return only the relevant slice
	return output[:stream.bufferPosition], nil
}

func (l *Lzss) Decode(input []byte) ([]byte, error) {
	inputLength := uint32(len(input))

	if inputLength == 0 {
		return []byte{}, nil
	}

	stream := bitStream{buffer: input, bufferLength: inputLength}
	originalLength, err := stream.read7BitUint32()
	if err != nil {
		return nil, err
	}
	output := make([]byte, originalLength)

	for index := uint32(0); index < originalLength; {
		isPair, err := stream.readBit()
		if err != nil {
			return nil, err
		}

		if isPair {
			offset, err := stream.readUint32(l.offsetBits)
			if err != nil {
				return nil, err
			}
			length, err := stream.readUint32(l.lengthBits)
			if err != nil {
				return nil, err
			}

			for i := uint32(0); i < length; i += 1 {
				output[index+i] = output[(index-offset)+i]
			}
			index += length
		} else {
			literal, err := stream.readUint32(8)
			if err != nil {
				return nil, err
			}
			output[index] = byte(literal)
			index += 1
		}
	}

	return output, nil
}

func main() {
	if len(os.Args) != 2 {
		fmt.Println("Was expecting a filename as argument")
		return
	}

	fileName := os.Args[1]

	input, err := os.ReadFile(fileName)
	if err != nil {
		panic(err)
	}

	lzss := NewLzss(10, 6, 2)

	compressed, err := lzss.Encode(input)
	if err != nil {
		panic(err)
	}

	uncompressed, err := lzss.Decode(compressed)
	if err != nil {
		panic(err)
	}

	for i, b := range uncompressed {
		if b != input[i] {
			fmt.Printf("Byte at %d does not match!\n", i)
			os.Exit(-1)
		}
	}
}
