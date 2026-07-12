import json
from lambda_function import validate_command, build_command_payload


def main():
    with open("test_event.json", "r", encoding="utf-8") as file:
        payload = json.load(file)

    errors = validate_command(payload)

    if errors:
        print("Invalid command payload:")
        for error in errors:
            print(f"- {error}")
        return

    command_payload = build_command_payload(payload)

    print("Command payload is valid.")
    print(json.dumps(command_payload, indent=2))


if __name__ == "__main__":
    main()