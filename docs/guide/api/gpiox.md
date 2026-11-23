# GPIO expanders

This page covers the PCA9554 and PCA9554A I2C GPIO expanders. Each chip adds eight pins on the I2C bus that [`i2c.h`](./buses.md#i2c-master-i2ch) drives. Return codes and context rules follow the [Conventions](../API.md#conventions).

## PCA9554 expanders (`gpiox.h`)

`gpiox.h` drives up to four chips. The caller gives each chip a table index from 0 to 3 with `gpiox_attach()`. The driver allocates no memory. It needs `i2c_init()` first.

The chip has four registers. A configuration bit of 1 makes the pin an input, 0 an output. At power-on every pin is an input, the output register holds all ones and the polarity register holds zero. A pin set as an output reads back its own level in the input register.

| Constant | Value | Meaning |
|---|---|---|
| `GPIOX_MAX_CHIPS` | `4` | Table size |
| `GPIOX_PINS` | `8` | Pins per chip |
| `GPIOX_REG_INPUT` | `0` | Input port register |
| `GPIOX_REG_OUTPUT` | `1` | Output port register |
| `GPIOX_REG_POLARITY` | `2` | Polarity inversion register |
| `GPIOX_REG_CONFIG` | `3` | Configuration register |
| `GPIOX_ADDR_MIN`, `GPIOX_ADDR_MAX` | `0x20`, `0x27` | PCA9554 addresses |
| `GPIOX_ADDR_A_MIN`, `GPIOX_ADDR_A_MAX` | `0x38`, `0x3f` | PCA9554A addresses |
| `GPIOX_IN`, `GPIOX_OUT` | `0`, `1` | Pin direction |
| `GPIOX_NAME_SIZE` | `6` | Buffer size for `gpiox_pin_name()` |
| `GPIOX_ERR_ARG` | `-30` | Bad chip index, pin, direction, level or NULL |
| `GPIOX_ERR_ADDR` | `-31` | Address outside both ranges |
| `GPIOX_ERR_ABSENT` | `-32` | The chip did not acknowledge the probe |
| `GPIOX_ERR_BUS` | `-33` | Any other I2C failure, including a missing `i2c_init()` |
| `GPIOX_ERR_BUSY` | `-34` | The index is attached, or another index holds the address |
| `GPIOX_ERR_DETACHED` | `-35` | The index is not attached |
| `GPIOX_ERR_DIR` | `-36` | A write to a pin that is not an output |

```c
int gpiox_attach(unsigned chip, uint8_t addr);
int gpiox_detach(unsigned chip);
int gpiox_present(unsigned chip);
```

`gpiox_attach()` probes the chip with a read of the configuration register. No acknowledge gives `GPIOX_ERR_ABSENT`. It then reads the output and polarity registers. It keeps the output and configuration registers in a shadow copy and never writes them, so a chip that already drives relays sees no glitch when a program starts. If the polarity register is not zero, it writes zero: the driver's levels are pin levels, and an inverted input bit would break that. A chip with polarity zero sees no write at all. Any later bus failure gives `GPIOX_ERR_BUS` and leaves the index detached.

`gpiox_detach()` forgets the chip and leaves its pins as they are. `gpiox_present()` returns 1 for an attached index, else 0.

```c
int gpiox_config(unsigned chip, unsigned pin, int dir, int level);
int gpiox_write(unsigned chip, unsigned pin, int level);
int gpiox_write_port(unsigned chip, uint8_t mask, uint8_t bits);
```

`gpiox_config()` makes a pin an input or an output. For an output it writes the output register first and the configuration register second, so the pin never drives the wrong level. Each write is the whole register built from the shadow, so the other pins keep their state. It skips a write that would change nothing. For an input, `level` is ignored and only the configuration register changes.

`gpiox_write()` sets one output pin. `gpiox_write_port()` sets the pins in `mask` to the matching bits of `bits` in one bus write. Both return `GPIOX_ERR_DIR` when a pin is not an output, before any bus transfer.

The shadow changes only after the chip acknowledges a write. If the second write of `gpiox_config()` fails, the shadow holds the new output register and the old configuration register, as the chip does.

```c
int gpiox_read(unsigned chip, unsigned pin, int *level);
int gpiox_read_port(unsigned chip, uint8_t *bits);
int gpiox_shadow(unsigned chip, uint8_t *output, uint8_t *config);
const char *gpiox_strerror(int code);
```

`gpiox_read_port()` reads the input port register. `gpiox_read()` reads it and returns one bit. For an output pin this is the level the chip reports on the pin, which can differ from the output register when something outside holds the line. `gpiox_shadow()` copies the shadow registers for diagnostics. `gpiox_strerror()` gives a short text for a `GPIOX_ERR_*` code.

### Pure helpers

```c
int gpiox_addr_valid(unsigned addr);
int gpiox_pin_valid(unsigned chip, unsigned pin);
int gpiox_pin_name(unsigned chip, unsigned pin, char *buf, size_t size);
```

`gpiox_addr_valid()` returns 1 for an address in either range. `gpiox_pin_valid()` returns 1 when the index and pin are in range. It does not check that the chip is attached. `gpiox_pin_name()` writes names such as `X0.3` for chip 0, pin 3, and returns the length. It needs `GPIOX_NAME_SIZE` bytes. A program that mixes Pi pins and expander pins can build its own pin numbers on these two helpers.

```c
int gpiox_plan_config(const gpiox_regs_t *s, unsigned pin, int dir, int level,
                      gpiox_plan_t *plan);
int gpiox_plan_write(const gpiox_regs_t *s, uint8_t mask, uint8_t bits, gpiox_plan_t *plan);
void gpiox_regs_apply(gpiox_regs_t *s, uint8_t reg, uint8_t val);
```

The shadow logic, in `src/lib/gpiox_shadow.c`, runs on the host. `gpiox_regs_t` holds the output and configuration registers. A `gpiox_plan_t` lists up to two register writes in order. `gpiox_regs_apply()` records one acknowledged write.

### Not done

- The chip's interrupt output. Poll `gpiox_read_port()` instead.
- The 16-bit parts such as the PCA9555 and TCA9535.
- Polarity inversion: the driver keeps it at zero.

### QEMU

QEMU's `pca9554` model sits on `i2c-bus.1` at any address. `tests/target/test_gpiox.py` reads output pins through QMP `qom-get` and drives input pins through qtest. Canary V17 in [tests/README.md](../../../tests/README.md#canary-facts) lists what the model does. The driver is not validated on hardware.
