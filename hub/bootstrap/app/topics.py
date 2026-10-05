def topic_root(node_type: str, node_id: str) -> str:
    kind = "esp-output" if node_type == "output" else "esp-ear"
    slug = node_id.lower()
    return f"{kind}/local/{slug}"


def topics_for(node_type: str, node_id: str) -> dict[str, str]:
    root = topic_root(node_type, node_id)
    names = ("hello", "keepalive", "health", "status", "command", "ack", "config", "observation")
    return {name: f"{root}/{name}" for name in names}


def parse_observation_topic(topic: str) -> str | None:
    parts = topic.split("/")
    if len(parts) != 4 or parts[0] != "esp-ear" or parts[1] != "local" or parts[3] != "observation":
        return None
    slug = parts[2]
    if not slug or slug != slug.lower():
        return None
    return slug.upper()


def parse_node_id_from_topic(topic: str) -> str | None:
    parts = topic.split("/")
    if len(parts) < 4:
        return None
    slug = parts[2]
    return slug.upper()
