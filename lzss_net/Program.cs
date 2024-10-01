using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Lzss;

public class BitStream(byte[] buffer)
{
    private int position = 0;
    private byte byteBuffer = 0;
    private byte bitCount = 0;

    public void Unflush()
    {
        if (position < buffer.Length)
        {
            byteBuffer = buffer[position++];
            bitCount = 8;
        }
        else
            throw new IndexOutOfRangeException();
    }

    public void Flush()
    {
        if (bitCount == 0) return;

        if (bitCount < 8)
            byteBuffer <<= 8 - bitCount;

        if (position >= buffer.Length)
            throw new IndexOutOfRangeException();

        buffer[position++] = byteBuffer;
        byteBuffer = 0;
        bitCount = 0;
    }

    public bool ReadBit()
    {
        if (bitCount == 0) Unflush();
        bitCount -= 1;
        return (byteBuffer & (1 << bitCount)) > 0;
    }

    public void WriteBit(bool bit)
    {
        byteBuffer <<= 1;
        byteBuffer |= (byte)(bit ? 1 : 0);
        bitCount += 1;

        if (bitCount == 8) Flush();
    }

    public uint ReadUint32(byte bits)
    {
        uint value = 0;
        for (var i = 0; i < bits; i++)
        {
            value <<= 1;
            value |= (byte)(ReadBit() ? 1 : 0);
        }

        return value;
    }

    public void WriteUint32(uint number, byte bits)
    {
        while (bits > 0)
        {
            var mask = 1 << (bits - 1);
            var bit = (number & mask) > 0;

            WriteBit(bit);
            bits -= 1;
        }
    }

    // Reads an uint32 using 7-bit VLQ approach
    public uint Read7BitUint32()
    {
        uint n = 0;
        var shift = 0;
        while (true)
        {
            var b = ReadUint32(8);
            n |= (b & 127) << shift;
            shift += 7;

            if ((b & 128) == 0 || shift > 32)
                break;
        }

        return n;
    }

    // Writes an uint32 using 7-bit VLQ approach
    public void Write7BitUint32(uint number)
    {
        while (number > 127)
        {
            var b = 128 | (number & 127);
            WriteUint32(b, 8);
            number >>= 7;
        }

        if (number > 0)
            WriteUint32(number & 127, 8);
    }

    public byte[] ToArray() => buffer[0..position];
}

public class LzssEncoder(byte offsetBits, byte lengthBits, byte minimumLength)
{
    private readonly uint maximumOffset = (uint)(1 << offsetBits) - 1;
    private readonly uint maximumLength = (uint)(1 << lengthBits) - 1;

    private uint GetUpperBound(uint inputLength)
    {
        var totalBits = 32 + inputLength * 9;
        return (totalBits / 8) + (uint)((totalBits % 8) > 0 ? 1 : 0);
    }

    private (uint offset, uint length) GetLongestMatch(byte[] input, uint index)
    {
        if (index + minimumLength >= input.Length)
            return (0, 0);

        uint bestOffset = 0, bestLength = 0;
        uint offset = (maximumOffset > index) ? 0 : index - maximumOffset;

        while (offset < index && offset < input.Length)
        {
            uint length = 0;
            while (
                offset + length < input.Length
                && index + length < input.Length
                && input[offset + length] == input[index + length]
            )
                length += 1;

            if (length >= bestLength)
            {
                bestOffset = offset;
                bestLength = length;
            }

            offset += 1;
        }

        return (index - bestOffset, Math.Min(bestLength, maximumLength));
    }

    public byte[] Encode(byte[] input)
    {
        if (input.Length == 0) return [];

        var upperBound = GetUpperBound((uint)input.Length);
        var output = new byte[upperBound];
        var stream = new BitStream(output);

        stream.Write7BitUint32((uint)input.Length);
        for (uint index = 0; index < input.Length;)
        {
            var (offset, length) = GetLongestMatch(input, index);
            if (length >= minimumLength)
            {
                stream.WriteBit(true);
                stream.WriteUint32(offset, offsetBits);
                stream.WriteUint32(length, lengthBits);
                index += length;
            }
            else
            {
                stream.WriteBit(false);
                stream.WriteUint32(input[index], 8);
                index += 1;
            }
        }

        stream.Flush();

        return stream.ToArray();
    }

    public byte[] Decode(byte[] input)
    {
        if (input.Length == 0) return [];

        var stream = new BitStream(input);
        var originalLength = stream.Read7BitUint32();
        var output = new byte[originalLength];

        for (uint index = 0; index < originalLength;)
        {
            var isPair = stream.ReadBit();
            if (isPair)
            {
                var offset = stream.ReadUint32(offsetBits);
                var length = stream.ReadUint32(lengthBits);

                for (var i = 0; i < length; i++)
                    output[index + i] = output[index - offset + i];

                index += length;
            }
            else
            {
                var literal = stream.ReadUint32(8);
                output[index] = (byte)literal;
                index += 1;
            }
        }

        return output;
    }
}

public static class Program
{
    public static void Main(string[] args)
    {
        if (args.Length != 1)
        {
            Console.WriteLine("Expected a filename");
            Environment.Exit(1);
        }

        var input = File.ReadAllBytes(args[0]);

        var lzss = new LzssEncoder(10, 6, 2);

        var compressed = lzss.Encode(input);

        var uncompressed = lzss.Decode(compressed);

        if (!Enumerable.SequenceEqual(input, uncompressed))
        {
            Console.WriteLine("Compression error\n");
            Environment.Exit(1);
        }
    }
}