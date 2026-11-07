from pydantic import Field, model_validator
from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_prefix="DBMAP_", extra="ignore")

    database_url: str = "sqlite:////data/analysis.db"
    admin_token: str = ""
    viewer_token: str = ""
    opa_decision_url: str = "https://opa:8181/v1/data/dbmap/authz/allow"
    opa_ca_file: str = "/certs/ca.crt"
    opa_timeout_s: float = 2.0

    # Scratch object lifecycle (ephemeral only; persistent analysis data is separate).
    analysis_scratch_default_ttl_s: int = Field(default=3600, ge=1, le=7200)
    analysis_scratch_max_ttl_s: int = Field(default=7200, ge=1, le=7200)
    analysis_scratch_max_payload_bytes: int = Field(
        default=1_048_576, ge=1024, le=16_777_216
    )
    analysis_scratch_max_objects: int = Field(default=1000, ge=1, le=100_000)

    @model_validator(mode="after")
    def default_ttl_within_max(self) -> "Settings":
        if self.analysis_scratch_default_ttl_s > self.analysis_scratch_max_ttl_s:
            raise ValueError(
                "analysis_scratch_default_ttl_s must be <= analysis_scratch_max_ttl_s"
            )
        return self


settings = Settings()
