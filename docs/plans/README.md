# Development plans archive

These documents record how features were proposed, split into milestones and developed. They may describe
superseded interfaces, old firmware limits or work that has since shipped. They are retained as engineering history,
not as installation instructions or a list of current promises.

*Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE). Written with the assistance of an AI
(Anthropic's Claude); reviewed by a human, but it may still contain mistakes.*

| Plan | Subject |
|---|---|
| [Local web app](1.1.0-local-first.md) | Moving everyday operation into a web app stored on the feeder |
| [Appliance setup](2.0.0-appliance.md) | Prebuilt images, runtime setup, credentials and browser updates |
| [Mechanisms and drive selection](drive-decoupling.md) | Separating motion from feeder policy and adding the direct-drive disc |
| [Integration scopes](2.2.0-scopes.md) | Keeping configuration in the web app and defining HA/MQTT access |
| [Configuration loose ends](2.3.0-loose-ends.md) | Wi-Fi scanning/recovery, key clearing and read-only MQTT guard settings |
| [NimBLE](nimble.md) | Replacing the Bluetooth stack while retaining the BLE wire protocol |
| [HTTPS and TLS](https-tls.md) | Certificates, MQTT TLS, ACME and their web controls |

Use the [README](../../README.md) and [user guides](../../README.md#find-the-right-guide) for current operation.
[DESIGN.md](../../DESIGN.md) is authoritative for current behavior and decisions; [CHANGELOG.md](../../CHANGELOG.md)
and [BENCH.md](../BENCH.md) record implementation and hardware results.
