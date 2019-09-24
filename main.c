#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "devicetree-parse.h"

// ---- DeviceTree printing -----------------------------------------------------------------------

static bool verbose;

static uint64_t
read_uint(const void *data, size_t size) {
	switch (size) {
		case 1: return *(uint8_t  *)data;
		case 2: return *(uint16_t *)data;
		case 4: return *(uint32_t *)data;
		case 8: return *(uint64_t *)data;
		default: return -1;
	}
}

static bool
all_printable_ascii(const void *data, size_t size) {
	const uint8_t *p = data;
	const uint8_t *end = p + size;
	for (; p < end; p++) {
		if (!isprint(*p)) {
			return false;
		}
	}
	return true;
}

struct measure_string_info {
	// The number of printable characters.
	size_t printable;
	// The index of the first null, or the size of the string.
	size_t first_null;
	// The number of printable characters after the first null.
	size_t after_null;
	// The number of null bytes.
	size_t null_count;
	// The number of characters in a printable run of length 8 or more.
	size_t printable_run_count;
};

static void
measure_string(const void *data, size_t size, struct measure_string_info *string_info) {
	string_info->printable = 0;
	string_info->first_null = size;
	string_info->after_null = 0;
	string_info->null_count = 0;
	string_info->printable_run_count = 0;
	const uint8_t *bytes = (const uint8_t *)data;
	size_t current_printable_run = 0;
	for (size_t i = 0; i < size; i++) {
		uint8_t byte = bytes[i];
		if (byte == 0) {
			string_info->null_count++;
		}
		if (isprint(byte)) {
			string_info->printable++;
			current_printable_run++;
		} else {
			if (current_printable_run >= 8) {
				string_info->printable_run_count += current_printable_run;
			}
			current_printable_run = 0;
		}
		if (string_info->first_null != size && byte != 0) {
			string_info->after_null++;
		}
		if (byte == 0 && string_info->first_null == size) {
			string_info->first_null = i;
		}
	}
	if (current_printable_run >= 8) {
		string_info->printable_run_count += current_printable_run;
	}
}

static int
check_physaddr_length_pairs(const uint64_t *words, size_t count) {
	for (size_t i = 0; i < count; i += 2) {
		uint64_t physaddr = words[i];
		uint64_t length = words[i + 1];
		if (physaddr > 0x980000000) {
			return false;
		}
		if ((physaddr & 0x3fff) != 0) {
			return false;
		}
		if (length > 0x80000000) {
			return false;
		}
	}
	return true;
}

enum display_type {
	DISP_HEX_DUMP,
	DISP_HEX_INT,
	DISP_DEC_INT,
	DISP_STRING,
	DISP_HEX_STRING,
	DISP_FUNCTION_PROP,
	DISP_PHYSADDR_LEN,
};

static enum display_type
compute_display_type(const char *name, const void *data, size_t size) {
	const uint8_t *bytes = (const uint8_t *)data;
	if (size == 1 || size == 2) {
		return DISP_HEX_INT;
	}
	if (name[0] == '#') {
		return DISP_DEC_INT;
	}
	struct measure_string_info string;
	measure_string(data, size, &string);
	if (string.printable == string.first_null && string.after_null == 0) {
		if ((size != 4 && size != 8) || string.printable >= size - 1) {
			return DISP_STRING;
		}
	}
	bool function_prop = strncmp(name, "function-", strlen("function-")) == 0;
	if (function_prop && size >= 8 && size % 4 == 0) {
		bool has_ascii = all_printable_ascii(bytes + 4, 4);
		if (has_ascii) {
			return DISP_FUNCTION_PROP;
		}
	}
	if (string.printable >= 0.75 * size) {
		return DISP_HEX_STRING;
	}
	if (size > 0 && size % 16 == 0) {
		bool is_reg = strstr(name, "reg") != NULL;
		if (is_reg) {
			return DISP_PHYSADDR_LEN;
		}
		bool valid = check_physaddr_length_pairs((const uint64_t *)data, size / 8);
		if (valid) {
			return DISP_PHYSADDR_LEN;
		}
	}
	if (string.printable >= 2 && size >= 24
			&& string.printable + string.null_count >= 0.90 * size) {
		return DISP_HEX_STRING;
	}
	if (string.printable_run_count > 0 && size >= 24
			&& string.printable_run_count + string.null_count >= 0.6 * size) {
		return DISP_HEX_STRING;
	}
	if (size == 4 || size == 8) {
		return DISP_HEX_INT;
	}
	return DISP_HEX_DUMP;
}

struct strbuf {
	char *str;  // the char data; malloc'd
	size_t pos; // the current position in the data if we had infinite capacity
	size_t cap; // the capacity of str including trailing null
	size_t max; // the maximum we can grow to
};

static void
strbuf_alloc(struct strbuf *strbuf, size_t max) {
	size_t cap = 0x4000;
	if (cap > max) {
		cap = max;
	}
	strbuf->str = malloc(cap);
	assert(strbuf->str != NULL);
	strbuf->str[0] = 0;
	strbuf->pos = 0;
	strbuf->cap = cap;
	strbuf->max = max;
}

static void
strbuf_free(struct strbuf *strbuf) {
	free(strbuf->str);
	strbuf->str = NULL;
	strbuf->pos = strbuf->cap = strbuf->max = 0;
}

static bool
strbuf_vprintf_no_grow(struct strbuf *strbuf, const char *fmt, va_list ap,
		size_t *req_cap) {
	assert(strbuf->cap <= strbuf->max);
	size_t size = strbuf->cap - strbuf->pos;
	if (strbuf->pos >= strbuf->cap - 1 || strbuf->cap == 0) {
		// We're already full, would need to grow.
		size = 0;
	}
	int ret = vsnprintf(strbuf->str + strbuf->pos, size, fmt, ap);
	size_t new_pos = strbuf->pos + ret;
	if (new_pos > strbuf->cap - 1) {
		// We overflowed. Don't reset the str back to usual just yet.
		*req_cap = new_pos + 1;
		return false;
	}
	strbuf->pos = new_pos;
	return true;
}

static bool
strbuf_printf(struct strbuf *strbuf, const char *fmt, ...) {
	va_list ap, ap2;
	va_start(ap, fmt);
	va_copy(ap2, ap);
	size_t req_cap;
	bool ok = strbuf_vprintf_no_grow(strbuf, fmt, ap, &req_cap);
	va_end(ap);
	if (!ok) {
		if (req_cap > strbuf->max) {
			// No available capacity. We did the write above, so just set pos.
			strbuf->pos = req_cap - 1;
			assert(strbuf->pos >= strbuf->cap);
			return false;
		}
		// We have room to grow.
		char *new_str = realloc(strbuf->str, req_cap);
		assert(new_str != NULL);
		strbuf->str = new_str;
		strbuf->cap = req_cap;
		// Try again.
		ok = strbuf_vprintf_no_grow(strbuf, fmt, ap2, &req_cap);
		assert(ok);
	}
	va_end(ap2);
	return true;
}

static bool
print_property_hex_dump(struct strbuf *sb, const void *data, size_t size) {
	const uint8_t *p = data;
	const uint8_t *end = p + size;
	const uint8_t *print_end = end;
	bool ok = true;
	for (; ok && p < print_end; p++) {
		const char *sep = " ";
		if (p == print_end - 1) {
			sep = (print_end == end ? "" : "...");
		}
		ok = strbuf_printf(sb, "%02x%s", *p, sep);
	}
	return ok;
}

static bool
print_property_hex_int(struct strbuf *sb, const void *data, size_t size) {
	uint64_t value = read_uint(data, size);
	if (value == 0) {
		return strbuf_printf(sb, "0");
	}
	return strbuf_printf(sb, "0x%llx", value);
}

static bool
print_property_dec_int(struct strbuf *sb, const void *data, size_t size) {
	return strbuf_printf(sb, "%lld", read_uint(data, size));
}

static bool
print_property_hex_string(struct strbuf *sb, const void *data, size_t size) {
	const char *p = data;
	const char *end = p + size;
	bool ok = strbuf_printf(sb, "\"");
	for (; ok && p < end; p++) {
		uint8_t c = *p;
		if (isprint(c)) {
			ok = strbuf_printf(sb, "%c", c);
		} else if (c == '"') {
			ok = strbuf_printf(sb, "\\\"");
		} else if (c == 0) {
			ok = strbuf_printf(sb, "\\0");
		} else {
			ok = strbuf_printf(sb, "\\x%02x", (unsigned)c);
		}
	}
	if (ok) {
		ok = strbuf_printf(sb, "\"");
	}
	return ok;
}

static bool
print_property_string(struct strbuf *sb, const void *data, size_t size) {
	assert(size > 0);
	size_t len = strnlen((const char *)data, size);
	return print_property_hex_string(sb, data, len);
}

static bool
print_property_function(struct strbuf *sb, const void *data, size_t size) {
	return print_property_hex_string(sb, data, size);
}

static bool
print_property_physaddr_len(struct strbuf *sb, const void *data, size_t size) {
	assert(size % 16 == 0 && size > 0);
	const uint64_t *p = data;
	bool ok = true;
	for (size_t i = 0; ok && i < size / 16; i++) {
		bool end = i == size / 16 - 1;
		ok = strbuf_printf(sb, "0x%016llx,%llx%s", p[2 * i], p[2 * i + 1],
				(end ? "" : "; "));
	}
	return ok;
}

static bool
print_property(struct strbuf *sb, const char *name, const void *value, size_t size) {
	enum display_type disp = compute_display_type(name, value, size);
	switch (disp) {
		default: // DISP_HEX_DUMP
			return print_property_hex_dump(sb, value, size);
		case DISP_HEX_INT:
			return print_property_hex_int(sb, value, size);
		case DISP_DEC_INT:
			return print_property_dec_int(sb, value, size);
		case DISP_STRING:
			return print_property_string(sb, value, size);
		case DISP_HEX_STRING:
			return print_property_hex_string(sb, value, size);
		case DISP_FUNCTION_PROP:
			return print_property_function(sb, value, size);
		case DISP_PHYSADDR_LEN:
			return print_property_physaddr_len(sb, value, size);
	}
}

static void
print_indent(unsigned depth) {
	for (size_t i = 0; i < 4 * depth; i++) {
		printf(" ");
	}
}

static bool
devicetree_print(const void *data, size_t size) {
	__block const char *node_name;
	__block struct strbuf sb;
	strbuf_alloc(&sb, verbose ? -1 : 64);
	devicetree_iterate_property_callback_t find_node_name_cb =
			^void(unsigned depth, const char *name,
					const void *value, size_t size, bool *stop) {
		if (strcmp(name, "name") == 0) {
			node_name = (const char *)value;
		}
	};
	devicetree_iterate_node_callback_t node_cb =
			^void(unsigned depth, const void *node, size_t size,
					unsigned n_properties, unsigned n_children, bool *stop) {
		bool ok = devicetree_node_scan_properties(node, size, find_node_name_cb);
		if (!ok) {
			node_name = "NODE";
		}
		print_indent(depth);
		printf("%s:\n", node_name);
	};
	devicetree_iterate_property_callback_t property_cb =
			^void(unsigned depth, const char *name,
					const void *value, size_t size, bool *stop) {
		print_indent(depth);
		printf("%s (%zu)%s", name, size, (size > 0 ? ": " : ""));
		if (size > 0) {
			sb.pos = 0;
			bool complete = print_property(&sb, name, value, size);
			printf("%s%s", sb.str, complete ? "" : "...");
		}
		printf("\n");
	};
	const void *processed = data;
	bool ok = devicetree_iterate(&processed, size, node_cb, property_cb);
	strbuf_free(&sb);
	return (ok && (processed == (uint8_t *)data + size));
}

// ---- devicetree-parse tool ---------------------------------------------------------------------

static bool
mmap_file(const char *path, void **data, size_t *size) {
	bool success = false;
	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open");
		goto fail_0;
	}
	struct stat st;
	int err = fstat(fd, &st);
	if (err != 0) {
		perror("fstat");
		goto fail_1;
	}
	size_t filesize = st.st_size;
	*size = filesize;
	void *mapped = mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapped == MAP_FAILED) {
		perror("mmap");
		goto fail_1;
	}
	*data = mapped;
	success = true;
fail_1:
	close(fd);
fail_0:
	return success;
}

int
main(int argc, const char *argv[]) {
	if (argc != 2 && argc != 3) {
usage:
		printf("usage: %s [-v] <devicetree-file>\n", getprogname());
		return 1;
	}
	const char *file = argv[1];
	if (argc == 3) {
		if (strcmp(argv[1], "-v") != 0) {
			goto usage;
		}
		verbose = true;
		file = argv[2];
	}
	void *data;
	size_t size;
	bool ok = mmap_file(file, &data, &size);
	if (!ok) {
		return 2;
	}
	ok = devicetree_print(data, size);
	return (!ok ? 3 : 0);
}
