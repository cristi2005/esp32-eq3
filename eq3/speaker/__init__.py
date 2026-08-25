import esphome.codegen as cg
from esphome.components import audio, speaker
import esphome.config_validation as cv
from esphome.const import (
    CONF_BITS_PER_SAMPLE,
    CONF_ID,
    CONF_NUM_CHANNELS,
    CONF_OUTPUT_SPEAKER,
    CONF_SAMPLE_RATE,
    PLATFORM_ESP32,
)
from esphome.core.entity_helpers import inherit_property_from
from esphome.types import ConfigType

# Real 3-band software equalizer (bass/mid/treble), implemented as biquad IIR filters applied to the
# PCM audio stream in software. Insert it between a source (e.g. the mixer's output) and the physical
# speaker: it is transparent about audio format (bits per sample / channels / sample rate) - whatever
# format the upstream component gives it, it filters and forwards unchanged to output_speaker.
AUTO_LOAD = ["audio"]
CODEOWNERS = ["@cristi"]

eq3_ns = cg.esphome_ns.namespace("eq3")
EQ3Speaker = eq3_ns.class_("EQ3Speaker", cg.Component, speaker.Speaker)

CONF_BASS_GAIN = "bass_gain"
CONF_MID_GAIN = "mid_gain"
CONF_TREBLE_GAIN = "treble_gain"

GAIN_SCHEMA = cv.float_range(min=-12.0, max=12.0)


def _validate_audio_compatibility(config: ConfigType) -> ConfigType:
    # EQ3 is transparent to audio format - it doesn't declare its own sample_rate/bits_per_sample/
    # channels, so inherit them from output_speaker's config. This both validates against it and makes
    # those properties visible on eq3's own config, for anything further upstream (e.g. the mixer) that
    # points to this eq3 entry as ITS OWN output_speaker and needs to inherit the same way.
    inherit_property_from(CONF_BITS_PER_SAMPLE, CONF_OUTPUT_SPEAKER)(config)
    inherit_property_from(CONF_NUM_CHANNELS, CONF_OUTPUT_SPEAKER)(config)
    inherit_property_from(CONF_SAMPLE_RATE, CONF_OUTPUT_SPEAKER)(config)

    audio.final_validate_audio_schema(
        "eq3",
        audio_device=CONF_OUTPUT_SPEAKER,
        bits_per_sample=config.get(CONF_BITS_PER_SAMPLE),
        channels=config.get(CONF_NUM_CHANNELS),
        sample_rate=config.get(CONF_SAMPLE_RATE),
    )(config)

    return config


CONFIG_SCHEMA = cv.All(
    speaker.SPEAKER_SCHEMA.extend(
        {
            cv.GenerateID(): cv.declare_id(EQ3Speaker),
            cv.Required(CONF_OUTPUT_SPEAKER): cv.use_id(speaker.Speaker),
            cv.Optional(CONF_BASS_GAIN, default=0.0): GAIN_SCHEMA,
            cv.Optional(CONF_MID_GAIN, default=0.0): GAIN_SCHEMA,
            cv.Optional(CONF_TREBLE_GAIN, default=0.0): GAIN_SCHEMA,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32]),
)

FINAL_VALIDATE_SCHEMA = _validate_audio_compatibility


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    output_spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(output_spkr))

    cg.add(var.set_bass_gain_db(config[CONF_BASS_GAIN]))
    cg.add(var.set_mid_gain_db(config[CONF_MID_GAIN]))
    cg.add(var.set_treble_gain_db(config[CONF_TREBLE_GAIN]))
