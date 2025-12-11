import csv
import logging
from pathlib import Path

import esphome.codegen as cg
from esphome.components.ota import BASE_OTA_SCHEMA, OTAComponent, ota_to_code
from esphome.config_helpers import merge_config
import esphome.config_validation as cv
from esphome.const import (
    CONF_ESPHOME,
    CONF_ID,
    CONF_MODE,
    CONF_NUM_ATTEMPTS,
    CONF_OTA,
    CONF_PASSWORD,
    CONF_PLATFORM,
    CONF_PORT,
    CONF_REBOOT_TIMEOUT,
    CONF_SAFE_MODE,
    CONF_VERSION,
)

CONF_TARGET_PARTITION = "target_partition"
CONF_OTA_HELPER_PARTITION = "ota_helper_partition"
from esphome.core import CORE, coroutine_with_priority
from esphome.coroutine import CoroPriority
import esphome.final_validate as fv
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)


CODEOWNERS = ["@esphome/core"]
DEPENDENCIES = ["network"]


def supports_sha256() -> bool:
    """Check if the current platform supports SHA256 for OTA authentication."""
    return bool(CORE.is_esp32 or CORE.is_esp8266 or CORE.is_rp2040 or CORE.is_libretiny)


def AUTO_LOAD() -> list[str]:
    """Conditionally auto-load sha256 only on platforms that support it."""
    base_components = ["md5", "socket"]
    if supports_sha256():
        return base_components + ["sha256"]
    return base_components


esphome = cg.esphome_ns.namespace("esphome")
ESPHomeOTAComponent = esphome.class_("ESPHomeOTAComponent", OTAComponent)


def _validate_ota_helper_partition(full_conf: ConfigType) -> None:
    """Validate and prepare partition table for OTA helper partition feature."""
    # Find OTA helper partition configuration
    ota_helper_partition = None
    for ota_conf in full_conf.get(CONF_OTA, []):
        if ota_conf.get(CONF_PLATFORM) == CONF_ESPHOME:
            if CONF_OTA_HELPER_PARTITION in ota_conf:
                ota_helper_partition = ota_conf[CONF_OTA_HELPER_PARTITION]
                break

    if not ota_helper_partition:
        return  # No OTA helper partition configured

    # Only ESP32 supports this feature
    if not CORE.is_esp32:
        raise cv.Invalid(
            f"'{CONF_OTA_HELPER_PARTITION}' is only supported on ESP32 platform"
        )

    # Check if user provided custom partitions.csv
    esp32_conf = full_conf.get("esp32", {})
    custom_partitions_path = esp32_conf.get("partitions")

    if custom_partitions_path:
        # User provided custom partitions - verify it contains OTA helper partition
        partitions_file = CORE.relative_config_path(custom_partitions_path)
        if not partitions_file.exists():
            raise cv.Invalid(
                f"Custom partitions file '{custom_partitions_path}' not found"
            )

        # Parse and validate
        partition_found = False
        try:
            with open(partitions_file, "r") as f:
                # ESP-IDF partition tables don't use CSV headers, so we specify fieldnames manually
                # Format: Name, Type, SubType, Offset, Size, Flags
                # Filter out comment lines and empty lines
                lines = [line for line in f if line.strip() and not line.strip().startswith("#")]
                reader = csv.DictReader(
                    lines,
                    fieldnames=["Name", "Type", "SubType", "Offset", "Size", "Flags"],
                    skipinitialspace=True
                )
                for row in reader:
                    partition_name = row.get("Name", "").strip()
                    if partition_name == ota_helper_partition:
                        partition_found = True
                        # Validate it's an app partition
                        partition_type = row.get("Type", "").strip().lower()
                        if partition_type != "app":
                            raise cv.Invalid(
                                f"Partition '{ota_helper_partition}' must be of type 'app', got '{partition_type}'"
                            )
                        break
        except Exception as e:
            raise cv.Invalid(
                f"Failed to parse custom partitions file '{custom_partitions_path}': {e}"
            ) from e

        if not partition_found:
            raise cv.Invalid(
                f"Custom partitions file '{custom_partitions_path}' does not contain "
                f"partition named '{ota_helper_partition}'. When using '{CONF_OTA_HELPER_PARTITION}', "
                f"your custom partition table must include this partition as an 'app' type."
            )

        _LOGGER.info(
            "Using custom partitions.csv with OTA helper partition '%s'",
            ota_helper_partition,
        )
    else:
        # No custom partitions - need to auto-generate based on flash size
        flash_size = esp32_conf.get("flash_size", "4MB")
        _generate_dual_partition_table(ota_helper_partition, flash_size)


def _generate_dual_partition_table(ota_helper_partition: str, flash_size: str) -> None:
    """Generate dual-partition table optimized for flash size."""
    # Map flash sizes to partition layouts
    # Format: (main_partition_size, ota_helper_size, nvs_size, otadata_offset)
    partition_layouts = {
        "4MB": ("0x2F0000", "0x100000", "0x6000"),  # 3MB main, 1MB helper
        "8MB": ("0x6F0000", "0x100000", "0x6000"),  # 7MB main, 1MB helper
        "16MB": ("0xEF0000", "0x100000", "0x6000"),  # 15MB main, 1MB helper
        "32MB": ("0x1EF0000", "0x100000", "0x6000"),  # 31MB main, 1MB helper
    }

    if flash_size not in partition_layouts:
        raise cv.Invalid(
            f"Dual-partition OTA with '{CONF_OTA_HELPER_PARTITION}' requires supported flash size. "
            f"Got '{flash_size}', supported: {list(partition_layouts.keys())}. "
            f"For custom layouts, provide your own partitions.csv file."
        )

    main_size, helper_size, nvs_size = partition_layouts[flash_size]

    # Generate partition table content
    partition_content = f"""# ESP-IDF Partition Table
# Auto-generated for dual-partition OTA (flash_size={flash_size})
# Name,   Type, SubType,  Offset,   Size,     Flags
nvs,      data, nvs,      0x9000,   {nvs_size},
otadata,  data, ota,      ,         0x2000,
phy_init, data, phy,      ,         0x1000,
main,     app,  ota_0,    0x10000,  {main_size},
{ota_helper_partition}, app, ota_1, , {helper_size},
"""

    # Write to project directory
    partitions_path = Path(CORE.config_dir) / "partitions.csv"
    with open(partitions_path, "w") as f:
        f.write(partition_content)

    _LOGGER.info(
        "Auto-generated dual-partition table: %s (main=%s, %s=%s)",
        partitions_path,
        main_size,
        ota_helper_partition,
        helper_size,
    )


def ota_esphome_final_validate(config):
    full_conf = fv.full_config.get()
    full_ota_conf = full_conf[CONF_OTA]
    new_ota_conf = []
    merged_ota_esphome_configs_by_port = {}
    ports_with_merged_configs = []
    for ota_conf in full_ota_conf:
        if ota_conf.get(CONF_PLATFORM) == CONF_ESPHOME:
            if (
                conf_port := ota_conf.get(CONF_PORT)
            ) not in merged_ota_esphome_configs_by_port:
                merged_ota_esphome_configs_by_port[conf_port] = ota_conf
            else:
                if merged_ota_esphome_configs_by_port[conf_port][
                    CONF_VERSION
                ] != ota_conf.get(CONF_VERSION):
                    raise cv.Invalid(
                        f"Found multiple configurations but {CONF_VERSION} is inconsistent"
                    )
                if (
                    merged_ota_esphome_configs_by_port[conf_port][CONF_ID].is_manual
                    and ota_conf.get(CONF_ID).is_manual
                ):
                    raise cv.Invalid(
                        f"Found multiple configurations but {CONF_ID} is inconsistent"
                    )
                if (
                    CONF_PASSWORD in merged_ota_esphome_configs_by_port[conf_port]
                    and CONF_PASSWORD in ota_conf
                    and merged_ota_esphome_configs_by_port[conf_port][CONF_PASSWORD]
                    != ota_conf.get(CONF_PASSWORD)
                ):
                    raise cv.Invalid(
                        f"Found multiple configurations but {CONF_PASSWORD} is inconsistent"
                    )

                ports_with_merged_configs.append(conf_port)
                merged_ota_esphome_configs_by_port[conf_port] = merge_config(
                    merged_ota_esphome_configs_by_port[conf_port], ota_conf
                )
        else:
            new_ota_conf.append(ota_conf)

    new_ota_conf.extend(merged_ota_esphome_configs_by_port.values())

    full_conf[CONF_OTA] = new_ota_conf
    fv.full_config.set(full_conf)

    if len(ports_with_merged_configs) > 0:
        _LOGGER.warning(
            "Found and merged multiple configurations for %s %s %s port(s) %s",
            CONF_OTA,
            CONF_PLATFORM,
            CONF_ESPHOME,
            ports_with_merged_configs,
        )

    # Validate OTA helper partition configuration
    _validate_ota_helper_partition(full_conf)


def _consume_ota_sockets(config: ConfigType) -> ConfigType:
    """Register socket needs for OTA component."""
    from esphome.components import socket

    # OTA needs 1 listening socket (client connections are temporary during updates)
    socket.consume_sockets(1, "ota")(config)
    return config


CONF_PSRAM = "psram"

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ESPHomeOTAComponent),
            cv.Optional(CONF_VERSION, default=2): cv.one_of(1, 2, int=True),
            cv.Optional(CONF_MODE): cv.enum({"standard": 0, "psram": 1}, upper=False),
            cv.Optional(CONF_TARGET_PARTITION): cv.All(cv.string, cv.Length(max=16)),
            cv.Optional(CONF_OTA_HELPER_PARTITION): cv.All(cv.string, cv.Length(max=16)),
            cv.SplitDefault(
                CONF_PORT,
                esp8266=8266,
                esp32=3232,
                rp2040=2040,
                bk72xx=8892,
                ln882x=8820,
                rtl87xx=8892,
            ): cv.port,
            cv.Optional(CONF_PASSWORD): cv.string,
            cv.Optional(CONF_NUM_ATTEMPTS): cv.invalid(
                f"'{CONF_SAFE_MODE}' (and its related configuration variables) has moved from 'ota' to its own component. See https://esphome.io/components/safe_mode"
            ),
            cv.Optional(CONF_REBOOT_TIMEOUT): cv.invalid(
                f"'{CONF_SAFE_MODE}' (and its related configuration variables) has moved from 'ota' to its own component. See https://esphome.io/components/safe_mode"
            ),
            cv.Optional(CONF_SAFE_MODE): cv.invalid(
                f"'{CONF_SAFE_MODE}' (and its related configuration variables) has moved from 'ota' to its own component. See https://esphome.io/components/safe_mode"
            ),
        }
    )
    .extend(BASE_OTA_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    _consume_ota_sockets,
)

FINAL_VALIDATE_SCHEMA = ota_esphome_final_validate


@coroutine_with_priority(CoroPriority.OTA_UPDATES)
async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_port(config[CONF_PORT]))

    # PSRAM mode
    if config.get(CONF_MODE) == "psram":
        if not CORE.is_esp32:
            raise cv.Invalid("PSRAM OTA mode only supported on ESP32")
        from esphome.components import psram
        if psram.DOMAIN not in fv.full_config.get():
            raise cv.Invalid("PSRAM OTA mode requires psram component")
        cg.add_define("USE_OTA_PSRAM_MODE")
        _LOGGER.warning("PSRAM OTA mode enabled - initial flash via serial required")

    # Target partition
    if CONF_TARGET_PARTITION in config:
        cg.add(var.set_target_partition(config[CONF_TARGET_PARTITION]))

    # OTA helper partition
    if CONF_OTA_HELPER_PARTITION in config:
        cg.add(var.set_ota_helper_partition(config[CONF_OTA_HELPER_PARTITION]))
        # Trigger OTA helper firmware build (will be handled by post-build script)
        cg.add_define("USE_OTA_HELPER_PARTITION", f'"{config[CONF_OTA_HELPER_PARTITION]}"')

    # Password could be set to an empty string and we can assume that means no password
    if config.get(CONF_PASSWORD):
        cg.add(var.set_auth_password(config[CONF_PASSWORD]))
        cg.add_define("USE_OTA_PASSWORD")
        # Only include hash algorithms when password is configured
        cg.add_define("USE_OTA_MD5")
        # Only include SHA256 support on platforms that have it
        if supports_sha256():
            cg.add_define("USE_OTA_SHA256")
    cg.add_define("USE_OTA_VERSION", config[CONF_VERSION])

    await cg.register_component(var, config)
    await ota_to_code(var, config)
