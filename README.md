## ToumaPet emulator

This electronic toy uses 65C02 chip. Currently the emulator only support OK-550 and OK-560 models.

You need to dump the firmware using the programmer. To do this, you need to hold down the reset button so that the game does not turn on while the programmer is working.

### How to build the emulator

On Linux: `$ make X11=1`  
On Windows (using MSYS2): `$ make GDI=1`  

You can also build it with SDL1.2 or SDL2: `$ make SDL=1` or `$ make SDL=2`  

### How to run the game

```
$ ./toumapet --rom ok550.bin
```

* Use `--save <filename>` option to save game state on exit.
* Use `--update-time` option to update the game time with the system time.

### Controls

| Key(s)           | Action             |
|------------------|--------------------|
| A/Left           | left (select)      |
| S/Down           | middle (enter)     |
| D/Right          | right (back/menu)  |
| Q/Delete         | left side button   |
| E/PageDown       | right side button  |
| R                | reset the game     |

### What is not working

This is an early version of the emulator. There is no sound, there may be various bugs.
