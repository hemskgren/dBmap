from pathlib import Path

TEMPLATE = (
    Path(__file__).resolve().parents[2] / "nginx" / "default.conf.template"
).read_text(encoding="utf-8")


def test_nginx_local_health_is_direct_and_keeps_existing_health_proxy() -> None:
    local_health = TEMPLATE.index("/api/v1/nginx/health")
    legacy_health = TEMPLATE.index("(?<health_uri>/health)$")
    assert local_health < legacy_health
    local_location_start = TEMPLATE.rfind("    location", 0, local_health)
    local_location = TEMPLATE[local_location_start:legacy_health]
    assert "return 200 '{\"status\":\"ok\",\"service\":\"nginx\"}'" in local_location
    assert "proxy_pass" not in local_location
    assert "proxy_pass https://$hub_bootstrap_upstream$health_uri;" in TEMPLATE
