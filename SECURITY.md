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

## Dependency handling

System libraries (OpenSSL, SQLite and friends) are linked at build time, not
vendored: update them through the OS or package manager. Vendored
third-party sources are listed in `NOTICE`.
