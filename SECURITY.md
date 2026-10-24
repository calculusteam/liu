# Security

## Reporting a vulnerability

Please report security problems privately, not in a public issue or
discussion.

Use GitHub's private reporting form: this repository's **Security** tab →
**Report a vulnerability**
(<https://github.com/calculusteam/liu/security/advisories/new>).

A report is most useful when it says:

- the version and build (`Liu --version`, or Settings › About) and the macOS
  version;
- what an attacker has to control — the bytes a program prints in a pane, a
  file Liu opens, a server it connects to, a page in the built-in browser;
- the smallest input or sequence of steps that shows the problem, and what
  happens.

## Scope

Liu.app and the two programs shipped beside it (`liu-notify`, `liu-history`),
the release packages, and the update feed.

The surfaces that take input Liu did not write are where a report is most
likely to be right:

- terminal output (escape sequences, titles, image protocols, links);
- files it reads: fonts, images, Markdown, other tools' session stores, model
  files, configuration;
- servers it talks to: SSH and SFTP hosts, the update feed, speech and
  translation endpoints the user configured;
- pages opened in the built-in browser, and text taken from them into a prompt;
- what Liu writes into other programs' settings (the agents' notify hooks).

Not a vulnerability by itself: what a program the user chose to run in a pane
can do with the user's own permissions. A terminal runs programs; the question
is whether something that should have stayed DATA became a command, a file
write or a network request.

## Supported versions

Fixes are made on the latest release; Liu updates itself from the signed feed
on this repository's releases, so a fixed version reaches installed copies
through the in-app updater.

## How untrusted input is tested

The parsers listed above (terminal output, Markdown, configuration, images,
fonts and colour emoji, model containers, agent-history stores, the notify
protocol, known_hosts lines, telnet negotiation, the browser's navigation
policy) and the SIMD kernels are fuzzed with libFuzzer under AddressSanitizer
and UndefinedBehaviorSanitizer, and every input that once failed is kept as a
regression case. Before a release is packaged, the stored cases are replayed,
each harness is fuzzed again and a leak check runs; a failure stops the
release. The harnesses and their corpora are maintained outside this
repository.
