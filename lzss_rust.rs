struct BitWriter {
    buffer: Vec<u8>,
    byte: u8,
    bit_count: u8,
}

impl BitWriter {
    fn new(capacity: usize) -> BitWriter {
        return BitWriter {
            buffer: Vec::with_capacity(capacity),
            byte: 0,
            bit_count: 0,
        };
    }

    fn flush(&mut self) {
        if self.bit_count == 0 {
            return;
        }

        if self.bit_count < 8 {
            self.byte <<= 8 - self.bit_count;
        }

        self.buffer.push(self.byte);
        self.byte = 0;
        self.bit_count = 0;
    }

    fn write_bit(&mut self, bit: bool) {
        self.byte <<= 1;
        self.byte |= if bit { 1 } else { 0 };

        self.bit_count += 1;

        if self.bit_count == 8 {
            self.flush();
        }
    }

    fn write_u32(&mut self, number: u32, bits: u8) {
        let mut bits = bits;

        while bits > 0 {
            let mask = 1 << (bits - 1);
            let bit = (number & mask) > 0;

            self.write_bit(bit);

            bits -= 1;
        }
    }

    fn write_7bit_u32(&mut self, number: u32) {
        let mut n = number;

        while n > 127 {
            let b = 128 | (n & 127);

            self.write_u32(b, 8);

            n >>= 7;
        }

        if n > 0 {
            self.write_u32(n & 127, 8);
        }
    }
}

struct BitReader<'a> {
    buffer: &'a [u8],
    position: usize,
    byte: u8,
    bit_count: u8,
}

impl<'a> BitReader<'a> {
    fn new(buffer: &'a [u8]) -> BitReader {
        return BitReader {
            buffer,
            position: 0,
            byte: 0,
            bit_count: 0,
        };
    }

    fn unflush(&mut self) {
        self.byte = self.buffer[self.position];
        self.position += 1;
        self.bit_count = 8;
    }

    fn read_bit(&mut self) -> bool {
        if self.bit_count == 0 {
            self.unflush();
        }

        self.bit_count -= 1;

        return (self.byte & (1 << self.bit_count)) > 0;
    }

    fn read_u32(&mut self, bits: u8) -> u32 {
        let mut value: u32 = 0;

        for _ in 0..bits {
            value <<= 1;
            let bit = self.read_bit();
            value |= if bit { 1 } else { 0 };
        }

        return value;
    }

    fn read_7bit_u32(&mut self) -> u32 {
        let mut n: u32 = 0;
        let mut shift: u32 = 0;

        loop {
            let byte = self.read_u32(8);

            n |= (byte & 127) << shift;
            shift += 7;

            if (byte & 128) == 0 || shift > 32 {
                break;
            }
        }

        return n;
    }
}

#[derive(Clone, Copy)]
struct Lzss {
    offset_bits: u8,
    length_bits: u8,

    maximum_offset: u32,

    minimum_length: u32,
    maximum_length: u32,
}

fn lzss_get_upper_bound(input_length: usize) -> usize {
    let total_bits = 32 + input_length * 9;

    return (total_bits / 8) + if total_bits % 8 == 0 { 1 } else { 0 };
}

fn lzss_new(offset_bits: u8, length_bits: u8, minimum_length: u32) -> Lzss {
    return Lzss {
        offset_bits,
        length_bits,
        maximum_offset: (1 << offset_bits) - 1,
        minimum_length,
        maximum_length: (1 << length_bits) - 1,
    };
}

fn __lzss_get_longest_match<'a>(lzss: Lzss, input: &'a [u8], index: u32) -> (u32, u32) {
    let input_lenght = input.len() as u32;

    if index + lzss.minimum_length >= input_lenght {
        return (0, 0);
    }

    let mut best_offset: u32 = 0;
    let mut best_length: u32 = 0;
    let mut offset: u32 = if lzss.maximum_offset > index {
        0
    } else {
        index - lzss.maximum_offset
    };

    while offset < index && offset < input_lenght {
        let mut length: u32 = 0;

        while offset + length < input_lenght
            && index + length < input_lenght
            && input[(offset + length) as usize] == input[(index + length) as usize]
        {
            length += 1;
        }

        if length >= best_length {
            best_offset = offset;
            best_length = length;
        }

        offset += 1;
    }

    return (
        index - best_offset,
        std::cmp::min(best_length, lzss.maximum_length),
    );
}

fn lzss_encode<'a>(lzss: Lzss, input: &[u8]) -> Vec<u8> {
    let upper_bound = lzss_get_upper_bound(input.len());

    let mut writer = BitWriter::new(upper_bound);

    writer.write_7bit_u32(input.len() as u32);

    let mut index: u32 = 0;
    while index < input.len() as u32 {
        let _match = __lzss_get_longest_match(lzss, input, index);

        if _match.1 >= lzss.minimum_length {
            writer.write_bit(true);
            writer.write_u32(_match.0, lzss.offset_bits);
            writer.write_u32(_match.1, lzss.length_bits);
            index += _match.1;
        } else {
            writer.write_bit(false);
            writer.write_u32(input[index as usize] as u32, 8);
            index += 1;
        }
    }

    writer.flush();

    return writer.buffer;
}

fn lzss_decode<'a>(lzss: Lzss, input: &[u8]) -> Vec<u8> {
    let mut reader = BitReader::new(input);

    let original_length = reader.read_7bit_u32() as usize;

    let mut output: Vec<u8> = vec![0; original_length];

    let mut index = 0;
    while index < original_length {
        let is_pair = reader.read_bit();

        if is_pair {
            let offset = reader.read_u32(lzss.offset_bits) as usize;
            let length = reader.read_u32(lzss.length_bits) as usize;

            for i in 0..length {
                output[index + i] = output[(index - offset) + i];
            }

            index += length;
        } else {
            let literal = reader.read_u32(8) as u8;
            output[index] = literal;
            index += 1;
        }
    }

    return output;
}

fn main() {
    let args: Vec<String> = std::env::args().collect();

    if args.len() != 2 {
        println!("Expected a filename");
        std::process::exit(1);
    }

    let file = std::fs::read(&args[1]).expect("Could not read file");

    let lzss = lzss_new(10, 6, 2);

    let compressed = lzss_encode(lzss, file.as_slice());

    let uncompressed = lzss_decode(lzss, compressed.as_slice());

    if !file.iter().zip(uncompressed.iter()).all(|(a, b)| a == b) {
        println!("Compression failed!");
        std::process::exit(1);
    }
}
