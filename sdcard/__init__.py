# Componenta de card SD pentru ESP32-A1S.
#
# ESPHome NU are asa ceva in mod normal: driverul de card si sistemul de fisiere FAT sunt
# scoase dinadins din compilare, ca sa nu ocupe loc degeaba. Aici le cerem inapoi, prin cele
# patru "include_builtin_idf_component" de mai jos - fara ele, codul nici nu s-ar lega.
#
# Pinii NU se pot alege: pe ESP32 cititorul de card e legat direct in siliciu la GPIO 14 (ceas),
# 15 (comenzi) si 2 (date). Restul (4, 12, 13) sunt folositi doar in modul pe 4 fire, pe care
# nu-l folosim tocmai ca sa nu ne certam cu butonul KEY2 de pe GPIO13.

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

CONF_MOUNT_POINT = "mount_point"

sdcard_ns = cg.esphome_ns.namespace("sdcard")
SDCard = sdcard_ns.class_("SDCard", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SDCard),
        cv.Optional(CONF_MOUNT_POINT, default="/sd"): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config: ConfigType) -> None:
    require_fatfs()
    require_vfs_dir()
    for idf_component in ("fatfs", "sdmmc", "esp_driver_sdmmc", "wear_levelling"):
        include_builtin_idf_component(idf_component)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mount_point(config[CONF_MOUNT_POINT]))
