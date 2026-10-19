# Security Policy

## Supported version

Only the latest v0.2.x release is considered for security fixes. This is a security-focused MVP, not an enterprise remote-access product.

## Reporting a vulnerability

Use the repository owner's GitHub private vulnerability-reporting channel when it is available. Otherwise, contact the repository owner through the contact method on their GitHub profile. Do not attach session tokens, PINs, private keys, captured desktop images, or other sensitive data to a public issue.

Please include the affected version, build configuration, reproduction steps, and the security impact. A response time is not guaranteed.

## Deployment boundary

- Run the server only on a trusted IPv4 LAN. Do not expose either port through port forwarding, a VPN you do not control, or a public interface.
- Compare the full certificate fingerprint with the server console during the first pairing. The HTTP metadata endpoint is not authenticated.
- The six-digit PIN changes on every server start. Failed PIN attempts are rate-limited, but this does not make a weak PIN suitable for hostile networks.
- Session tokens are held in server memory and in Android's private app preferences. They are not a replacement for operating-system account security.
- The runtime private key is stored unencrypted beside the executable in the ignored `runtime/` directory. Protect the Windows account and that directory; never publish its contents.
- TLS 1.2 or newer protects the media/input connection after certificate pinning. No claim is made that every Android 8.0 device supports TLS 1.3.

See `docs/PROTOCOL.md` and the README's Security section for protocol and trust details.
