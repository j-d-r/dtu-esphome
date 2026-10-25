"""Configurations that must be *rejected*, checked through `esphome config`.

The valid-config fixtures live in tests/components/hoymiles_dtu/ and are covered by
test_esphome_config.py. These are the guards that stop a config which would compile
and flash happily but never talk to the inverter: an inverter model paired with the
radio it cannot use, a CMT2300A frequency the chip cannot tune to, or a CMT2300A
SPI host that a `spi:` bus already owns.
"""

import shutil
import subprocess
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).parents[2]
COMPONENTS = REPO / "components"

HEADER = textwrap.dedent(
    f"""\
    esphome:
      name: invalid-case

    esp32:
      board: esp32dev
      framework:
        type: arduino

    external_components:
      - source:
          type: local
          path: {COMPONENTS}
        components: [hoymiles_dtu_base, hoymiles_dtu_rf24, hoymiles_dtu_cmt]
    """
)

NRF24_HUB = textwrap.dedent(
    """\
    spi:
      id: dtu_spi
      clk_pin: GPIO18
      mosi_pin: GPIO23
      miso_pin: GPIO19

    hoymiles_dtu_rf24:
      id: dtu
      spi_id: dtu_spi
      cs_pin: GPIO5
      ce_pin: GPIO4
      inverters:
        - id: inv
          serial: "{serial}"
          model: {model}
    """
)

CMT_HUB = textwrap.dedent(
    """\
    hoymiles_dtu_cmt:
      id: dtu
      sclk_pin: GPIO12
      sdio_pin: GPIO14
      csb_pin: GPIO27
      fcsb_pin: GPIO26
    {extra}
      inverters:
        - id: inv
          serial: "{serial}"
          model: {model}
    """
)


def spi_buses(*interfaces):
    """`spi:` buses claiming hardware hosts, one per `interface:` value given."""
    buses = "".join(
        f"  - id: other_spi_{index}\n"
        f"    clk_pin: GPIO{18 + index * 3}\n"
        f"    mosi_pin: GPIO{19 + index * 3}\n"
        f"    interface: {interface}\n"
        for index, interface in enumerate(interfaces)
    )
    return "spi:\n" + buses + "\n"


CASES = [
    pytest.param(
        NRF24_HUB.format(serial="114400000000", model="hms_800"),
        "CMT2300A",
        id="hms-model-on-nrf24",
    ),
    pytest.param(
        CMT_HUB.format(serial="116182806989", model="hm_1200", extra=""),
        "nRF24",
        id="hm-model-on-cmt2300a",
    ),
    pytest.param(
        CMT_HUB.format(
            serial="114400000000", model="hms_800", extra="  frequency: 865.1MHz"
        ),
        "multiple of 250 kHz",
        id="frequency-off-grid",
    ),
    pytest.param(
        CMT_HUB.format(
            serial="114400000000", model="hms_800", extra="  frequency: 900MHz"
        ),
        "eu band",
        id="frequency-outside-region-band",
    ),
    pytest.param(
        spi_buses("any")
        + CMT_HUB.format(
            serial="114400000000", model="hms_800", extra="  interface: spi2"
        ),
        "already used by a `spi:` bus",
        id="cmt-host-taken-by-spi-bus",
    ),
    pytest.param(
        spi_buses("hardware", "hardware")
        + CMT_HUB.format(
            serial="114400000000", model="hms_800", extra="  interface: hardware"
        ),
        "no hardware SPI host left",
        id="cmt-no-hardware-host-left",
    ),
]


@pytest.mark.parametrize("hub,expected_message", CASES)
def test_config_is_rejected(tmp_path, hub, expected_message):
    esphome = shutil.which("esphome")
    if esphome is None:
        pytest.skip("esphome executable not found")
    config = tmp_path / "invalid.yaml"
    config.write_text(HEADER + "\n" + hub)
    result = subprocess.run(
        [esphome, "config", str(config)],
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    assert result.returncode != 0, result.stdout
    assert expected_message in result.stdout, result.stdout
