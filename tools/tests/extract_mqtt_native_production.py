"""Compile the production MQTT methods unchanged without unrelated ESP hardware.

The host fixture uses the real cJSON library, register catalogs and protocol
codecs. Only the ESP event, queue, clock and MQTT client boundaries are replaced.
This extractor copies function definitions verbatim on each test run; native
parsing, validation and serialization are never reimplemented in the fixture.
"""
from pathlib import Path
import argparse


def definition(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    mode = "code"
    index = opening
    while index < len(source):
        character = source[index]
        following = source[index:index + 2]
        if mode == "line":
            if character == "\n":
                mode = "code"
        elif mode == "comment":
            if following == "*/":
                mode = "code"
                index += 1
        elif mode in ('"', "'"):
            if character == "\\":
                index += 1
            elif character == mode:
                mode = "code"
        elif following == "//":
            mode = "line"
            index += 1
        elif following == "/*":
            mode = "comment"
            index += 1
        elif character in ('"', "'"):
            mode = character
        elif character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
        index += 1
    raise ValueError(f"Unterminated production function: {signature}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "main/src/network/mqtt_bridge.cpp").read_text(encoding="utf-8")
    helpers = [
        "int enqueue_payload(", "bool parse_operation(", "bool parse_command_id(",
        "const char *protocol_outcome_name(", "template <std::size_t Size>",
        "bool topic_component(", "bool parse_named_category(", "bool parse_register_domain(",
        "const inverter::RegisterDescriptor *find_named_register(", "bool encode_named_value(",
        "std::uint8_t member_from_topic(",
        "bool protocol_poll_info(", "cJSON *protocol_record_json(", "const char *protocol_live_section(",
    ]
    methods = [
        "MqttBridge::MqttBridge(", "void MqttBridge::handle_event(",
        "void MqttBridge::handle_command(", "bool MqttBridge::remember_command_id(",
        "void MqttBridge::publish_protocol_telemetry(",
        "void MqttBridge::publish_protocol_telemetry_now(",
        "void MqttBridge::publish_protocol_record_now(",
        "void MqttBridge::publish_protocol_live_now(",
        "void MqttBridge::publish_protocol_command_result(",
        "void MqttBridge::publish_protocol_command_result_now(",
        "bool MqttBridge::ota_publisher_healthy()", "bool MqttBridge::ota_results_drained()",
        "void MqttBridge::make_topic(",
    ]
    globals_start = source.index('constexpr char tag[] = "mqtt_bridge";')
    globals_end = source.index("// Only for literal keys", globals_start)
    code = "// Generated from production mqtt_bridge.cpp; do not edit.\n"
    code += "namespace inverter_gateway::network {\nnamespace {\n"
    code += source[globals_start:globals_end]
    code += "\n\n".join(definition(source, item) for item in helpers)
    code += "\n} // namespace\n"
    code += "\n\n".join(definition(source, item) for item in methods)
    code += "\n} // namespace inverter_gateway::network\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(code, encoding="utf-8")
    print(f"Extracted {len(helpers) + len(methods)} production MQTT definitions unchanged.")


if __name__ == "__main__":
    main()
