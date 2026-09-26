---
title: "Publishing Apps"
---

The App Store is an automatic index of public GitHub repositories tagged with
the topic `picodeck-app`. There is no registration or review: tag the repo,
publish a Release with one ZIP, and it is listed within 30 minutes.

The full contract, the `app.json` fields the index reads, and every
rejection reason are documented on the store itself:

- **Guide:** https://store.picodeck.net/publish
- **Why is my app not listed?** https://store.picodeck.net/status
- **Browse:** https://store.picodeck.net/
- **Catalog JSON the device reads:** https://store.picodeck.net/catalog.json

## Quick version

1. Public GitHub repo, not a fork, `app.json` at the root with `id`, `name`, `version`.
2. Add the topic `picodeck-app` under the repo's About settings.
3. Create a Release with exactly one `.zip` whose root holds `app.json` and `main.lua` or `main.elf`.
4. The index computes the SHA-256 itself. Do not publish a checksum file.

## Pointing a device at another indexer

    picocalc.sysconfig.set("store_url", "https://example.workers.dev/catalog.json")
    picocalc.sysconfig.save()

Remove the key to return to the default.
