import json
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]

FIS_PROCESSOR_PATH = PROJECT_ROOT / "aws" / "lambdas" / "fis_processor"
COMMAND_DISPATCHER_PATH = PROJECT_ROOT / "aws" / "lambdas" / "command_dispatcher"

sys.path.insert(0, str(FIS_PROCESSOR_PATH))
import lambda_function as fis_lambda

sys.path.pop(0)
sys.modules.pop("lambda_function", None)

sys.path.insert(0, str(COMMAND_DISPATCHER_PATH))
import lambda_function as command_lambda


def main():
    test_event_path = FIS_PROCESSOR_PATH / "test_event.json"

    with open(test_event_path, "r", encoding="utf-8") as file:
        telemetry_payload = json.load(file)

    fis_errors = fis_lambda.validate_input(telemetry_payload)

    if fis_errors:
        print("FIS input is invalid:")
        for error in fis_errors:
            print(f"- {error}")
        return

    fis_result = fis_lambda.evaluate_fis(telemetry_payload)
    command_request = fis_lambda.build_command_request(telemetry_payload, fis_result)

    command_errors = command_lambda.validate_command(command_request)

    if command_errors:
        print("Command request is invalid:")
        for error in command_errors:
            print(f"- {error}")
        return

    command_payload = command_lambda.build_command_payload(command_request)

    print("Local cloud flow is valid.")
    print("\nFIS mode:")
    print(fis_result["main_fis_output"]["fis_mode"])

    print("\nRequested mode:")
    print(fis_result["deterministic_validation"]["requested_mode"])

    print("\nGenerated command:")
    print(command_payload["command"])

    print("\nCommand payload:")
    print(json.dumps(command_payload, indent=2))


if __name__ == "__main__":
    main()