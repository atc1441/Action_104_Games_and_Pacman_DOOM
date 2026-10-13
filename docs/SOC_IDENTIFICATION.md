# SoC identification: the Aisinochip ACM32 register set

The SoC is still unmarked and its exact part number is unknown. Its
peripherals, though, match **Aisinochip's (上海航芯) ACM32 MCUs**: base
addresses, register offsets and bit positions agree with the public
Aisinochip SDKs for the **ACM32G103** and the **ACM32H5**. Those SDKs are
the closest thing to a datasheet this chip has.

This page lists what was compared, and where the chip differs from both
parts. It says nothing about what the chip is beyond that.

## Reference material

| | Source |
|---|---|
| ACM32G103 HAL SDK V1.1.6 | [aisinochip.com product page](https://www.aisinochip.com/index.php/product/detail/id/39.html), `ACM32G103_HAL_SDK_V1.1.6.zip`, header `Drivers/Device/acm32g103.h` |
| ACM32H5 HAL SDK | [github.com/ACM32-MCU/ACM32H5XX_HAL_SDK](https://github.com/ACM32-MCU/ACM32H5XX_HAL_SDK), header `Drivers/Device/acm32h5xx.h` |
| ACM32F4 (older generation) | RT-Thread `bsp/acm32/acm32f4xx-nucleo`, header `libraries/Device/ACM32F4.h` |
| Product range | [Aisinochip selection table, 2023-10-08](https://www.aisinochip.com/Uploads/2023/10/10/%E4%B8%8A%E6%B5%B7%E8%88%AA%E8%8A%AF%E4%BA%A7%E5%93%81%E9%80%89%E5%9E%8B%E8%A1%A8_20231008.pdf) |

The ACM32H5 datasheet names the STAR-MC1 as its core, the same core as
here. Both SDKs build against CMSIS `core_cm33.h`.

## Evidence in the dumps

**The stock 104 Games firmware leaks Aisinochip SDK paths.** From offset
`0x3E88D` in `dumps/action104/extflash_0x08000000_4MB.bin`:

```
../../Drivers/HAL_Driver/Src/hal_rcc.c
../../Drivers/HAL_Driver/Src/hal_exti.c
../../Drivers/HAL_Driver/Src/hal_gpio.c
../../Drivers/HAL_Driver/Src/hal_uart.c
```

The G103 and H5 SDKs both keep their drivers in `Drivers/HAL_Driver/Src/`,
with exactly these file names (lowercase `hal_*.c`). The older ACM32F4
SDK uses uppercase names (`HAL_UART.c`). The Pac-Man firmware
contains no such paths.

**The boot ROM names Aisinochip-style security features.** From offset `0xD54`
in `dumps/*/bootrom_0x00000000_32K.bin` (identical on both consoles):

```
1.5.1,0417
OTFDEC Enabled
PUF-Random
UID-Random
OTFDEC Disabled
App M1  App M4  App M3  App M2
```

OTFDEC (on-the-fly decryption of external flash) is listed as a feature of
exactly two ACM32 lines in the selection table: the G103 and the H5.

## Memory map

| Block | This SoC | ACM32G103 SDK | ACM32H5 SDK |
|---|---|---|---|
| RCC | `0x40021000` | `RCC_BASE_ADDR` `0x40021000` | `RCC_BASE_ADDR` `0x40021000` |
| GPIOA / B / C | `0x48000000` / `0400` / `0800` | same | same |
| LCD SPI | `0x40030000` | `SPI1_BASE_ADDR` | `SPI1_BASE_ADDR` |
| USART2 | `0x40004400` | `UART2_BASE_ADDR` | `USART2_BASE_ADDR` |
| USART1 | `0x40013800` | `UART1_BASE_ADDR` | `USART1_BASE_ADDR` |
| Audio block | `0x40012C00` | `TIM1_BASE_ADDR` | `TIM1_BASE_ADDR` |
| DMA controller | `0x40031000` | `DMA1_BASE_ADDR` | - (`0x40020000`) |
| Flash controller ("MPI") | `0x52005000` | - (QSPI at `0x90000000`) | `SPI7_BASE_ADDR` |
| XIP window | `0x08000000` | - (internal eFlash) | `SPI7_MEM_BASE_ADDR` |
| SRAM | `0x20000000` | `SRAM_BASE_ADDR` | `SRAM_BASE_ADDR` |

The `0x40012C00` row matters most: the audio block is TIM1, see
[Audio](#audio-tim1-pwm) below.

## Register layouts

Each offset below was established on the device first (see
[HARDWARE.md](HARDWARE.md) and `firmware/sdk/soc.h`). The SDK name is what
the Aisinochip headers call the same offset.

### GPIO

The layout is identical in both SDKs (`GPIO_TypeDef`).

| Offset | This project | SDK name |
|---|---|---|
| `+0x00` | MODER, 2 bits per pin | `MD` |
| `+0x04` | OTYPER (assumed) | `OTYP` |
| `+0x08` | pull config, `01` = pull-up | `PUPD` |
| `+0x0C` | input | `IDATA` |
| `+0x10` | output (assumed) | `ODATA` |
| `+0x14` | set low 16 / reset high 16 | `BSC` |
| `+0x18` | alternate function, pins 0-7 | `AF0` |
| `+0x1C` | alternate function, pins 8-15 (PA15 = 2 for audio) | `AF1` |
| `+0x20` | drive strength, **2 bits per pin, pins 0-15** | `DS0` (4 bits per pin, pins 0-7) |
| `+0x24` | not used by the stock HAL | `DS1` |
| `+0x28` | Schmitt input enable | `SMIT` |

The stock firmware's `HAL_GPIO_Init` (both the application's and the boot
ROM's copy, see [HARDWARE.md](HARDWARE.md#gpio)) confirms `AF0`, `AF1` and
`SMIT` as in the SDKs. It differs on drive strength: it packs 2 bits per pin into `DS0`
for all 16 pins (`GPIO_DRIVE_LEVEL0..3`), where the G103 SDK uses 4 bits
per pin across `DS0`/`DS1`. It also has no `AIEN` write.

The user manuals agree with the SDK, not with this chip: ACM32G103 UM
V2.0 section 8.4.10/8.4.11 and ACM32H5xx UM V1.5 section 10.4.10/10.4.11
both define `DS0`/`DS1` as 4 bits per pin, pins 0-7 and 8-15, with codes
`0000`-`0111` for eight drive levels. The disassembly leaves no doubt about
the stock layout: both HAL copies load `+0x20`, clear `3 << (pin * 2)` for
pins 0-15 and store it back, and the app HAL rejects drive values above 3.
The live GPIOA value `0xC03C0000` only makes sense that way: read as
2-bit fields it is drive 3 on PA9, PA10 and PA15, exactly the three pins
the stock firmware configures with drive 3. Read as 4-bit fields it would
put drive codes on PA4, PA5 and PA7, which are an LCD pin set to drive 0,
an input, and a pin that does not exist.

### USART

| Offset | This project | SDK name (both) |
|---|---|---|
| `+0x00` | data | `DR` |
| `+0x04` | status: bit 5 TX full, bit 9 TX busy | `FR`: bit 5 `TXFF`, bit 9 `BUSY` |
| `+0x08` | baud | `BRR` |
| `+0x14` | control: bit 0 enable, `0x300` TX+RX | `CR1` |

### SPI

Matches the G103's `SPI_TypeDef`.

| Offset | This project | SDK name (G103) |
|---|---|---|
| `+0x00` | data | `DAT` |
| `+0x04` | flash divider | `BAUD` |
| `+0x08` | mode | `CTL` |
| `+0x0C` | TX control | `TX_CTL` |
| `+0x10` | RX control | `RX_CTL` |
| `+0x18` | status: bit 0 busy, bit 3 TX full, bit 4 RX empty, bit 14 TX done | `STATUS`: bit 0 `TX_BUSY`, bit 3 `TX_FIFO_FULL`, bit 4 `RX_FIFO_EMPTY`, bit 14 `TX_BATCH_DONE` |
| `+0x20` | length | `BATCH` |
| `+0x24` | start | `CS` |
| `+0x2C` | manual / XIP switch | `MEMO_ACC` |
| `+0x30` | XIP read command | `CMD` |

In the H5 SDK, `+0x2C` and `+0x30` are `ALTER_BYTE` and `CS_TOUT_VAL`.

### DMA

Matches the G103 controller at the same base address, `0x40031000`.

| Offset | This project | SDK name (G103) |
|---|---|---|
| `+0x08` | write 1 to acknowledge | `INTTCCLR` |
| `+0x14` | bit 0 = channel 0 done | `RAWINTTCSTATUS` |
| `+0x1C` | 1 while streaming | `ENCHSTATUS` |
| `+0x30` | master enable | `CONFIG`, bit 0 `EN` |
| channel `+0x00..+0x10` | SRC, DST, NEXT, CTRL, CFG | `CXSRCADDR`, `CXDESTADDR`, `CXLLI`, `CXCTRL`, `CXCONFIG` |

Both SDKs place channel *n* at `base + 0x100 + n * 0x20`. `soc.h` uses a
`0x40` stride, so the LCD "channel 1" at `0x40031140` is the address the
SDKs call channel 2.

## Audio: TIM1 PWM

The block at `0x40012C00` is documented in [HARDWARE.md](HARDWARE.md) as a
DAC. In both SDKs that address is TIM1, and every value observed while the
stock firmware played sound decodes as an ordinary PWM timer. The bit
meanings below are the ones the G103 HAL (`hal_timer.c`, `hal_timer.h`,
`hal_timer_ex.h`) uses to set up PWM:

| Offset | Observed | TIM1 register | Meaning |
|---|---|---|---|
| `+0x00` | `0x00000081` | `CR1` | bit 0 CEN counter on, bit 7 ARPE auto-reload preload |
| `+0x0C` | `0x00000200` | `DIER` | bit 9: DMA request on capture/compare 1 |
| `+0x10` | `0x00000003` | `SR` | bit 0 UIF, bit 1 CC1IF (status flags) |
| `+0x14` | written at start | `EGR` | event generation |
| `+0x18` | `0x00000068` | `CCMR1` | OC1M = 6 (PWM mode 1) at bits 4-6, OC1PE preload at bit 3 |
| `+0x20` | `0x00000001` | `CCER` | bit 0: channel 1 output enable |
| `+0x24` | live counter | `CNT` | the counter itself |
| `+0x28` | `0x00000010` | `PSC` | prescaler, divide by 17 |
| `+0x2C` | `0x000000FF` | `ARR` | period 256 |
| `+0x34` | DMA target | `CCR1` | duty cycle of channel 1 |
| `+0x44` | `0x00008000` | `BDTR` | bit 15 MOE, main output enable |

This explains three things that were open questions:

* **The output is 8-bit PWM, not a 16-bit DAC.** With `ARR` = 255 the duty
  cycle has 256 steps. The stock samples are 8-bit values divided by a
  volume divisor, and the DMA writes each one into `CCR1`.
* **`+0x24` keeps changing with the CPU halted** because it is the timer
  counter.
* **The sample rate comes from the timer clock.** One DMA request per timer
  period gives a rate of `f_TIM / ((PSC + 1) * (ARR + 1))`, which is
  `f_TIM / 4352` with the stock `PSC` = 16 (the SDK examples set both
  registers as "value - 1"): 44.6 kHz at the stock 194 MHz. `+0x18` is
  `CCMR1`, not a divider.

## Where the SoC matches neither part

| | This SoC | ACM32G103 | ACM32H5 |
|---|---|---|---|
| Package | QFN48 | QFN32 / QFN48 / LQFP48-100 | LQFP100-176 |
| SRAM | 280 KB | 64 KB | 352 KB system + 64 KB TCM |
| Program memory | external SPI flash, XIP | 320 KB internal eFlash | internal/stacked SPI flash, XIP |
| Boot ROM | 32 KB at `0x00000000` | ROM at `0x12000000` | 32 KB, IROM at `0x1FF00000` |
| Max clock | stock firmware runs ~194 MHz | 120 MHz | 220 MHz |
| RCC layout | enables at `+0x28/+0x2C/+0x34/+0x38`, source `+0x1C`, PLL `+0x4C` | different | different |
| DMA base | `0x40031000` | `0x40031000` | `0x40020000` |

The RCC is the one block where neither SDK helps. The clock map in
[HARDWARE.md](HARDWARE.md#clock-tree) remains the only reference.

## Parts ruled out

Every series in the Aisinochip selection table was checked against the
table above:

* **ACM32F070, ACM32A070, ACM32WB15, ACM32FP001**: 64 MHz M0-class parts
  with 32 KB SRAM.
* **ACM32F403, ACM32F433, ACM32A403, ACM32FP401/402, ACM32FP421**: 180 MHz
  STAR-MC1, 128-192 KB SRAM. Older register generation: GPIO at
  `0x4001F000`, a PL011-style UART, and uppercase `HAL_*.c` files. The
  QFN48 members (ACM32F403CEU7, ACM32FP401CKU6) fail on registers.
* **ACM32G103, ACM32H5**: closest, but see the table above.

Other STAR-MC1 parts compared and ruled out on memory map: MindMotion
MM32F5277, Synwit SWM341/SWM330, SiFli SF32LB52x-58x.
