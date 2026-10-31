"""Hub MQTT listener must ingest Ear status (OTA) as well as Output status."""

from app import mqtt_listener


def test_on_connect_subscribes_to_ear_and_output_status() -> None:
    subscribed: list[tuple[str, int]] = []

    class FakeClient:
        def subscribe(self, topic, qos=0):
            subscribed.append((topic, qos))

    mqtt_listener._on_connect(
        FakeClient(), None, None, type("RC", (), {"is_failure": False})()
    )

    topics = {topic for topic, _qos in subscribed}
    assert "esp-output/local/+/status" in topics
    assert "esp-ear/local/+/status" in topics
    assert "esp-ear/local/+/hello" in topics
    assert "esp-ear/local/+/observation" in topics

    status_qos = {qos for topic, qos in subscribed if topic.endswith("/status")}
    assert status_qos == {1}
