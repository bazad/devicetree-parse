# devicetree-parse

This is a small utility to parse Apple's binary device tree format.

## Usage

Run devicetree-parse on a raw binary devicetree (i.e. not encrypted or wrapped in an IMG4 file).

	./devicetree-parse [-v] <devicetree-file>

By default, long data entries will be truncated to reduce clutter. Run with `-v` to show the full
value of every property.

## License

The devicetree-parse code is released into the public domain. As a courtesy I ask that if you
reference or use any of this code you attribute it to me.


---------------------------------------------------------------------------------------------------
Brandon Azad
