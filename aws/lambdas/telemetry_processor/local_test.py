import json
from lambda_function import validate_payload


def main():
    with open("test_event.json", "r", encoding="utf-8") as file:
        payload = json.load(file)

    errors = validate_payload(payload)

    if errors:
        print("Invalid payload:")
        for error in errors:
            print(f"- {error}")
    else:
        print("Payload is valid.")


if __name__ == "__main__":
    main()