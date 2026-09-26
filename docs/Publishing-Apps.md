---
title: "Publishing Apps"
---

The App Store is an automatic index of public GitHub repositories tagged with
the topic `picos-app`. There is no registration or review: tag the repo,
publish a Release with one ZIP, and it is listed within 30 minutes.

The full contract, the `app.json` fields the index reads, and every
rejection reason are documented on the store itself:

- **Guide:** https://picos.jeffory.dev/publish
- **Why is my app not listed?** https://picos.jeffory.dev/status
- **Browse:** https://picos.jeffory.dev/
- **Catalog JSON the device reads:** https://picos.jeffory.dev/catalog.json

## Quick version

1. Public GitHub repo, not a fork, `app.json` at the root with `id`, `name`, `version`.
2. Add the topic `picos-app` under the repo's About settings.
3. Create a Release with exactly one `.zip` whose root holds `app.json` and `main.lua` or `main.elf`.
4. The index computes the SHA-256 itself. Do not publish a checksum file.

## Pointing a device at another indexer

    picocalc.sysconfig.set("store_url", "https://example.workers.dev/catalog.json")
    picocalc.sysconfig.save()

Remove the key to return to the default.
