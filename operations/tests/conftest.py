import os

os.environ["DBMAP_DATABASE_URL"] = "sqlite:///operations-tests.db"
os.environ["DBMAP_CORE_MONITOR_ENABLED"] = "false"
os.environ["DBMAP_PUBLIC_API_MONITOR_ENABLED"] = "false"
os.environ["DBMAP_PUBLIC_MQTT_MONITOR_ENABLED"] = "false"
