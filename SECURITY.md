# Security Policy

dBmap is an open-source project under active development. Security features
and deployment guidance may change as the system evolves.

## Reporting a vulnerability

Please report suspected security vulnerabilities privately. Do not create a
public issue or pull request containing exploit details, credentials, private
keys, personal data, or other sensitive information.

If GitHub's private vulnerability reporting is enabled for this repository,
use **Security → Advisories → Report a vulnerability**. If it is unavailable,
contact a repository maintainer privately through their GitHub profile and
include only the information needed to reproduce and assess the issue.

Please include the affected component and version or commit, impact, and
reproduction steps when safe to do so. Avoid sending real credentials, live
private keys, or personal data; use redacted or synthetic examples.

There is no guaranteed response or remediation timeline at this stage. The
maintainers will work with the reporter to validate and address confirmed
issues before public disclosure where practical.

## Handling secrets

Never commit real Wi-Fi or MQTT credentials, bootstrap or admin tokens,
private CA keys, device private keys, or certificates containing real
identity data. Do not include them in logs, screenshots, issue reports, or
pull requests. Revoke or rotate any secret that may have been exposed.

The current local MQTT setup uses TLS with server-certificate validation and
per-node username/password credentials; it does not require client
certificates (mTLS). Prototype devices do not currently have flash encryption
enabled, so credentials stored in device NVS should not be treated as
protected against physical extraction.
