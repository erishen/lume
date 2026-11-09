# Security Policy

## Reporting a vulnerability

Please **do not open a public issue** for a security problem. Use GitHub's
private vulnerability reporting ("Report a vulnerability" on the Security
tab) or a private Security Advisory, so a fix can be prepared before the
details become public. Include the affected version or commit, reproduction
steps, the impact you observed, and any suggested patch.

We will acknowledge the report, confirm or reject the finding with reasons,
and ship accepted fixes in the next release.

## Supported versions

The `main` branch and the most recent tagged release receive security fixes.
Older tags are not maintained; please reproduce against `main` or the latest
release before reporting.

## Deployment boundary (please read)

Lume is a local / self-hosted development tool, not a hardened
internet-facing server. Anyone who can reach the listening port can use the
agent chat and read files reachable to the process — see the Security
section of `README.md`. Bind to loopback, put an authenticated reverse proxy
in front if exposure is unavoidable, and never put real credentials in a
`.lume` file or a committed config. `LLM_API_KEY` and similar values belong
in the environment, and `.env.example` exists for documenting key names.

Out of scope: findings that require an attacker to already have a local
shell account, or that only affect a demo/example deliberately run with
elevated flags such as `LUME_FS_ROOT`.

## Encryption at rest and personal data (policy, 2026-11-09)

Verdict from lume-core `docs/PII_AT_REST.md` (research) and `TODO.md` P18:
**encryption at rest belongs to the storage layer, with an explicit
deployment boundary.** The language has no symmetric `encrypt`/`decrypt`
builtin, and adding one is deliberately not the default answer — see the
research document for the cost model and the still-open alternatives.

- **Pure `lume-core` / `bridge_serve` services backed by the built-in SQLite
  must not collect personal data** (names, phone numbers, contact details).
  Neither tree ships transparent SQLite encryption (no SQLCipher / SEE),
  and SQLCipher would add an OpenSSL dependency to the server link that
  `agent-httpd/src/agent/llm.c:16` explicitly avoids. A copied `.db` file
  is plaintext; whole-volume encryption is the only mitigation and is not
  equivalent to field-level protection.
- PII-at-rest is considered **covered only** for an `agent-httpd`
  deployment against a remote MySQL/PostgreSQL instance where the operator
  has explicitly enabled tablespace/volume encryption **and** log hygiene.
  TDE protects data files only — prepared-statement values can still reach
  general/slow logs, binlogs, backups, memory and read replicas.
- Any privacy notice claiming "encrypted storage" must match the shape
  actually deployed. A policy statement alone is not a control; the
  operator assertion above is part of the deployment checklist.
- If a future service needs field-level PII protection, prefer a
  **framework-layer** hook in agent-httpd (`db_layer.c` entry points) or an
  explicit tool over a new language builtin. Mandatory consent, notices in
  the response **body** (never non-ASCII header values), masking and
  retention erasure are handler conventions, not new primitives.
- Secrets stay in the environment. `env()` masks credential names from DSL
  scripts; runtime C code reads them with `getenv` directly. Never put a
  key in a `.lume` file or a committed config.

## Dependency handling

System libraries (OpenSSL, SQLite and friends) are linked at build time, not
vendored: update them through the OS or package manager. Vendored
third-party sources are listed in `NOTICE`.
