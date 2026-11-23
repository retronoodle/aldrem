# Security Policy

## Supported versions

Aldrem is pre-1.0. Security fixes land on `main`; there are no maintained release branches yet. If you run a build, please rebuild from the latest `main` before reporting.

## Reporting a vulnerability

Please **do not open a public issue** for a security problem.

Report it privately, either:

- through GitHub's [private vulnerability reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability) (Security → "Report a vulnerability" on the repository), or
- by email to **tsmonaghan@gmail.com** with `Aldrem security` in the subject.

Useful details: the version or commit, your platform, the file that triggers the problem if it's a malformed-media bug, and steps to reproduce. A crash sample you're comfortable sharing helps a lot — the decoders parse untrusted files, so out-of-bounds reads on crafted input are the likeliest class of bug.

## What to expect

- An acknowledgement within a few days.
- A fix on `main` and, where possible, a note in the release. Credit is given on request.
- Please give me a reasonable window to ship a fix before any public disclosure.

## Scope

In scope: the player itself, the vendored decoders as wired into `src/` (e.g. a malformed `.mp3`/`.flac`/`.ogg`/`.aiff` causing a crash, memory corruption, or arbitrary file access), `.m3u` playlist parsing, the file browser, and the internet radio network surface: the Radio Browser JSON parser, the libcurl transfer code in `src/net.c`, and the live MP3/Ogg/AAC stream decoders. The network surface is limited to `http` and `https` (redirects included, at most 5), sends no cookies or credentials, and caps response sizes. A malicious station or mirror that can crash the player or read or write local files is in scope.

Out of scope: bugs in vendored upstream projects that are best fixed upstream (please also tell the upstream project), issues that need a locally modified binary, and anything requiring an already-compromised machine.
