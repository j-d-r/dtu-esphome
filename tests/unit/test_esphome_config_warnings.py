"""Configurations that validate but must *warn*, checked through `esphome config`.

Sensor channels that do not match the inverter model's DC inputs are reported as
warnings, and the config must still be accepted (the model may be the wrong part, not
the channels): a channel the model does not have only ever reports NaN, and a channel
the model has but the config omits is never published.
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
      name: warning-case

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
      inverters:
        - id: inv
          serial: "{serial}"
          model: {model}
    """
)


def sensors(*entries):
    """A sensor: block with one hoymiles_dtu_base entry per tuple of channel numbers."""
    text = "sensor:\n"
    for numbers in entries:
        text += "  - platform: hoymiles_dtu_base\n    inverter_id: inv\n"
        text += f'    ac_power: {{name: "AC Power {len(text)}"}}\n'
        if numbers:
            text += "    channels:\n"
            for n in numbers:
                text += f"      - number: {n}\n"
                text += f'        dc_power: {{name: "CH{n} DC Power"}}\n'
    return text


EXTRA = "sensors will only ever report NaN"
MISSING = "but no sensors are declared for"

# (config, expected extra-channel text or None, expected missing-channel text or None)
CASES = [
    pytest.param(
        NRF24_HUB.format(serial="112100000000", model="hm_400") + sensors((1, 2)),
        "its channel 2 sensors",
        None,
        id="hm-1-input-extra-channel-2",
    ),
    pytest.param(
        # The v2 record is shaped like a 2-channel one but has a single DC input.
        CMT_HUB.format(serial="112500000000", model="hms_500_v2") + sensors((1, 2)),
        "its channel 2 sensors",
        None,
        id="hms-v2-extra-channel-2",
    ),
    pytest.param(
        NRF24_HUB.format(serial="116100000000", model="hm_1000") + sensors((1, 2)),
        None,
        "declared for channels 3, 4",
        id="hm-4-inputs-missing-3-4",
    ),
    pytest.param(
        CMT_HUB.format(serial="114400000000", model="hms_800") + sensors((1, 3, 4)),
        "its channels 3, 4 sensors",
        "declared for channel 2",
        id="hms-2-inputs-extra-and-missing",
    ),
    pytest.param(
        NRF24_HUB.format(serial="116100000000", model="hm_1500")
        + sensors((1, 2, 3, 4)),
        None,
        None,
        id="hm-4-inputs-all-channels",
    ),
    pytest.param(
        # Channels spread over several entries, like the configs/ packages do.
        NRF24_HUB.format(serial="116100000000", model="hm_1500")
        + sensors((), (1, 2), (3, 4)),
        None,
        None,
        id="hm-4-inputs-split-entries",
    ),
    pytest.param(
        # No channels at all reads as deliberate (AC values only).
        CMT_HUB.format(serial="114400000000", model="hms_800") + sensors(()),
        None,
        None,
        id="hms-no-channels",
    ),
]


@pytest.mark.parametrize("config_text,extra,missing", CASES)
def test_channel_warnings(tmp_path, config_text, extra, missing):
    esphome = shutil.which("esphome")
    if esphome is None:
        pytest.skip("esphome executable not found")
    config = tmp_path / "case.yaml"
    config.write_text(HEADER + "\n" + config_text)
    result = subprocess.run(
        [esphome, "config", str(config)],
        cwd=REPO,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    assert result.returncode == 0, result.stdout
    for marker, expected in ((EXTRA, extra), (MISSING, missing)):
        if expected is None:
            assert marker not in result.stdout, result.stdout
        else:
            # Reported once per inverter, however many sensor entries it has.
            assert result.stdout.count(marker) == 1, result.stdout
            assert expected in result.stdout, result.stdout
