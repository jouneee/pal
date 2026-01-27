## Generate color palettes from images
My personal substitute for wal.

Specifically designed to be used with [pastel](https://github.com/sharkdp/pastel)

Requires [stb_image.h](https://github.com/nothings/stb) to compile.

## Usage of templates:
Template variables must be wrapped in `` and start with @. Allowed variables are: 
- @background 
- @foreground 
- @color0 ... @colorN.

Templates are taken from ~/.config/pal

Generated schemes are output to ~/.cache/pal  

A few template examples are provided in repo examples folder.
