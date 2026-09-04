import esphome.codegen as cg
from esphome.components.esp32 import (
    include_builtin_idf_component,
    require_fatfs,
    require_vfs_dir,
)
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.types import ConfigType

CODEOWNERS = ["@cristi2005"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["audio"]

CONF_MOUNT_POINT = "mount_point"
CONF_MUSIC_FOLDER = "music_folder"

sdcard_ns = cg.esphome_ns.namespace("sdcard")
SDCard = sdcard_ns.class_("SDCard", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SDCard),
        cv.Optional(CONF_MOUNT_POINT, default="/sd"): cv.string,
        cv.Optional(CONF_MUSIC_FOLDER, default="/sd"): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config: ConfigType) -> None:
    require_fatfs()
    require_vfs_dir()
    # NU mai avem nevoie de esp_http_server - nu mai exista niciun server. Doar cardul in sine.
    for idf_component in (
        "fatfs",
        "sdmmc",
        "esp_driver_sdmmc",
        "wear_levelling",
    ):
        include_builtin_idf_component(idf_component)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mount_point(config[CONF_MOUNT_POINT]))
    cg.add(var.set_music_folder(config[CONF_MUSIC_FOLDER]))
