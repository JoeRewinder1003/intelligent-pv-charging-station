import json

from lambda_function import (
    detect_message_type,
    validate_diagnostics_payload,
)


TEST_FILES = [
    "test_event_fault.json",
    "test_event_ack.json",
    "test_event_status.json",
]


def main():
    for filename in TEST_FILES:
        print(f"\nTesting {filename}...")

        with open(filename, "r", encoding="utf-8") as file:
            payload = json.load(file)

        message_type = detect_message_type(payload)
        errors = validate_diagnostics_payload(payload, message_type)

        if errors:
            print("Invalid diagnostics payload:")
            for error in errors:
                print(f"- {error}")
        else:
            print(f"Valid diagnostics payload. Type: {message_type}")


if __name__ == "__main__":
    main()