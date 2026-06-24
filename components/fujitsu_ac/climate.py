import esphome.codegen as cg
from esphome.components import climate, uart
import esphome.config_validation as cv
from pathlib import Path

DEPENDENCIES = ["climate", "uart"]

# Reuse the Arduino library protocol sources from the repo root (../../src).
PROTOCOL_DIR = (Path(__file__).parent.parent.parent / "src").resolve()

fujitsu_ac_ns = cg.esphome_ns.namespace("fujitsu_ac")
FujitsuClimate = fujitsu_ac_ns.class_(
    "FujitsuClimate", climate.Climate, cg.Component, uart.UARTDevice
)

CONFIG_SCHEMA = (
    climate.climate_schema(FujitsuClimate)
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)

FINAL_VALIDATE_SCHEMA = cv.All(
    uart.final_validate_device_schema(
        "fujitsu_ac",
        require_rx=True,
        require_tx=True,
        baud_rate=9600,
        data_bits=8,
        parity="NONE",
        stop_bits=1,
    )
)


async def to_code(config):
    var = await climate.new_climate(config)
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    cg.add_build_flag(f"-I{PROTOCOL_DIR}")
