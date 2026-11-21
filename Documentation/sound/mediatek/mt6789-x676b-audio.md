# Infinix X676B (MT6789) audio references

This directory collects the reference material used by the MT6789 audio
bring-up for the Infinix Note 12 Pro (X676B).

## Speaker amplifier: Richtek RT5512

The phone uses a Richtek RT5512 boosted class-D speaker amplifier on
I2C6 at address 0x5c (DT compatible `richtek,rt5512`).

References:

- `mt6789-x676b-refs/DS5512B-00.pdf` - RT5512B datasheet (vendor PDF,
  included here as a hardware reference only).
- `mt6789-x676b-refs/rt5512-lkml-2022-10-13.patch` - the RT5512 ASoC
  driver posted to the ALSA/LKML lists by Jeff Chang (Richtek), dated
  2022-10-13. The driver in `sound/soc/codecs/rt5512.c` of this tree is
  based on that patch, adapted to the current kernel APIs.
- Original LKML message:
  https://lkml.iu.edu/hypermail/linux/kernel/2210.1/05285.html
- Datasheet source:
  https://www.richtek.com/content/dam/richtek/data/en/resources/product-specifications/rt55/rt5512b/product-data-sheets/pdf/DS5512B-00.pdf

## AFE / codec

- AFE: MediaTek MT6789 AFE (`sound/soc/mediatek/mt6789/`), ported from
  the vendor BSP driver.
- Codec: MT6358/MT6366 PMIC codec via MTKAIF.
- Machine: `mt6789-mt6366-sound`.

## Notes

The RT5512 is powered independently of the SoC and answers on I2C only
when its supply/enable is active; see the board schematics and the
gate-IC description in the vendor devicetree.
