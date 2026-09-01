# Componenta de card SD pentru ESP32-A1S: monteaza cardul si serveste fisierele de pe el
# printr-un mic server web pornit chiar pe placa.
#
# DE CE UN SERVER WEB, si nu citire directa din fisier: biblioteca de decodare folosita de
# ESPHome (micro-decoder) stie sa porneasca doar din doua locuri - o adresa http:// sau un
# fisier incarcat INTREG in memorie. Nu are varianta cu fisier de pe disc. Verificat in lista
# ei de metode publice, nu presupus. Servindu-le prin http://127.0.0.1, fisierele de pe card
# trec prin exact acelasi drum ca un post de radio, deci pastram egalizatorul, vu-metrul,
# volumul si toate butoanele care merg deja.
#
# ESPHome scoate dinadins din compilare driverul de card, protocolul SD si sistemul de fisiere
# FAT. Aici le cerem inapoi - fara asta codul nici nu s-ar lega.
#
# Pinii NU se pot alege: pe ESP32 cititorul de card e legat in siliciu la GPIO 14 (ceas),
# 15 (comenzi) si 2 (date). Mergem pe 1 fir ca sa nu avem nevoie de GPIO 4, 12 si 13 - dintre
# care 13 e butonul KEY2.

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
CONF_MUSIC_FOLDER = "music_folder"
CONF_HTTP_PORT = "http_port"

sdcard_ns = cg.esphome_ns.namespace("sdcard")
SDCard = sdcard_ns.class_("SDCard", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SDCard),
        cv.Optional(CONF_MOUNT_POINT, default="/sd"): cv.string,
        cv.Optional(CONF_MUSIC_FOLDER, default="/sd"): cv.string,
        # 0 opreste serverul. Nu pune 80 - acolo sta deja pagina web a placii.
        cv.Optional(CONF_HTTP_PORT, default=81): cv.port,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config: ConfigType) -> None:
    require_fatfs()
    require_vfs_dir()
    for idf_component in (
        "fatfs",
        "sdmmc",
        "esp_driver_sdmmc",
        "wear_levelling",
        "esp_http_server",
    ):
        include_builtin_idf_component(idf_component)

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_mount_point(config[CONF_MOUNT_POINT]))
    cg.add(var.set_music_folder(config[CONF_MUSIC_FOLDER]))
    cg.add(var.set_http_port(config[CONF_HTTP_PORT]))
