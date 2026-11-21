"""Unit tests for Hub identity / desired-state helpers."""

from app.ids import pending_configuration_change


def test_pending_empty_desired_classifier_is_not_a_mismatch() -> None:
    reported = {
        "firmware_version": "0.8.36",
        "config_version": 0,
        "calibration_version": 0,
        "classifier_version": "1.1",
    }
    desired = {
        "firmware_version": "0.8.36",
        "config_version": 0,
        "calibration_version": 0,
        "classifier_version": "",
    }
    assert pending_configuration_change(reported, desired) is False


def test_pending_whitespace_desired_string_is_no_requirement() -> None:
    assert (
        pending_configuration_change(
            {"classifier_version": "1.1", "firmware_version": "a"},
            {"classifier_version": "   ", "firmware_version": "a"},
        )
        is False
    )


def test_pending_explicit_desired_classifier_mismatch() -> None:
    assert (
        pending_configuration_change(
            {
                "firmware_version": "0.8.36",
                "config_version": 0,
                "calibration_version": 0,
                "classifier_version": "1.1",
            },
            {
                "firmware_version": "0.8.36",
                "config_version": 0,
                "calibration_version": 0,
                "classifier_version": "2.0",
            },
        )
        is True
    )


def test_pending_explicit_desired_classifier_match() -> None:
    assert (
        pending_configuration_change(
            {"classifier_version": "1.1", "firmware_version": "0.8.36"},
            {"classifier_version": "1.1", "firmware_version": "0.8.36"},
        )
        is False
    )


def test_pending_explicit_config_version_mismatch_still_true() -> None:
    assert (
        pending_configuration_change(
            {
                "firmware_version": "0.1.0",
                "config_version": 0,
                "calibration_version": 0,
                "classifier_version": "",
            },
            {
                "firmware_version": "0.1.0",
                "config_version": 18,
                "calibration_version": 0,
                "classifier_version": "",
            },
        )
        is True
    )


def test_pending_zero_config_version_is_still_compared() -> None:
    """Integer 0 is an explicit revision counter value, not 'Not set'."""
    assert (
        pending_configuration_change(
            {
                "firmware_version": "0.1.0",
                "config_version": 5,
                "calibration_version": 0,
                "classifier_version": "",
            },
            {
                "firmware_version": "0.1.0",
                "config_version": 0,
                "calibration_version": 0,
                "classifier_version": "",
            },
        )
        is True
    )
