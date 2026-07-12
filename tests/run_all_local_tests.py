import subprocess
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


TESTS = [
    {
        "name": "telemetry_processor local test",
        "cwd": PROJECT_ROOT / "aws" / "lambdas" / "telemetry_processor",
        "command": [sys.executable, "local_test.py"],
    },
    {
        "name": "command_dispatcher local test",
        "cwd": PROJECT_ROOT / "aws" / "lambdas" / "command_dispatcher",
        "command": [sys.executable, "local_test.py"],
    },
    {
        "name": "diagnostics local test",
        "cwd": PROJECT_ROOT / "aws" / "lambdas" / "diagnostics",
        "command": [sys.executable, "local_test.py"],
    },
    {
        "name": "local cloud flow integration test",
        "cwd": PROJECT_ROOT,
        "command": [sys.executable, "tests/test_local_cloud_flow.py"],
    },
]


def run_test(test):
    print(f"\n=== Running: {test['name']} ===")

    result = subprocess.run(
        test["command"],
        cwd=test["cwd"],
        text=True,
        capture_output=True,
    )

    if result.stdout:
        print(result.stdout)

    if result.stderr:
        print(result.stderr)

    if result.returncode != 0:
        print(f"FAILED: {test['name']}")
        return False

    print(f"PASSED: {test['name']}")
    return True


def main():
    all_passed = True

    for test in TESTS:
        passed = run_test(test)
        if not passed:
            all_passed = False

    print("\n=== Summary ===")

    if all_passed:
        print("All local tests passed.")
        sys.exit(0)

    print("One or more tests failed.")
    sys.exit(1)


if __name__ == "__main__":
    main()