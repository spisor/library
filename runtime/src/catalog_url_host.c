#include "catalog_url_host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog_text.h"

#define IPV6_PIECES 8
#define IPV4_PARTS 4
#define TOO_LARGE 0x100000000ull

static int hex_value(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

static int is_digit(char character) {
    return character >= '0' && character <= '9';
}

static char *percent_decoded(const char *bytes, size_t length, size_t *decoded_length) {
    char *decoded = malloc(length + 1);
    if (decoded == NULL) {
        return NULL;
    }
    size_t filled = 0;
    for (size_t index = 0; index < length; index++) {
        if (bytes[index] == '%' && index + 2 < length && hex_value(bytes[index + 1]) >= 0 &&
            hex_value(bytes[index + 2]) >= 0) {
            decoded[filled++] = (char)(hex_value(bytes[index + 1]) * 16 + hex_value(bytes[index + 2]));
            index += 2;
        } else {
            decoded[filled++] = bytes[index];
        }
    }
    decoded[filled] = 0;
    *decoded_length = filled;
    return decoded;
}

static int is_forbidden_domain_byte(unsigned char byte) {
    return byte <= 0x20 || byte == 0x7F || strchr("#%/:<>?@[\\]^|", byte) != NULL;
}

/** An IPv4 part in decimal, 0x hex or leading-zero octal; -1 when it is none, TOO_LARGE past 32 bits. */
static long long ipv4_number(const char *part, size_t length) {
    if (length == 0) {
        return -1;
    }
    int radix = 10;
    if (length >= 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X')) {
        radix = 16;
        part += 2;
        length -= 2;
    } else if (length >= 2 && part[0] == '0') {
        radix = 8;
        part += 1;
        length -= 1;
    }
    unsigned long long value = 0;
    for (size_t index = 0; index < length; index++) {
        int digit = hex_value(part[index]);
        if (digit < 0 || digit >= radix) {
            return -1;
        }
        value = value * (unsigned long long)radix + (unsigned long long)digit;
        if (value >= TOO_LARGE) {
            value = TOO_LARGE;
        }
    }
    return (long long)value;
}

typedef struct host_parts {
    const char *starts[IPV4_PARTS + 2];
    size_t lengths[IPV4_PARTS + 2];
    size_t count;
    int overflowed;
} host_parts;

static void split_on_dots(const char *host, size_t length, host_parts *parts) {
    parts->count = 0;
    parts->overflowed = 0;
    size_t start = 0;
    for (size_t index = 0; index <= length; index++) {
        if (index == length || host[index] == '.') {
            if (parts->count == IPV4_PARTS + 2) {
                parts->overflowed = 1;
                return;
            }
            parts->starts[parts->count] = host + start;
            parts->lengths[parts->count++] = index - start;
            start = index + 1;
        }
    }
    if (parts->count > 1 && parts->lengths[parts->count - 1] == 0) {
        parts->count--;
    }
}

static int ends_in_a_number(const char *host, size_t length) {
    size_t last_end = length > 0 && host[length - 1] == '.' ? length - 1 : length;
    size_t last_start = last_end;
    while (last_start > 0 && host[last_start - 1] != '.') {
        last_start--;
    }
    if (last_end == last_start) {
        return 0;
    }
    int digits = 1;
    for (size_t index = last_start; index < last_end; index++) {
        digits = digits && is_digit(host[index]);
    }
    return digits || ipv4_number(host + last_start, last_end - last_start) >= 0;
}

static char *ipv4_host(const char *host, size_t length) {
    host_parts parts;
    split_on_dots(host, length, &parts);
    if (parts.overflowed || parts.count > IPV4_PARTS) {
        return NULL;
    }
    long long numbers[IPV4_PARTS];
    for (size_t index = 0; index < parts.count; index++) {
        numbers[index] = ipv4_number(parts.starts[index], parts.lengths[index]);
        if (numbers[index] < 0 || (index + 1 < parts.count && numbers[index] > 255)) {
            return NULL;
        }
    }
    unsigned long long limit = 1;
    for (size_t power = 0; power < 5 - parts.count; power++) {
        limit *= 256;
    }
    if ((unsigned long long)numbers[parts.count - 1] >= limit) {
        return NULL;
    }
    unsigned long long address = (unsigned long long)numbers[parts.count - 1];
    for (size_t index = 0; index + 1 < parts.count; index++) {
        address += (unsigned long long)numbers[index] << (8 * (3 - index));
    }
    char *serialized = malloc(16);
    if (serialized != NULL) {
        snprintf(serialized, 16, "%u.%u.%u.%u", (unsigned)(address >> 24) & 255, (unsigned)(address >> 16) & 255,
                 (unsigned)(address >> 8) & 255, (unsigned)address & 255);
    }
    return serialized;
}

static int ipv6_embedded_ipv4(const char *input, size_t length, size_t *pointer, uint16_t *address,
                              int *piece_index) {
    int numbers_seen = 0;
    while (*pointer < length) {
        int piece = -1;
        if (numbers_seen > 0) {
            if (input[*pointer] != '.' || numbers_seen >= 4) {
                return 0;
            }
            (*pointer)++;
        }
        if (*pointer >= length || !is_digit(input[*pointer])) {
            return 0;
        }
        while (*pointer < length && is_digit(input[*pointer])) {
            int number = input[*pointer] - '0';
            if (piece == 0) {
                return 0;
            }
            piece = piece < 0 ? number : piece * 10 + number;
            if (piece > 255) {
                return 0;
            }
            (*pointer)++;
        }
        address[*piece_index] = (uint16_t)(address[*piece_index] * 0x100 + piece);
        numbers_seen++;
        if (numbers_seen == 2 || numbers_seen == 4) {
            (*piece_index)++;
        }
    }
    return numbers_seen == 4;
}

static int ipv6_pieces(const char *input, size_t length, uint16_t *address, int *piece_index, int *compress) {
    size_t pointer = 0;
    if (length > 0 && input[0] == ':') {
        if (length < 2 || input[1] != ':') {
            return 0;
        }
        pointer = 2;
        *compress = ++(*piece_index);
    }
    while (pointer < length) {
        if (*piece_index == IPV6_PIECES) {
            return 0;
        }
        if (input[pointer] == ':') {
            if (*compress >= 0) {
                return 0;
            }
            pointer++;
            *compress = ++(*piece_index);
            continue;
        }
        unsigned value = 0;
        size_t digits = 0;
        while (digits < 4 && pointer < length && hex_value(input[pointer]) >= 0) {
            value = value * 16 + (unsigned)hex_value(input[pointer++]);
            digits++;
        }
        if (pointer < length && input[pointer] == '.') {
            if (digits == 0 || *piece_index > 6) {
                return 0;
            }
            pointer -= digits;
            return ipv6_embedded_ipv4(input, length, &pointer, address, piece_index);
        }
        if (pointer < length && input[pointer] == ':') {
            if (++pointer == length) {
                return 0;
            }
        } else if (pointer < length) {
            return 0;
        }
        address[(*piece_index)++] = (uint16_t)value;
    }
    return 1;
}

static int parse_ipv6(const char *input, size_t length, uint16_t *address) {
    memset(address, 0, IPV6_PIECES * sizeof *address);
    int piece_index = 0;
    int compress = -1;
    if (!ipv6_pieces(input, length, address, &piece_index, &compress)) {
        return 0;
    }
    if (compress < 0) {
        return piece_index == IPV6_PIECES;
    }
    int swaps = piece_index - compress;
    piece_index = IPV6_PIECES - 1;
    while (piece_index != 0 && swaps > 0) {
        uint16_t swapped = address[piece_index];
        address[piece_index] = address[compress + swaps - 1];
        address[compress + swaps - 1] = swapped;
        piece_index--;
        swaps--;
    }
    return 1;
}

static int longest_zero_run(const uint16_t *address) {
    int best = -1;
    int best_length = 1;
    for (int start = 0; start < IPV6_PIECES; start++) {
        int length = 0;
        while (start + length < IPV6_PIECES && address[start + length] == 0) {
            length++;
        }
        if (length > best_length) {
            best = start;
            best_length = length;
        }
    }
    return best;
}

static char *ipv6_host(const char *input, size_t length) {
    uint16_t address[IPV6_PIECES];
    if (!parse_ipv6(input, length, address)) {
        return NULL;
    }
    int compress = longest_zero_run(address);
    runtime_catalog_text text;
    runtime_catalog_text_start(&text);
    runtime_catalog_text_add_character(&text, '[');
    int ignoring_zero = 0;
    for (int index = 0; index < IPV6_PIECES; index++) {
        if (ignoring_zero && address[index] == 0) {
            continue;
        }
        ignoring_zero = 0;
        if (compress == index) {
            runtime_catalog_text_add(&text, index == 0 ? "::" : ":");
            ignoring_zero = 1;
            continue;
        }
        char piece[8];
        snprintf(piece, sizeof piece, "%x", (unsigned)address[index]);
        runtime_catalog_text_add(&text, piece);
        if (index != IPV6_PIECES - 1) {
            runtime_catalog_text_add_character(&text, ':');
        }
    }
    runtime_catalog_text_add_character(&text, ']');
    return runtime_catalog_text_finish(&text);
}

static char *domain_host(const char *bytes, size_t length) {
    size_t decoded_length;
    char *domain = percent_decoded(bytes, length, &decoded_length);
    if (domain == NULL) {
        return NULL;
    }
    int usable = decoded_length > 0;
    for (size_t index = 0; usable && index < decoded_length; index++) {
        unsigned char byte = (unsigned char)domain[index];
        if (byte >= 0x80 || is_forbidden_domain_byte(byte)) {
            usable = 0;
        } else if (byte >= 'A' && byte <= 'Z') {
            domain[index] = (char)(byte + ('a' - 'A'));
        }
    }
    if (!usable) {
        free(domain);
        return NULL;
    }
    if (!ends_in_a_number(domain, decoded_length)) {
        return domain;
    }
    char *address = ipv4_host(domain, decoded_length);
    free(domain);
    return address;
}

char *runtime_catalog_parse_host(const char *bytes, size_t length) {
    if (length > 0 && bytes[0] == '[') {
        if (length < 2 || bytes[length - 1] != ']') {
            return NULL;
        }
        return ipv6_host(bytes + 1, length - 2);
    }
    return domain_host(bytes, length);
}
