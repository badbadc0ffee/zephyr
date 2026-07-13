.. zephyr:board:: g0b0cet6

Overview
********

Custom board based on the STMicroelectronics STM32G0B0CET6, an Arm Cortex-M0+
value-line MCU with USB 2.0 full-speed support.

This board configuration was created primarily to bring up USB device support
on the STM32G0B0. See `USB and clocking`_ below for the design rationale.

Hardware
********

The STM32G0B0CET6 provides the following hardware:

- Arm 32-bit Cortex-M0+ CPU, 64 MHz max
- LQFP48 package
- 512 KB Flash
- 144 KB SRAM
- USB 2.0 full-speed device and host controller (1)
- USART/LPUART, I2C, SPI/I2S
- 12-bit ADC, timers, DMA
- GPIO with external interrupt capability

.. note::

   Unlike the STM32G0B1, the STM32G0B0 **value line has no CRS** (Clock Recovery
   System). See `USB and clocking`_.

More information about the SoC can be found here:

- `STM32G0B0 on www.st.com`_
- `STM32G0B0 reference manual`_

Supported Features
==================

.. zephyr:board-supported-hw::

USB and clocking
================

USB full-speed requires a 48 MHz clock with ±0.25 % accuracy. On STM32 parts
this is typically met in one of two ways:

#. **HSI48 + CRS** — the internal 48 MHz RC oscillator, trimmed against the USB
   SOF packets by the Clock Recovery System ("crystal-less USB").
#. **HSE crystal → PLL → PLLQ = 48 MHz** — an external crystal, with the PLL "Q"
   output providing 48 MHz. No CRS required.

The STM32G0B0 has USB but **no CRS**, so HSI48 cannot be trimmed and does not
reliably meet the USB FS tolerance over temperature. This board therefore uses
**HSE → PLLQ** for the USB clock:

.. code-block:: none

   VCO = HSE / M * N = 8 MHz / 1 * 24 = 192 MHz
     SYSCLK = VCO / R = 192 / 3 = 64 MHz   (max for STM32G0)
     USB    = VCO / Q = 192 / 4 = 48 MHz   (routed to USB)

The USB clock source is selected in the devicetree with
``<&rcc STM32_SRC_PLL_Q USB_SEL(1)>`` (``USB_SEL(1)`` = PLL "Q"). USB DP/DM are
on PA12/PA11 (``usb_dp_pa12`` / ``usb_dm_pa11``), and the USB node carries the
``zephyr_udc0`` label used by the USB device samples.

.. important::

   The ``clk_hse`` frequency in :file:`g0b0cet6.dts` (currently 8 MHz) must be
   adjusted to match the crystal actually fitted on your board. If a different
   frequency is used, pick ``mul-n`` / ``div-*`` so that PLLQ stays at 48 MHz and
   SYSCLK stays ≤ 64 MHz.

   If your board has **no HSE crystal**, USB on the STM32G0B0 can only fall back
   to the free-running HSI48, which is not guaranteed to meet the USB FS clock
   tolerance. That path additionally requires a ``clk_hsi48`` node (present in
   ``stm32g0b1.dtsi`` but not in ``stm32g0b0.dtsi``) and ``USB_SEL(0)``.

Connections and IOs
===================

Default Zephyr peripheral mapping:

- USART1 TX/RX : PA9/PA10 (console, 115200 8N1)
- USB DM/DP    : PA11/PA12

Programming and Debugging
*************************

Applications for the ``g0b0cet6`` board configuration can be built and flashed
in the usual way (see :ref:`build_an_application` and :ref:`application_run` for
more details).

.. note::

   This board lives outside the mainline board tree (``boards/custom``). Build it
   either from within this Zephyr tree or by pointing ``BOARD_ROOT`` at the
   directory that contains ``boards/custom``.

Flashing
========

The board is configured to be flashed using the west `STM32CubeProgrammer`_
runner. OpenOCD and JLink are also configured and can be selected with the
``--runner`` (or ``-r``) option:

.. code-block:: console

   $ west flash --runner openocd
   $ west flash --runner jlink

Building the USB CDC-ACM sample
-------------------------------

.. zephyr-app-commands::
   :zephyr-app: samples/subsys/usb/cdc_acm
   :board: g0b0cet6
   :goals: build flash

.. note::

   If the build stops in Kconfig with
   ``Could not open '...zephyr\' (in 'osource "$(ZEPHYR_HAL_ESPRESSIF_KCONFIG)"')``,
   this is a workspace module-sync issue (reproducible with any board, including
   stock ones), not a board problem. Run ``west update`` (and, if needed,
   ``pip install -r zephyr/scripts/requirements.txt``) to resolve it.

Debugging
=========

You can debug an application in the usual way. Here is an example for the
:zephyr:code-sample:`hello_world` application.

.. zephyr-app-commands::
   :zephyr-app: samples/hello_world
   :board: g0b0cet6
   :maybe-skip-config:
   :goals: debug

References
**********

.. target-notes::

.. _STM32G0B0 on www.st.com:
   https://www.st.com/en/microcontrollers-microprocessors/stm32g0b0ce.html

.. _STM32G0B0 reference manual:
   https://www.st.com/resource/en/reference_manual/rm0444-stm32g0x1-advanced-armbased-32bit-mcus-stmicroelectronics.pdf

.. _STM32CubeProgrammer:
   https://www.st.com/en/development-tools/stm32cubeprog.html
