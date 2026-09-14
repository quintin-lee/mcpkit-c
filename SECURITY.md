# Security Policy

## Supported versions

| Version | Supported |
| ------- | --------- |
| 0.1.x   | Yes       |

## Reporting

Report vulnerabilities through GitHub Security Advisories on this
repository once it is public. Include reproduction steps, affected
commit, and impact assessment.

Do not open public issues for unpatched vulnerabilities.

## Scope priorities

Transport input (stdio/HTTP framing), JSON parsing, JSON Schema
validation of tool arguments, and MCP Apps CSP enforcement are
security-critical paths and require negative tests for every change.
