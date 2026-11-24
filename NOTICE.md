# Notice

This project was written from scratch against the SSD1683 datasheet
and live experiments on the panel. The following third-party works
were studied as references only - no code was copied from them:

- Elecrow's official Arduino demo for the CrowPanel 5.79 (the factory
  source code bundled with the product wiki) - the factory init and
  update sequences, used as behavioral facts.
- ESPBoards `esphome-lvgl-crowpanel-epaper-5.79-4.2` ESPHome
  component - partial refresh and custom LUT approaches.
- `samperk1/esphome-crowpanel-579` - ESPHome configuration for the
  same panel.

Command and register sequences are hardware facts (bytes, register
addresses); the text in this repository is original.

Trademarks: CrowPanel and Elecrow are trademarks of their respective
owners. This is an independent project and is not affiliated with or
endorsed by Elecrow.

The built-in neutral image is Luo Mu's River Landscape in Autumn,
converted to 1bpp for this panel. The original work is in the public
domain and is available through Met Open Access:
https://www.metmuseum.org/art/collection/search/51769
