import json

from lambda_function import evaluate_fis, build_command_request, validate_input


def main():
    with open("test_event.json", "r", encoding="utf-8") as file:
        payload = json.load(file)

    errors = validate_input(payload)

    if errors:
        print("Invalid FIS input:")
        for error in errors:
            print(f"- {error}")
        return

    fis_result = evaluate_fis(payload)
    command_request = build_command_request(payload, fis_result)

    print("FIS input is valid.")
    print("\nFIS result:")
    print(json.dumps(fis_result, indent=2))

    print("\nCommand request:")
    print(json.dumps(command_request, indent=2))


if __name__ == "__main__":
    main()