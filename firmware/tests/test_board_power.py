"""Static regression checks against Waveshare's V2 active-low power controls.

Reference: waveshareteam/ESP32-S3-ePaper-1.54, ESP-IDF/V2/08_Audio_Test/
components/board_power_bsp/board_power_bsp.cpp. These checks do not test hardware.
"""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def body(relative, signature):
    source = (ROOT / relative).read_text()
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    position = opening + 1
    while depth:
        if source[position] == '{':
            depth += 1
        elif source[position] == '}':
            depth -= 1
        position += 1
    return source[opening + 1:position - 1]


class V2PowerTests(unittest.TestCase):
    def assert_power(self, relative, signature, pin, level):
        function = body(relative, signature)
        values = re.findall(r'gpio_set_level\(board::' + pin + r',\s*([01])\)', function)
        self.assertEqual(values, [str(level)], f'{signature}: {pin} must be {level}')

    def test_epaper_init_enables_active_low_supply(self):
        self.assert_power('firmware/src/display/epaper_display.cpp',
                          'bool EpaperDisplay::init()', 'EPD_PWR', 0)

    def test_epaper_sleep_disables_active_low_supply(self):
        self.assert_power('firmware/src/display/epaper_display.cpp',
                          'void EpaperDisplay::sleep()', 'EPD_PWR', 1)

    def test_audio_init_enables_active_low_supply(self):
        self.assert_power('firmware/src/audio/audio_recorder.cpp',
                          'bool AudioRecorder::init()', 'AUDIO_PWR', 0)


if __name__ == '__main__':
    unittest.main()
