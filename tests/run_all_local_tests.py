import subprocess
import sys
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]

TESTS = [
    {
        "name": "cloud backend unit tests",
        "cwd": PROJECT_ROOT,
        "command": [
            sys.executable,
            "-m",
            "unittest",
            "-v",
            "tests/test_cloud_backend.py",
        ],
    },
    {
        "name": "local cloud flow integration test",
        "cwd": PROJECT_ROOT,
        "command": [
            sys.executable,
            "tests/test_local_cloud_flow.py",
        ],
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
        if not run_test(test):
            all_passed = False

    print("\n=== Summary ===")

    if all_passed:
        print("All automated tests passed.")
        sys.exit(0)

    print("One or more automated tests failed.")
    sys.exit(1)


if __name__ == "__main__":
    main()
