/*
 * devicetree-parse.h
 * Brandon Azad
 */
#ifndef DEVICETREE_PARSE__H_
#define DEVICETREE_PARSE__H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef void (^devicetree_iterate_node_callback_t)(
		unsigned depth,
		const void *node, size_t size,
		unsigned n_properties, unsigned n_children,
		bool *stop);

typedef void (^devicetree_iterate_property_callback_t)(
		unsigned depth,
		const char *name,
		const void *value, size_t size,
		bool *stop);

bool devicetree_iterate(const void **data, size_t size,
		devicetree_iterate_node_callback_t node_callback,
		devicetree_iterate_property_callback_t property_callback);

bool devicetree_node_scan_properties(const void *node, size_t size,
		devicetree_iterate_property_callback_t property_callback);

#endif
