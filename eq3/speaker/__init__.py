import esphome.codegen as cg
from esphome.components import speaker
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_OUTPUT_SPEAKER, PLATFORM_ESP32
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


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await speaker.register_speaker(var, config)

    output_spkr = await cg.get_variable(config[CONF_OUTPUT_SPEAKER])
    cg.add(var.set_output_speaker(output_spkr))

    cg.add(var.set_bass_gain_db(config[CONF_BASS_GAIN]))
    cg.add(var.set_mid_gain_db(config[CONF_MID_GAIN]))
    cg.add(var.set_treble_gain_db(config[CONF_TREBLE_GAIN]))
