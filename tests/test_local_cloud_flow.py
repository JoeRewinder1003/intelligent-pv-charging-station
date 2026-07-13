import importlib.util
import json
import os
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]

os.environ.setdefault("AWS_EC2_METADATA_DISABLED", "true")
os.environ.setdefault("AWS_DEFAULT_REGION", "us-east-2")


def load_module(module_name: str, relative_path: str):
    module_path = PROJECT_ROOT / relative_path
    spec = importlib.util.spec_from_file_location(module_name, module_path)

    if spec is None or spec.loader is None:
        raise ImportError(f"Could not load module from {module_path}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    return module


fis_lambda = load_module(
    "integration_fis_processor",
    "aws/lambdas/fis_processor/lambda_function.py",
)

command_lambda = load_module(
    "integration_command_dispatcher",
    "aws/lambdas/command_dispatcher/lambda_function.py",
)


def fail(message: str, details=None) -> None:
    print(message)

    if details:
        for detail in details:
            print(f"- {detail}")

    sys.exit(1)


def main():
    test_event_path = (
        PROJECT_ROOT
        / "aws"
        / "lambdas"
        / "fis_processor"
        / "test_event.json"
    )

    with open(test_event_path, "r", encoding="utf-8") as file:
        telemetry_payload = json.load(file)

    fis_errors = fis_lambda.validate_input(telemetry_payload)

    if fis_errors:
        fail("FIS input is invalid:", fis_errors)

    fis_result = fis_lambda.evaluate_fis(telemetry_payload)
    command_request = fis_lambda.build_command_request(
        telemetry_payload,
        fis_result,
    )

    command_errors = command_lambda.validate_command(command_request)

    if command_errors:
        fail("Command request is invalid:", command_errors)

    command_payload = command_lambda.build_command_payload(
        command_request
    )

    if command_payload["status"] != "pending":
        fail("Command payload did not start with pending status.")

    if command_payload["command"] not in command_lambda.ALLOWED_COMMANDS:
        fail("Generated command is not allowed.")

    print("Local cloud flow is valid.")
    print("\nFIS mode:")
    print(fis_result["main_fis_output"]["fis_mode"])

    print("\nRequested mode:")
    print(
        fis_result["deterministic_validation"]["requested_mode"]
    )

    print("\nGenerated command:")
    print(command_payload["command"])

    print("\nCommand payload:")
    print(json.dumps(command_payload, indent=2))

    sys.exit(0)


if __name__ == "__main__":
    main()
