import importlib.util
import json
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


PROJECT_ROOT = Path(__file__).resolve().parents[1]

# Prevent boto3 from trying to query EC2 instance metadata during local tests.
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


telemetry = load_module(
    "telemetry_processor_lambda",
    "aws/lambdas/telemetry_processor/lambda_function.py",
)

command = load_module(
    "command_dispatcher_lambda",
    "aws/lambdas/command_dispatcher/lambda_function.py",
)

diagnostics = load_module(
    "diagnostics_lambda",
    "aws/lambdas/diagnostics/lambda_function.py",
)

fis = load_module(
    "fis_processor_lambda",
    "aws/lambdas/fis_processor/lambda_function.py",
)


def load_json(relative_path: str):
    with open(PROJECT_ROOT / relative_path, "r", encoding="utf-8") as file:
        return json.load(file)


def parse_lambda_body(result):
    return json.loads(result["body"])


class TelemetryProcessorTests(unittest.TestCase):
    def setUp(self):
        self.payload = load_json(
            "aws/lambdas/telemetry_processor/test_event.json"
        )

    def test_valid_payload_is_stored_and_status_is_updated(self):
        with (
            patch.object(telemetry.telemetry_table, "put_item") as put_item,
            patch.object(telemetry.status_table, "update_item") as update_item,
            patch.object(
                telemetry,
                "invoke_fis_processor",
                return_value={
                    "status_code": 200,
                    "body": {"message": "FIS processed in unit test"},
                },
            ) as invoke_fis,
        ):
            result = telemetry.lambda_handler(self.payload, None)

        self.assertEqual(result["statusCode"], 200)
        self.assertEqual(
            parse_lambda_body(result)["message"],
            "Telemetry processed successfully",
        )
        put_item.assert_called_once()
        update_item.assert_called_once()
        invoke_fis.assert_called_once()

    def test_invalid_payload_returns_400_without_writes(self):
        invalid_payload = dict(self.payload)
        invalid_payload.pop("battery")

        with (
            patch.object(telemetry.telemetry_table, "put_item") as put_item,
            patch.object(telemetry.status_table, "update_item") as update_item,
        ):
            result = telemetry.lambda_handler(invalid_payload, None)

        self.assertEqual(result["statusCode"], 400)
        self.assertIn(
            "Missing required field: battery",
            parse_lambda_body(result)["errors"],
        )
        put_item.assert_not_called()
        update_item.assert_not_called()

    def test_out_of_range_soc_is_rejected(self):
        invalid_payload = json.loads(json.dumps(self.payload))
        invalid_payload["battery"]["soc_percent"] = 101

        errors = telemetry.validate_payload(invalid_payload)

        self.assertIn("soc_percent must be <= 100", errors)

    def test_partial_status_update_uses_update_item(self):
        with patch.object(
            telemetry.status_table,
            "update_item",
        ) as update_item:
            telemetry.update_station_status(self.payload)

        kwargs = update_item.call_args.kwargs
        self.assertEqual(
            kwargs["Key"],
            {"station_id": "station_001"},
        )
        self.assertTrue(
            kwargs["UpdateExpression"].startswith("SET ")
        )


class CommandDispatcherTests(unittest.TestCase):
    def setUp(self):
        self.payload = load_json(
            "aws/lambdas/command_dispatcher/test_event.json"
        )

    def test_valid_command_completes_pending_to_sent_flow(self):
        with (
            patch.object(command, "save_command_to_dynamodb") as save_command,
            patch.object(command, "publish_command_to_iot") as publish_command,
            patch.object(command, "update_command_status") as update_status,
        ):
            result = command.lambda_handler(self.payload, None)

        self.assertEqual(result["statusCode"], 200)
        self.assertEqual(parse_lambda_body(result)["status"], "sent")
        save_command.assert_called_once()
        publish_command.assert_called_once()
        update_status.assert_called_once()

        update_kwargs = update_status.call_args.kwargs
        self.assertEqual(update_kwargs["status"], "sent")

        published_payload = publish_command.call_args.args[1]
        self.assertEqual(published_payload["status"], "sent")

    def test_invalid_command_returns_400_without_side_effects(self):
        invalid_payload = dict(self.payload)
        invalid_payload["command"] = "OPEN_DOOR"

        with (
            patch.object(command, "save_command_to_dynamodb") as save_command,
            patch.object(command, "publish_command_to_iot") as publish_command,
            patch.object(command, "update_command_status") as update_status,
        ):
            result = command.lambda_handler(invalid_payload, None)

        self.assertEqual(result["statusCode"], 400)
        save_command.assert_not_called()
        publish_command.assert_not_called()
        update_status.assert_not_called()

    def test_cloud_fis_command_is_skipped_when_not_required(self):
        payload = json.loads(json.dumps(self.payload))
        payload["source"] = "cloud_fis"
        payload.setdefault("parameters", {})["command_required"] = False

        with (
            patch.object(command, "save_command_to_dynamodb") as save_command,
            patch.object(command, "publish_command_to_iot") as publish_command,
            patch.object(command, "update_command_status") as update_status,
        ):
            result = command.lambda_handler(payload, None)

        self.assertEqual(result["statusCode"], 200)
        self.assertEqual(parse_lambda_body(result)["status"], "skipped")
        save_command.assert_not_called()
        publish_command.assert_not_called()
        update_status.assert_not_called()

    def test_publish_failure_marks_command_as_failed(self):
        with (
            patch.object(command, "save_command_to_dynamodb") as save_command,
            patch.object(
                command,
                "publish_command_to_iot",
                side_effect=RuntimeError("simulated publish failure"),
            ),
            patch.object(command, "update_command_status") as update_status,
        ):
            result = command.lambda_handler(self.payload, None)

        self.assertEqual(result["statusCode"], 500)
        save_command.assert_called_once()
        update_status.assert_called_once()

        update_kwargs = update_status.call_args.kwargs
        self.assertEqual(update_kwargs["status"], "failed")
        self.assertIn(
            "simulated publish failure",
            update_kwargs["error_message"],
        )


class DiagnosticsTests(unittest.TestCase):
    def setUp(self):
        self.status_payload = load_json(
            "aws/lambdas/diagnostics/test_event_status.json"
        )
        self.fault_payload = load_json(
            "aws/lambdas/diagnostics/test_event_fault.json"
        )
        self.ack_payload = load_json(
            "aws/lambdas/diagnostics/test_event_ack.json"
        )

    def test_status_message_updates_station_status(self):
        with patch.object(
            diagnostics,
            "update_station_status",
        ) as update_status:
            result = diagnostics.lambda_handler(
                self.status_payload,
                None,
            )

        self.assertEqual(result["statusCode"], 200)
        update_status.assert_called_once_with(self.status_payload)

    def test_fault_message_is_saved_and_updates_status(self):
        with (
            patch.object(diagnostics, "save_fault_event") as save_fault,
            patch.object(
                diagnostics,
                "update_station_status_from_fault",
            ) as update_status,
        ):
            result = diagnostics.lambda_handler(
                self.fault_payload,
                None,
            )

        self.assertEqual(result["statusCode"], 200)
        save_fault.assert_called_once_with(self.fault_payload)
        update_status.assert_called_once_with(self.fault_payload)

    def test_existing_command_ack_is_processed(self):
        with patch.object(
            diagnostics,
            "update_command_acknowledgement",
        ) as update_ack:
            result = diagnostics.lambda_handler(
                self.ack_payload,
                None,
            )

        self.assertEqual(result["statusCode"], 200)
        update_ack.assert_called_once_with(self.ack_payload)

    def test_nonexistent_command_ack_returns_404(self):
        with patch.object(
            diagnostics,
            "update_command_acknowledgement",
            side_effect=diagnostics.CommandNotFoundError(
                "simulated missing command"
            ),
        ):
            result = diagnostics.lambda_handler(
                self.ack_payload,
                None,
            )

        self.assertEqual(result["statusCode"], 404)
        self.assertIn(
            "simulated missing command",
            parse_lambda_body(result)["error"],
        )

    def test_unknown_message_type_is_rejected(self):
        payload = {
            "station_id": "station_001",
            "timestamp": "2026-07-09T21:00:00Z",
        }

        result = diagnostics.lambda_handler(payload, None)

        self.assertEqual(result["statusCode"], 400)


class FISProcessorTests(unittest.TestCase):
    def setUp(self):
        self.payload = load_json(
            "aws/lambdas/fis_processor/test_event.json"
        )

    def test_valid_fis_event_is_saved(self):
        with (
            patch.object(
                fis,
                "load_fis_state",
                return_value=None,
            ) as load_state,
            patch.object(
                fis,
                "invoke_command_dispatcher",
                return_value={"status_code": 200, "body": {"status": "sent"}},
            ) as invoke_dispatcher,
            patch.object(
                fis,
                "save_fis_state",
            ) as save_state,
            patch.object(
                fis,
                "save_fis_decision",
            ) as save_decision,
        ):
            result = fis.lambda_handler(self.payload, None)

        self.assertEqual(result["statusCode"], 200)
        body = parse_lambda_body(result)
        self.assertEqual(
            body["message"],
            "FIS decision evaluated successfully",
        )
        load_state.assert_called_once_with("station_001")
        invoke_dispatcher.assert_called_once()
        save_state.assert_called_once()
        saved_stabilization = save_state.call_args.kwargs["stabilization"]
        self.assertEqual(saved_stabilization["last_dispatched_mode"], "M4")
        save_decision.assert_called_once()

    def test_invalid_fis_event_returns_400_without_write(self):
        invalid_payload = json.loads(json.dumps(self.payload))
        invalid_payload["battery"]["soc_percent"] = 120

        with patch.object(
            fis,
            "save_fis_decision",
        ) as save_decision:
            result = fis.lambda_handler(invalid_payload, None)

        self.assertEqual(result["statusCode"], 400)
        save_decision.assert_not_called()

    def test_critical_lockout_generates_m0_lockout_request(self):
        payload = json.loads(json.dumps(self.payload))
        payload["fault_state"] = "critical_lockout"

        fis_result = fis.evaluate_fis(payload)
        command_request = fis.build_command_request(
            payload,
            fis_result,
        )

        self.assertEqual(
            fis_result["deterministic_validation"]["requested_mode"],
            "M0",
        )
        self.assertEqual(
            fis_result["deterministic_validation"]["blocked_reason"],
            "critical_lockout",
        )
        self.assertEqual(command_request["command"], "LOCKOUT")

    def test_critical_soc_forces_m0_and_blocks_outputs(self):
        result = fis.evaluate_deterministic_layer(
            fis_mode="M5",
            soc_percent=15.0,
            fault_state="normal",
            local_irradiance_wm2=800.0,
            weather_index=0.8,
        )

        self.assertEqual(result["requested_mode"], "M0")
        self.assertEqual(result["blocked_reason"], "critical_soc")
        self.assertTrue(result["outputs_blocked"])
        self.assertEqual(result["outputs_active"], 0)

    def test_low_battery_caps_mode_to_m1(self):
        result = fis.evaluate_deterministic_layer(
            fis_mode="M5",
            soc_percent=24.0,
            fault_state="normal",
            local_irradiance_wm2=800.0,
            weather_index=0.8,
        )

        self.assertEqual(result["requested_mode"], "M1")
        self.assertEqual(
            result["blocked_reason"],
            "low_battery_restriction",
        )
        self.assertTrue(result["tracking_blocked"])
        self.assertTrue(result["outputs_blocked"])
        self.assertEqual(result["outputs_active"], 0)

    def test_soc_above_25_does_not_apply_low_battery_cap(self):
        requested_mode = fis.apply_cloud_side_validation(
            fis_mode="M5",
            soc_percent=25.1,
            fault_state="normal",
        )

        self.assertEqual(requested_mode, "M5")

    def test_data_fault_caps_mode_to_m1(self):
        result = fis.evaluate_deterministic_layer(
            fis_mode="M4",
            soc_percent=90.0,
            fault_state="data_or_sensor_fault",
            local_irradiance_wm2=700.0,
            weather_index=0.8,
        )

        self.assertEqual(result["requested_mode"], "M1")
        self.assertEqual(result["fault_state_level"], 2)
        self.assertTrue(result["outputs_blocked"])
        self.assertEqual(result["outputs_active"], 0)

    def test_poor_tracking_reduces_m2_to_m1(self):
        result = fis.evaluate_deterministic_layer(
            fis_mode="M2",
            soc_percent=90.0,
            fault_state="normal",
            local_irradiance_wm2=100.0,
            weather_index=0.8,
        )

        self.assertEqual(result["requested_mode"], "M1")
        self.assertTrue(result["tracking_blocked"])
        self.assertFalse(result["tracking_allowed"])

    def test_poor_tracking_keeps_charging_mode_but_disables_tracking(self):
        result = fis.evaluate_deterministic_layer(
            fis_mode="M4",
            soc_percent=90.0,
            fault_state="normal",
            local_irradiance_wm2=100.0,
            weather_index=0.8,
        )

        self.assertEqual(result["requested_mode"], "M4")
        self.assertTrue(result["tracking_blocked"])
        self.assertFalse(result["tracking_allowed"])
        self.assertFalse(result["outputs_blocked"])
        self.assertEqual(result["outputs_active"], 2)

    def test_invalid_fault_state_is_rejected(self):
        payload = json.loads(json.dumps(self.payload))
        payload["fault_state"] = "unexpected_state"

        errors = fis.validate_input(payload)

        self.assertTrue(
            any("fault_state must be one of" in error for error in errors)
        )

    def test_first_valid_decision_initializes_applied_mode(self):
        deterministic = {
            "fault_state_level": 0,
            "outputs_blocked": False,
        }

        result = fis.stabilize_operating_mode(
            requested_mode="M4",
            deterministic=deterministic,
            previous_state=None,
            evaluation_time="2026-08-04T00:00:00Z",
        )

        self.assertEqual(result["applied_mode"], "M4")
        self.assertTrue(result["command_required"])
        self.assertEqual(
            result["decision_reason"],
            "initialized_from_first_valid_decision",
        )

    def test_candidate_waits_for_confirmation_and_dwell(self):
        deterministic = {
            "fault_state_level": 0,
            "outputs_blocked": False,
        }
        state = {
            "applied_mode": "M2",
            "candidate_mode": "M4",
            "candidate_since": "2026-08-04T00:00:00Z",
            "last_mode_change_at": "2026-08-04T00:00:00Z",
            "last_evaluation_at": "2026-08-04T00:00:00Z",
            "last_dispatched_mode": "M2",
            "last_dispatch_at": "2026-08-04T00:00:00Z",
        }

        waiting_confirmation = fis.stabilize_operating_mode(
            requested_mode="M4",
            deterministic=deterministic,
            previous_state=state,
            evaluation_time="2026-08-04T00:09:00Z",
        )
        self.assertEqual(waiting_confirmation["applied_mode"], "M2")
        self.assertFalse(waiting_confirmation["command_required"])
        self.assertEqual(
            waiting_confirmation["decision_reason"],
            "waiting_for_candidate_confirmation",
        )

        waiting_dwell = fis.stabilize_operating_mode(
            requested_mode="M4",
            deterministic=deterministic,
            previous_state=state,
            evaluation_time="2026-08-04T00:10:00Z",
        )
        self.assertEqual(waiting_dwell["applied_mode"], "M2")
        self.assertEqual(
            waiting_dwell["decision_reason"],
            "waiting_for_minimum_dwell",
        )

        applied = fis.stabilize_operating_mode(
            requested_mode="M4",
            deterministic=deterministic,
            previous_state=state,
            evaluation_time="2026-08-04T00:15:00Z",
        )
        self.assertEqual(applied["applied_mode"], "M4")
        self.assertTrue(applied["mode_changed"])
        self.assertTrue(applied["command_required"])

    def test_safety_reduction_is_applied_immediately(self):
        deterministic = {
            "fault_state_level": 1,
            "outputs_blocked": True,
        }
        state = {
            "applied_mode": "M4",
            "candidate_mode": "M5",
            "candidate_since": "2026-08-04T00:05:00Z",
            "last_mode_change_at": "2026-08-04T00:00:00Z",
            "last_evaluation_at": "2026-08-04T00:05:00Z",
            "last_dispatched_mode": "M4",
            "last_dispatch_at": "2026-08-04T00:00:00Z",
        }

        result = fis.stabilize_operating_mode(
            requested_mode="M1",
            deterministic=deterministic,
            previous_state=state,
            evaluation_time="2026-08-04T00:06:00Z",
        )

        self.assertEqual(result["applied_mode"], "M1")
        self.assertTrue(result["safety_reduction"])
        self.assertTrue(result["command_required"])
        self.assertEqual(
            result["decision_reason"],
            "safety_reduction_applied_immediately",
        )

    def test_candidate_is_cleared_when_request_returns_to_applied_mode(self):
        deterministic = {
            "fault_state_level": 0,
            "outputs_blocked": False,
        }
        state = {
            "applied_mode": "M2",
            "candidate_mode": "M4",
            "candidate_since": "2026-08-04T00:00:00Z",
            "last_mode_change_at": "2026-08-03T23:30:00Z",
            "last_evaluation_at": "2026-08-04T00:05:00Z",
            "last_dispatched_mode": "M2",
            "last_dispatch_at": "2026-08-04T00:00:00Z",
        }

        result = fis.stabilize_operating_mode(
            requested_mode="M2",
            deterministic=deterministic,
            previous_state=state,
            evaluation_time="2026-08-04T00:06:00Z",
        )

        self.assertEqual(result["applied_mode"], "M2")
        self.assertEqual(result["candidate_mode"], "M2")
        self.assertIsNone(result["candidate_since"])
        self.assertFalse(result["command_required"])

    def test_command_uses_stabilized_mode_not_unconfirmed_request(self):
        previous_state = {
            "applied_mode": "M2",
            "candidate_mode": "M2",
            "candidate_since": None,
            "last_mode_change_at": "2026-08-04T00:00:00Z",
            "last_evaluation_at": "2026-08-04T00:00:00Z",
            "last_dispatched_mode": "M2",
            "last_dispatch_at": "2026-08-04T00:00:00Z",
        }

        fis_result = fis.evaluate_fis(
            self.payload,
            previous_state=previous_state,
            evaluation_time="2026-08-04T00:01:00Z",
        )
        command_request = fis.build_command_request(
            self.payload,
            fis_result,
        )

        self.assertEqual(
            fis_result["deterministic_validation"]["requested_mode"],
            "M4",
        )
        self.assertEqual(
            fis_result["final_decision"]["operating_mode"],
            "M2",
        )
        self.assertFalse(
            fis_result["final_decision"]["command_required"],
        )
        self.assertEqual(command_request["command"], "ENABLE_TRACKING")
        self.assertEqual(
            command_request["parameters"]["applied_mode"],
            "M2",
        )

    def test_handler_does_not_dispatch_when_mode_was_already_sent(self):
        previous_state = {
            "applied_mode": "M4",
            "candidate_mode": "M4",
            "candidate_since": None,
            "last_mode_change_at": "2026-08-04T00:00:00Z",
            "last_evaluation_at": "2026-08-04T00:00:00Z",
            "last_dispatched_mode": "M4",
            "last_dispatch_at": "2026-08-04T00:00:00Z",
        }

        with (
            patch.object(fis, "load_fis_state", return_value=previous_state),
            patch.object(fis, "invoke_command_dispatcher") as invoke_dispatcher,
            patch.object(fis, "save_fis_state") as save_state,
            patch.object(fis, "save_fis_decision") as save_decision,
        ):
            result = fis.lambda_handler(self.payload, None)

        self.assertEqual(result["statusCode"], 200)
        body = parse_lambda_body(result)
        self.assertEqual(body["dispatch_result"]["status"], "not_required")
        invoke_dispatcher.assert_not_called()
        save_state.assert_called_once()
        save_decision.assert_called_once()

    def test_dispatch_failure_keeps_command_pending_for_retry(self):
        previous_state = {
            "applied_mode": "M4",
            "candidate_mode": "M4",
            "candidate_since": None,
            "last_mode_change_at": "2026-08-04T00:00:00Z",
            "last_evaluation_at": "2026-08-04T00:00:00Z",
            "last_dispatched_mode": "M2",
            "last_dispatch_at": "2026-08-04T00:00:00Z",
        }

        with (
            patch.object(fis, "load_fis_state", return_value=previous_state),
            patch.object(
                fis,
                "invoke_command_dispatcher",
                side_effect=RuntimeError("simulated dispatch failure"),
            ),
            patch.object(fis, "save_fis_state") as save_state,
            patch.object(fis, "save_fis_decision") as save_decision,
        ):
            result = fis.lambda_handler(self.payload, None)

        self.assertEqual(result["statusCode"], 502)
        saved_stabilization = save_state.call_args.kwargs["stabilization"]
        self.assertEqual(saved_stabilization["last_dispatched_mode"], "M2")
        save_decision.assert_called_once()

    def test_save_fis_state_updates_station_status(self):
        stabilization = {
            "requested_mode": "M4",
            "applied_mode": "M2",
            "candidate_mode": "M4",
            "candidate_since": "2026-08-04T00:01:00Z",
            "last_mode_change_at": "2026-08-04T00:00:00Z",
            "last_evaluation_at": "2026-08-04T00:01:00Z",
            "last_dispatched_mode": "M2",
            "last_dispatch_at": "2026-08-04T00:00:00Z",
        }

        with patch.object(
            fis.status_table,
            "update_item",
        ) as update_item:
            fis.save_fis_state("station_001", stabilization)

        update_item.assert_called_once()
        kwargs = update_item.call_args.kwargs
        self.assertEqual(
            kwargs["Key"],
            {"station_id": "station_001"},
        )
        self.assertIn("fis_state", kwargs["UpdateExpression"])

    def test_trapezoid_shoulders_include_endpoints(self):
        self.assertEqual(fis.trapmf(0, 0, 0, 20, 40), 1.0)
        self.assertEqual(
            fis.trapmf(100, 60, 80, 100, 100),
            1.0,
        )


class LocalCloudFlowTests(unittest.TestCase):
    def test_fis_output_builds_a_valid_command(self):
        payload = load_json(
            "aws/lambdas/fis_processor/test_event.json"
        )

        fis_errors = fis.validate_input(payload)
        self.assertEqual(fis_errors, [])

        fis_result = fis.evaluate_fis(payload)
        command_request = fis.build_command_request(
            payload,
            fis_result,
        )

        command_errors = command.validate_command(command_request)
        self.assertEqual(command_errors, [])

        command_payload = command.build_command_payload(
            command_request
        )

        self.assertEqual(
            command_payload["station_id"],
            payload["station_id"],
        )
        self.assertIn(
            command_payload["command"],
            command.ALLOWED_COMMANDS,
        )
        self.assertEqual(command_payload["status"], "pending")


if __name__ == "__main__":
    unittest.main(verbosity=2)
