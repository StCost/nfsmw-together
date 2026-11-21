# Race script tools

The race / career flow of the game is Lua bytecode in `GLOBAL\gameplay.bin`, each handler compressed.
- `huff.c` / `b.bat`: a small program that runs the exe's own decoder (0x64DCA0) to decompress a handler.
- `gp.py`: pulls the compressed handlers out of gameplay.bin.
- `luadis.py`: disassembles the decompressed Lua bytecode.

Read the scripts before changing anything that ends races, pays rewards or moves the career on.
