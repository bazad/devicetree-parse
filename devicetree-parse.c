/*
 * devicetree-parse.c
 * Brandon Azad
 */
#include "devicetree-parse.h"

#include <assert.h>

struct devicetree_node {
	uint32_t n_properties;
	uint32_t n_children;
};

struct devicetree_property {
	char name[32];
	uint32_t size;
	uint8_t data[0];
};

static bool
devicetree_iterate_node(const void **data, const void *data_end,
		unsigned depth, bool *stop,
		devicetree_iterate_node_callback_t node_callback,
		devicetree_iterate_property_callback_t property_callback) {
	assert(!*stop);
	const uint8_t *p = *data;
	const uint8_t *end = (const uint8_t *)data_end;
	// We start by parsing the node header.
	struct devicetree_node *node = (struct devicetree_node *)p;
	p += sizeof(*node);
	if (p > end) {
		return false;
	}
	uint32_t n_properties = node->n_properties;
	uint32_t n_children   = node->n_children;
	// If we have a node callback, call it.
	if (node_callback != NULL) {
		node_callback(depth, (const void *)node, end - p + sizeof(*node),
				n_properties, n_children, stop);
		if (*stop) {
			return true;
		}
	}
	// Iterate through all the node's properties.
	for (size_t i = 0; i < n_properties; i++) {
		// Parse out property header.
		struct devicetree_property *prop = (struct devicetree_property *)p;
		p += sizeof(*prop);
		if (p > end) {
			return false;
		}
		// Make sure that the property name is null-terminated.
		if (prop->name[sizeof(prop->name) - 1] != 0) {
			return false;
		}
		// Properties are padded to a multiple of 4 bytes. There also appears to be a flag
		// field (bit 31) which is set if iBoot should replace the value of the field with
		// a syscfg property or other value. (We do not see this flag for device trees
		// dumped from kernel memory.)
		uint32_t prop_size = prop->size & ~0x80000000;
		size_t padded_size = (prop_size + 0x3) & ~0x3;
		p += padded_size;
		if (p > end) {
			if (p - padded_size + prop_size == end) {
				// We're at the very end, ease up on the lack of padding.
				p = end;
			} else {
				return false;
			}
		}
		// If we have a property callback, invoke it.
		if (property_callback != NULL) {
			property_callback(depth + 1, prop->name, prop->data, prop_size, stop);
			if (*stop) {
				return true;
			}
		}
	}
	// Now that we've finished the properties, update the pointer to the head of the data.
	*data = p;
	// Iterate through all the node's children recursively.
	for (size_t i = 0; i < n_children; i++) {
		bool ok = devicetree_iterate_node(data, end, depth + 1, stop,
				node_callback, property_callback);
		if (!ok) {
			return false;
		}
		if (*stop) {
			return true;
		}
	}
	return true;
}

bool
devicetree_iterate(const void **data, size_t size,
		devicetree_iterate_node_callback_t node_callback,
		devicetree_iterate_property_callback_t property_callback) {
	const void *end = (const uint8_t *)*data + size;
	bool stop = false;
	return devicetree_iterate_node(data, end, 0, &stop, node_callback, property_callback);
}

bool
devicetree_node_scan_properties(const void *node, size_t size,
		devicetree_iterate_property_callback_t property_callback) {
	devicetree_iterate_node_callback_t do_not_scan_children =
			^void(unsigned depth, const void *node, size_t size,
					unsigned n_properties, unsigned n_children, bool *stop) {
		if (depth != 0) {
			*stop = true;
		}
	};
	return devicetree_iterate(&node, size, do_not_scan_children, property_callback);
}
